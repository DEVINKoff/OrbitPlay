// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "settings.h"

#include <chiaki/session.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/opusencoder.h>
#include <chiaki/ffmpegdecoder.h>

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct AVFrame;
class Audio;

struct StreamConfig
{
	RegisteredHost host;
	std::string address;
	std::string psn_account_id;
	int resolution = 1080;
	int fps = 60;
	int bitrate_kbps = 0;
	VideoCodec codec = VideoCodec::H265;
	bool hdr = false;
	bool dualsense = true;
	float haptics_gain = 1.0f;
};

// Events delivered from chiaki's threads to the UI thread.
struct SessionEvent
{
	enum Type { Connected, Quit, Rumble, TriggerEffects, LedColor, PlayerIndex, LoginPinRequest,
		KeyboardOpen, KeyboardClose, MotionReset, HapticRumble, Intensity, Nickname };
	Type type;
	uint8_t a = 0, b = 0;                 // rumble left/right, trigger types, player index, ...
	uint8_t left[10] = {}, right[10] = {}; // trigger effect params
	uint8_t rgb[3] = {};
	uint16_t strength = 0;                // haptic-derived rumble
	bool flag = false;                    // pin_incorrect / quit is error
	std::string text;
};

struct StreamStats
{
	double rtt_ms = 0.0;
	double mbps = 0.0;            // received video bitrate
	double decode_fps = 0.0;
	uint64_t frames_lost = 0;
	int width = 0, height = 0;
	std::string codec;
};

class StreamSession
{
public:
	StreamSession();
	~StreamSession();

	bool Start(const StreamConfig &config, ID3D11Device *device, ID3D11DeviceContext *ctx, Audio *audio, std::string *error);
	void Stop();

	bool Running() const { return running; }
	bool Connected() const { return connected; }

	// Newest decoded frame since the last call, or nullptr. Free with av_frame_free.
	AVFrame *PullFrame();
	// When the newest frame finished decoding (microseconds, chiaki monotonic clock).
	uint64_t LastFrameTimeUs() const { return last_frame_us; }
	HANDLE FrameEvent() const { return frame_event; }

	void SetControllerState(const ChiakiControllerState &state);
	std::vector<SessionEvent> TakeEvents();

	void SetLoginPin(const std::string &pin);
	void KeyboardSetText(const std::string &text);
	void KeyboardAccept();
	void KeyboardReject();
	void GotoBed();
	void GoHome();
	void RequestKeyframe();

	// Microphone
	void SetMicMuted(bool muted);
	bool MicMuted() const { return mic_muted; }

	void SetHapticsGain(float gain) { haptics_gain = gain; }

	StreamStats Stats();
	const StreamConfig &Config() const { return config; }

private:
	static void EventCb(ChiakiEvent *event, void *user);
	static bool VideoSampleCb(uint8_t *buf, size_t buf_size, int32_t frames_lost, bool frame_recovered, void *user);
	static void FrameAvailableCb(ChiakiFfmpegDecoder *decoder, void *user);
	static void AudioSettingsCb(uint32_t channels, uint32_t rate, void *user);
	static void AudioFrameCb(int16_t *buf, size_t samples_count, void *user);
	static void HapticsFrameCb(uint8_t *buf, size_t buf_size, void *user);
	static void CantDisplayCb(void *user, bool cant_display);

	void PushEvent(const SessionEvent &e);

	StreamConfig config;
	std::string host_str;
	Audio *audio = nullptr;

	ChiakiSession session = {};
	ChiakiFfmpegDecoder decoder = {};
	ChiakiOpusDecoder opus_decoder = {};
	ChiakiOpusEncoder opus_encoder = {};
	AVBufferRef *hw_device_ctx = nullptr;
	bool session_inited = false, decoder_inited = false, opus_inited = false;
	HANDLE frame_event = nullptr;

	std::atomic<bool> running{ false };
	std::atomic<bool> connected{ false };
	std::atomic<bool> mic_muted{ true };
	bool mic_connected = false;
	std::atomic<float> haptics_gain{ 1.0f };

	std::mutex events_mutex;
	std::deque<SessionEvent> events;

	// stats
	std::mutex stats_mutex;
	uint64_t bytes_window = 0, frames_window = 0, window_start_ms = 0;
	uint64_t total_lost = 0;
	double mbps = 0.0, decode_fps = 0.0;
	std::atomic<uint64_t> decoded_frames{ 0 };
	std::atomic<uint64_t> last_frame_us{ 0 };
};
