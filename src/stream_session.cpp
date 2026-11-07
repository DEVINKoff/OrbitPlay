// SPDX-License-Identifier: AGPL-3.0-only
#include "stream_session.h"
#include "audio.h"
#include "recorder.h"
#include "log.h"

#include <chiaki/time.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d11va.h>
#endif
}

#include <algorithm>
#include <cmath>
#include <cstring>

static const int kMicFrames = 480; // 10 ms @ 48 kHz
static const uint16_t kHapticRumbleMin = 100;

StreamSession::StreamSession()
{
#ifdef _WIN32
	frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#endif
}

StreamSession::~StreamSession()
{
	Stop();
#ifdef _WIN32
	if(frame_event)
		CloseHandle(frame_event);
#endif
}

#ifdef _WIN32
static AVBufferRef *CreateD3D11VAContext(ID3D11Device *device, ID3D11DeviceContext *ctx)
{
	AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
	if(!ref)
		return nullptr;
	auto *hw = reinterpret_cast<AVHWDeviceContext *>(ref->data);
	auto *d3d = reinterpret_cast<AVD3D11VADeviceContext *>(hw->hwctx);
	// FFmpeg releases these when the context is freed
	device->AddRef();
	ctx->AddRef();
	d3d->device = device;
	d3d->device_context = ctx;
	if(av_hwdevice_ctx_init(ref) < 0)
	{
		av_buffer_unref(&ref);
		return nullptr;
	}
	return ref;
}
#endif

static ChiakiVideoResolutionPreset ResolutionPreset(int res)
{
	switch(res)
	{
		case 360: return CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
		case 540: return CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
		case 720: return CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
		default: return CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
	}
}

bool StreamSession::Start(const StreamConfig &cfg, void *hw_device, void *hw_context, Audio *aud, std::string *error)
{
	Stop();
	config = cfg;
	audio = aud;
	host_str = cfg.address;
	haptics_gain = cfg.haptics_gain;
	connected = false;
	mic_muted = true;
	mic_connected = false;
	total_lost = 0;
	window_start_ms = chiaki_time_now_monotonic_ms();
	bytes_window = frames_window = 0;

	ChiakiLog *log = Log::Get().Chiaki();

	ChiakiCodec codec = CHIAKI_CODEC_H264;
	if(cfg.codec == VideoCodec::H265 || cfg.hdr)
		codec = cfg.hdr ? CHIAKI_CODEC_H265_HDR : CHIAKI_CODEC_H265;

	// Video decoder: D3D11VA on the renderer's device so frames never leave the GPU.
#ifdef _WIN32
	hw_device_ctx = CreateD3D11VAContext((ID3D11Device *)hw_device, (ID3D11DeviceContext *)hw_context);
	if(!hw_device_ctx)
		LOGW("Could not set up D3D11VA, falling back to software decoding");
	const char *hw_name = hw_device_ctx ? "d3d11va" : nullptr;
#elif defined(__APPLE__)
	// VideoToolbox: chiaki creates the device; frames arrive as CVPixelBuffers (zero copy into Metal)
	(void)hw_device;
	(void)hw_context;
	const char *hw_name = "videotoolbox";
#else
	(void)hw_device;
	(void)hw_context;
	const char *hw_name = nullptr;
#endif
	ChiakiErrorCode err = chiaki_ffmpeg_decoder_init(&decoder, log, codec, (unsigned)cfg.fps,
			hw_name, hw_device_ctx, FrameAvailableCb, this);
	if(err != CHIAKI_ERR_SUCCESS && hw_name)
	{
		LOGW("Hardware decoder failed to start, trying software decoding");
		av_buffer_unref(&hw_device_ctx);
		err = chiaki_ffmpeg_decoder_init(&decoder, log, codec, (unsigned)cfg.fps, nullptr, nullptr, FrameAvailableCb, this);
	}
	if(err != CHIAKI_ERR_SUCCESS)
	{
		*error = std::string("Video decoder failed: ") + chiaki_error_string(err);
		Stop();
		return false;
	}
	decoder_inited = true;

	chiaki_opus_decoder_init(&opus_decoder, log);
	chiaki_opus_encoder_init(&opus_encoder, log);
	opus_inited = true;

	ChiakiConnectInfo info = {};
	info.ps5 = cfg.host.ps5;
	info.host = host_str.c_str();
	memcpy(info.regist_key, cfg.host.regist_key, sizeof(info.regist_key));
	memcpy(info.morning, cfg.host.rp_key, sizeof(info.morning));
	chiaki_connect_video_profile_preset(&info.video_profile, ResolutionPreset(cfg.resolution),
			cfg.fps >= 60 ? CHIAKI_VIDEO_FPS_PRESET_60 : CHIAKI_VIDEO_FPS_PRESET_30);
	if(cfg.bitrate_kbps > 0)
		info.video_profile.bitrate = (unsigned)cfg.bitrate_kbps;
	info.video_profile.codec = codec;
	info.video_profile_auto_downgrade = true;
	info.enable_keyboard = false;
	info.enable_dualsense = cfg.dualsense;
	info.audio_video_disabled = CHIAKI_NONE_DISABLED;
	info.auto_regist = false;
	info.holepunch_session = nullptr;
	info.rudp_sock = nullptr;
	info.packet_loss_max = 0.05;
	info.enable_idr_on_fec_failure = true;
	std::vector<uint8_t> account = Base64Decode(cfg.psn_account_id);
	if(account.size() == CHIAKI_PSN_ACCOUNT_ID_SIZE)
		memcpy(info.psn_account_id, account.data(), CHIAKI_PSN_ACCOUNT_ID_SIZE);

	err = chiaki_session_init(&session, &info, log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		*error = std::string("Session init failed: ") + chiaki_error_string(err);
		Stop();
		return false;
	}
	session_inited = true;

	ChiakiCtrlDisplaySink display_sink = {};
	display_sink.user = this;
	display_sink.cantdisplay_cb = CantDisplayCb;
	chiaki_session_ctrl_set_display_sink(&session, &display_sink);

	chiaki_opus_decoder_set_cb(&opus_decoder, AudioSettingsCb, AudioFrameCb, this);
	ChiakiAudioSink audio_sink;
	chiaki_opus_decoder_get_sink(&opus_decoder, &audio_sink);
	chiaki_session_set_audio_sink(&session, &audio_sink);

	ChiakiAudioHeader mic_header;
	chiaki_audio_header_set(&mic_header, 2, 16, kMicFrames * 100, kMicFrames);
	chiaki_opus_encoder_header(&mic_header, &opus_encoder, &session);

	if(cfg.dualsense)
	{
		ChiakiAudioSink haptics_sink = {};
		haptics_sink.user = this;
		haptics_sink.frame_cb = HapticsFrameCb;
		chiaki_session_set_haptics_sink(&session, &haptics_sink);
	}

	chiaki_session_set_video_sample_cb(&session, VideoSampleCb, this);
	chiaki_session_set_event_cb(&session, EventCb, this);

	err = chiaki_session_start(&session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		*error = std::string("Could not start the session: ") + chiaki_error_string(err);
		Stop();
		return false;
	}
	running = true;
	LOGI("Connecting to %s: %dp%d, %s, %u kbps", host_str.c_str(), cfg.resolution, cfg.fps,
			chiaki_codec_name(codec), info.video_profile.bitrate);
	return true;
}

void StreamSession::Stop()
{
	if(session_inited)
	{
		if(running)
		{
			chiaki_session_stop(&session);
			chiaki_session_join(&session);
		}
		if(audio)
			audio->CloseMic();
		chiaki_session_fini(&session);
		session_inited = false;
	}
	running = false;
	connected = false;
	if(opus_inited)
	{
		chiaki_opus_encoder_fini(&opus_encoder);
		chiaki_opus_decoder_fini(&opus_decoder);
		opus_inited = false;
	}
	if(decoder_inited)
	{
		chiaki_ffmpeg_decoder_fini(&decoder);
		decoder_inited = false;
	}
	if(hw_device_ctx)
		av_buffer_unref(&hw_device_ctx);
	if(audio)
		audio->CloseHaptics();
}

AVFrame *StreamSession::PullFrame()
{
	if(!decoder_inited)
		return nullptr;
	int32_t lost = 0;
	ChiakiFfmpegFrame f = chiaki_ffmpeg_decoder_pull_frame(&decoder, &lost);
	if(lost > 0)
	{
		std::lock_guard<std::mutex> lock(stats_mutex);
		total_lost += (uint64_t)lost;
	}
	if(f.frame)
	{
		std::lock_guard<std::mutex> lock(stats_mutex);
		frames_window++;
	}
	return f.frame;
}

void StreamSession::SetControllerState(const ChiakiControllerState &state)
{
	if(!running || !connected)
		return;
	ChiakiControllerState s = state;
	chiaki_session_set_controller_state(&session, &s);
}

std::vector<SessionEvent> StreamSession::TakeEvents()
{
	std::lock_guard<std::mutex> lock(events_mutex);
	std::vector<SessionEvent> out(events.begin(), events.end());
	events.clear();
	return out;
}

void StreamSession::PushEvent(const SessionEvent &e)
{
	std::lock_guard<std::mutex> lock(events_mutex);
	// Rumble events can flood in; keep only the most recent one queued.
	if(e.type == SessionEvent::HapticRumble || e.type == SessionEvent::Rumble)
	{
		for(auto &q : events)
			if(q.type == e.type)
			{
				q = e;
				return;
			}
	}
	events.push_back(e);
}

void StreamSession::SetLoginPin(const std::string &pin)
{
	if(session_inited)
		chiaki_session_set_login_pin(&session, (const uint8_t *)pin.data(), pin.size());
}

void StreamSession::KeyboardSetText(const std::string &text)
{
	if(connected)
		chiaki_session_keyboard_set_text(&session, text.c_str());
}

void StreamSession::KeyboardAccept()
{
	if(connected)
		chiaki_session_keyboard_accept(&session);
}

void StreamSession::KeyboardReject()
{
	if(connected)
		chiaki_session_keyboard_reject(&session);
}

void StreamSession::GotoBed()
{
	if(connected)
		chiaki_session_goto_bed(&session);
}

void StreamSession::GoHome()
{
	if(connected)
		chiaki_session_go_home(&session);
}

void StreamSession::RequestKeyframe()
{
	if(connected)
		chiaki_session_request_idr(&session);
}

void StreamSession::SetMicMuted(bool muted)
{
	if(!connected || !audio)
		return;
	if(!muted && !mic_connected)
	{
		bool ok = audio->OpenMic(kMicFrames, [this](int16_t *samples, size_t) {
			if(!mic_muted)
				chiaki_opus_encoder_frame(samples, &opus_encoder);
		});
		if(!ok)
			return;
		chiaki_session_connect_microphone(&session);
		mic_connected = true;
	}
	// chiaki's argument is "currently muted" -> true means unmute
	chiaki_session_toggle_microphone(&session, !muted);
	mic_muted = muted;
	LOGI("Microphone %s", muted ? "muted" : "on");
}

StreamStats StreamSession::Stats()
{
	StreamStats s;
	uint64_t now = chiaki_time_now_monotonic_ms();
	{
		std::lock_guard<std::mutex> lock(stats_mutex);
		uint64_t dt = now - window_start_ms;
		if(dt >= 1000)
		{
			mbps = bytes_window * 8.0 / 1000.0 / (double)dt;
			decode_fps = frames_window * 1000.0 / (double)dt;
			bytes_window = frames_window = 0;
			window_start_ms = now;
		}
		s.mbps = mbps;
		s.decode_fps = decode_fps;
		s.frames_lost = total_lost;
	}
	if(session_inited)
	{
		s.rtt_ms = session.rtt_us / 1000.0;
		s.width = (int)session.connect_info.video_profile.width;
		s.height = (int)session.connect_info.video_profile.height;
		s.codec = chiaki_codec_name(session.connect_info.video_profile.codec);
	}
	return s;
}

// ---------------------------------------------------------------------------
// chiaki callbacks (called on chiaki's threads)

void StreamSession::EventCb(ChiakiEvent *event, void *user)
{
	auto *self = static_cast<StreamSession *>(user);
	SessionEvent e;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			self->connected = true;
			e.type = SessionEvent::Connected;
			LOGI("Connected");
			break;
		case CHIAKI_EVENT_QUIT:
			self->connected = false;
			e.type = SessionEvent::Quit;
			e.flag = chiaki_quit_reason_is_error(event->quit.reason);
			e.text = chiaki_quit_reason_string(event->quit.reason);
			if(event->quit.reason_str)
				e.text += std::string(" (") + event->quit.reason_str + ")";
			LOGI("Session ended: %s", e.text.c_str());
			break;
		case CHIAKI_EVENT_RUMBLE:
			e.type = SessionEvent::Rumble;
			e.a = event->rumble.left;
			e.b = event->rumble.right;
			break;
		case CHIAKI_EVENT_TRIGGER_EFFECTS:
			e.type = SessionEvent::TriggerEffects;
			e.a = event->trigger_effects.type_left;
			e.b = event->trigger_effects.type_right;
			memcpy(e.left, event->trigger_effects.left, 10);
			memcpy(e.right, event->trigger_effects.right, 10);
			break;
		case CHIAKI_EVENT_LED_COLOR:
			e.type = SessionEvent::LedColor;
			memcpy(e.rgb, event->led_state, 3);
			break;
		case CHIAKI_EVENT_PLAYER_INDEX:
			e.type = SessionEvent::PlayerIndex;
			e.a = event->player_index;
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			e.type = SessionEvent::LoginPinRequest;
			e.flag = event->login_pin_request.pin_incorrect;
			break;
		case CHIAKI_EVENT_KEYBOARD_OPEN:
			e.type = SessionEvent::KeyboardOpen;
			if(event->keyboard.text_str)
				e.text = event->keyboard.text_str;
			break;
		case CHIAKI_EVENT_KEYBOARD_TEXT_CHANGE:
			return;
		case CHIAKI_EVENT_KEYBOARD_REMOTE_CLOSE:
			e.type = SessionEvent::KeyboardClose;
			break;
		case CHIAKI_EVENT_MOTION_RESET:
			e.type = SessionEvent::MotionReset;
			break;
		case CHIAKI_EVENT_HAPTIC_INTENSITY:
		case CHIAKI_EVENT_TRIGGER_INTENSITY:
			e.type = SessionEvent::Intensity;
			e.flag = event->type == CHIAKI_EVENT_TRIGGER_INTENSITY;
			e.a = (uint8_t)event->intensity;
			break;
		case CHIAKI_EVENT_NICKNAME_RECEIVED:
			e.type = SessionEvent::Nickname;
			e.text = std::string(event->server_nickname, strnlen(event->server_nickname, sizeof(event->server_nickname)));
			break;
		default:
			return;
	}
	self->PushEvent(e);
}

bool StreamSession::VideoSampleCb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user)
{
	auto *self = static_cast<StreamSession *>(user);
	{
		std::lock_guard<std::mutex> lock(self->stats_mutex);
		self->bytes_window += buf_size;
	}
	if(Recorder *r = self->recorder)
		r->OnVideoPacket(buf, buf_size);
	return chiaki_ffmpeg_decoder_video_sample_cb(buf, buf_size, frames_lost, frame_recovered, &self->decoder);
}

void StreamSession::FrameAvailableCb(ChiakiFfmpegDecoder *, void *user)
{
	auto *self = static_cast<StreamSession *>(user);
	self->decoded_frames++;
	self->last_frame_us = chiaki_time_now_monotonic_us();
#ifdef _WIN32
	SetEvent(self->frame_event);
#endif
}

void StreamSession::AudioSettingsCb(uint32_t channels, uint32_t rate, void *user)
{
	auto *self = static_cast<StreamSession *>(user);
	self->audio_channels = (int)channels;
	self->audio_rate = (int)rate;
	if(self->audio)
		self->audio->OpenGame((int)channels, (int)rate);
}

void StreamSession::AudioFrameCb(int16_t *buf, size_t samples_count, void *user)
{
	auto *self = static_cast<StreamSession *>(user);
	if(self->audio)
		self->audio->PushGame(buf, samples_count);
	if(Recorder *r = self->recorder)
		r->OnAudio(buf, samples_count, self->audio_channels, self->audio_rate);
}

void StreamSession::HapticsFrameCb(uint8_t *buf, size_t buf_size, void *user)
{
	auto *self = static_cast<StreamSession *>(user);
	if(buf_size == 0 || buf_size % 4 != 0)
		return;
	size_t frames = buf_size / 4;
	const int16_t *samples = reinterpret_cast<const int16_t *>(buf);

	if(self->audio && self->audio->HapticsActive())
	{
		self->audio->PushHaptics(samples, frames, self->haptics_gain);
		return;
	}

	// No DualSense haptics device: turn the haptic waveform into classic rumble.
	uint32_t sum_l = 0, sum_r = 0;
	for(size_t i = 0; i < frames; i++)
	{
		sum_l += (uint32_t)std::abs((int)samples[i * 2]) * 2;
		sum_r += (uint32_t)std::abs((int)samples[i * 2 + 1]) * 2;
	}
	uint32_t l = sum_l / frames, r = sum_r / frames;
	l = l > kHapticRumbleMin ? l : 0;
	r = r > kHapticRumbleMin ? r : 0;
	SessionEvent e;
	e.type = SessionEvent::HapticRumble;
	e.strength = (uint16_t)std::min<uint32_t>(std::max(l, r), 0xffff);
	self->PushEvent(e);
}

void StreamSession::CantDisplayCb(void *, bool cant_display)
{
	if(cant_display)
		LOGW("The console reports this content can't be shown over Remote Play (e.g. protected video)");
}
