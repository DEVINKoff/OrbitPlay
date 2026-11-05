// SPDX-License-Identifier: AGPL-3.0-only
#include "recorder.h"
#include "log.h"

#include <chiaki/time.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cstring>

namespace {

// Iterates the NAL units of an Annex B buffer: cb(type, start_including_startcode, size)
template<class F>
void ForEachNal(const uint8_t *d, size_t n, bool hevc, F &&cb)
{
	size_t i = 0;
	auto find_start = [&](size_t from, size_t *sc_len) -> size_t {
		for(size_t k = from; k + 3 <= n; k++)
		{
			if(d[k] == 0 && d[k + 1] == 0 && d[k + 2] == 1)
			{
				bool four = k > 0 && d[k - 1] == 0;
				*sc_len = four ? 4 : 3;
				return four ? k - 1 : k;
			}
		}
		return n;
	};
	size_t sc = 0;
	i = find_start(0, &sc);
	while(i < n)
	{
		size_t payload = i + sc;
		size_t next_sc = 0;
		size_t next = find_start(payload, &next_sc);
		if(payload < n)
		{
			int type = hevc ? ((d[payload] >> 1) & 0x3f) : (d[payload] & 0x1f);
			cb(type, i, next - i, sc);
		}
		i = next;
		sc = next_sc;
	}
}

bool IsParamSet(int type, bool hevc)
{
	return hevc ? (type == 32 || type == 33 || type == 34) : (type == 7 || type == 8);
}

bool IsIdr(int type, bool hevc)
{
	return hevc ? (type >= 16 && type <= 21) : type == 5;
}

std::vector<uint8_t> ExtractParamSets(const uint8_t *d, size_t n, bool hevc)
{
	std::vector<uint8_t> out;
	ForEachNal(d, n, hevc, [&](int type, size_t off, size_t len, size_t sc) {
		if(!IsParamSet(type, hevc))
			return;
		static const uint8_t start[4] = { 0, 0, 0, 1 };
		out.insert(out.end(), start, start + 4);
		out.insert(out.end(), d + off + sc, d + off + len);
	});
	return out;
}

std::string AvErr(int e)
{
	char b[128];
	av_make_error_string(b, sizeof(b), e);
	return b;
}

// Muxer state for one output file
struct Mux
{
	AVFormatContext *oc = nullptr;
	AVStream *vs = nullptr, *as = nullptr;
	AVCodecContext *aenc = nullptr;
	AVFrame *aframe = nullptr;
	uint64_t t0 = 0;
	int64_t last_vpts = -1;
	int64_t last_index = -1;
	int fps = 60;
	bool audio_started = false;
	int64_t audio_samples = 0;
	std::vector<float> abuf[2];   // planar accumulation
	int channels = 2, rate = 48000;
	bool ok = false;

	bool Open(const std::string &path, const Recorder::StreamInfo &info, const std::vector<uint8_t> &extradata,
			uint64_t start_us, int ch, int sr, std::string *err)
	{
		t0 = start_us;
		fps = std::max(1, info.fps);
		channels = std::clamp(ch, 1, 2);
		rate = sr > 0 ? sr : 48000;
		int r = avformat_alloc_output_context2(&oc, nullptr, "mp4", path.c_str());
		if(r < 0 || !oc)
		{
			*err = "could not create MP4: " + AvErr(r);
			return false;
		}
		vs = avformat_new_stream(oc, nullptr);
		vs->time_base = { 1, 90000 };
		AVCodecParameters *vp = vs->codecpar;
		vp->codec_type = AVMEDIA_TYPE_VIDEO;
		vp->codec_id = info.hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
		if(info.hevc)
			vp->codec_tag = MKTAG('h', 'v', 'c', '1'); // plays in Windows' Media Player, Premiere, Resolve
		vp->width = info.width;
		vp->height = info.height;
		vp->color_range = AVCOL_RANGE_MPEG;
		if(info.hdr)
		{
			vp->color_primaries = AVCOL_PRI_BT2020;
			vp->color_trc = AVCOL_TRC_SMPTE2084;
			vp->color_space = AVCOL_SPC_BT2020_NCL;
		}
		else
		{
			vp->color_primaries = AVCOL_PRI_BT709;
			vp->color_trc = AVCOL_TRC_BT709;
			vp->color_space = AVCOL_SPC_BT709;
		}
		vp->extradata = (uint8_t *)av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(vp->extradata, extradata.data(), extradata.size());
		vp->extradata_size = (int)extradata.size();
		vs->avg_frame_rate = { info.fps, 1 };

		const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
		if(aac)
		{
			aenc = avcodec_alloc_context3(aac);
			aenc->sample_rate = rate;
			av_channel_layout_default(&aenc->ch_layout, channels);
			aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
			aenc->bit_rate = 192000;
			aenc->time_base = { 1, rate };
			if(oc->oformat->flags & AVFMT_GLOBALHEADER)
				aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			if(avcodec_open2(aenc, aac, nullptr) < 0)
				avcodec_free_context(&aenc);
		}
		if(aenc)
		{
			as = avformat_new_stream(oc, nullptr);
			as->time_base = { 1, rate };
			avcodec_parameters_from_context(as->codecpar, aenc);
			aframe = av_frame_alloc();
			aframe->format = AV_SAMPLE_FMT_FLTP;
			av_channel_layout_copy(&aframe->ch_layout, &aenc->ch_layout);
			aframe->sample_rate = rate;
			aframe->nb_samples = aenc->frame_size > 0 ? aenc->frame_size : 1024;
			av_frame_get_buffer(aframe, 0);
		}
		else
			LOGW("Recording without audio (no AAC encoder)");

		r = avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE);
		if(r < 0)
		{
			*err = "could not open the file for writing: " + AvErr(r);
			return false;
		}
		AVDictionary *opts = nullptr;
		av_dict_set(&opts, "movflags", "+write_colr", 0);
		r = avformat_write_header(oc, &opts);
		av_dict_free(&opts);
		if(r < 0)
		{
			*err = "could not write the MP4 header: " + AvErr(r);
			return false;
		}
		ok = true;
		return true;
	}

	void WriteVideo(const uint8_t *data, size_t size, uint64_t t_us, bool key)
	{
		if(!ok)
			return;
		AVPacket *pkt = av_packet_alloc();
		if(av_new_packet(pkt, (int)size) < 0)
		{
			av_packet_free(&pkt);
			return;
		}
		memcpy(pkt->data, data, size);
		// Constant frame rate: snap arrival times to the stream's frame grid (network jitter
		// would otherwise make a variable-frame-rate file that some editors drift on).
		double secs = (t_us >= t0 ? t_us - t0 : 0) / 1e6;
		int64_t index = (int64_t)(secs * fps + 0.5);
		if(index <= last_index)
			index = last_index + 1;
		last_index = index;
		int64_t pts = index * 90000 / fps;
		last_vpts = pts;
		pkt->pts = pkt->dts = av_rescale_q(pts, { 1, 90000 }, vs->time_base);
		pkt->stream_index = vs->index;
		if(key)
			pkt->flags |= AV_PKT_FLAG_KEY;
		av_interleaved_write_frame(oc, pkt);
		av_packet_free(&pkt);
	}

	void Drain(bool flush)
	{
		if(!aenc)
			return;
		if(flush)
			avcodec_send_frame(aenc, nullptr);
		AVPacket *pkt = av_packet_alloc();
		while(avcodec_receive_packet(aenc, pkt) == 0)
		{
			av_packet_rescale_ts(pkt, aenc->time_base, as->time_base);
			pkt->stream_index = as->index;
			av_interleaved_write_frame(oc, pkt);
		}
		av_packet_free(&pkt);
	}

	void WriteAudio(const int16_t *pcm, size_t frames, int ch, uint64_t t_us)
	{
		if(!ok || !aenc || t_us < t0)
			return;
		if(!audio_started)
		{
			// align the first audio sample with the video timeline
			audio_samples = (int64_t)((t_us - t0) * (uint64_t)rate / 1000000);
			audio_started = true;
		}
		for(size_t i = 0; i < frames; i++)
			for(int c = 0; c < channels; c++)
				abuf[c].push_back(pcm[i * ch + std::min(c, ch - 1)] / 32768.0f);
		int fs = aframe->nb_samples;
		while((int)abuf[0].size() >= fs)
		{
			if(av_frame_make_writable(aframe) < 0)
				return;
			for(int c = 0; c < channels; c++)
			{
				memcpy(aframe->data[c], abuf[c].data(), fs * sizeof(float));
				abuf[c].erase(abuf[c].begin(), abuf[c].begin() + fs);
			}
			aframe->pts = audio_samples;
			audio_samples += fs;
			if(avcodec_send_frame(aenc, aframe) == 0)
				Drain(false);
		}
	}

	void Close()
	{
		if(oc)
		{
			if(ok)
			{
				Drain(true);
				av_write_trailer(oc);
			}
			if(oc->pb)
				avio_closep(&oc->pb);
			avformat_free_context(oc);
			oc = nullptr;
		}
		if(aframe)
			av_frame_free(&aframe);
		if(aenc)
			avcodec_free_context(&aenc);
	}
};

} // namespace

// ---------------------------------------------------------------------------

Recorder::Recorder() = default;

Recorder::~Recorder()
{
	StopRecording();
	std::unique_lock<std::mutex> lock(mutex);
	quit = true;
	cv.notify_all();
	// writer threads are detached: give them up to 5 s to finish their files
	for(int i = 0; i < 50 && active_writers > 0; i++)
	{
		lock.unlock();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		lock.lock();
	}
}

bool Recorder::IsKeyframe(const uint8_t *data, size_t size, bool hevc)
{
	bool idr = false, ps = false;
	ForEachNal(data, size, hevc, [&](int type, size_t, size_t, size_t) {
		idr |= IsIdr(type, hevc);
		ps |= IsParamSet(type, hevc);
	});
	return idr && ps;
}

void Recorder::SetStreamInfo(const StreamInfo &i)
{
	std::lock_guard<std::mutex> lock(mutex);
	info = i;
	ring_video.clear();
	ring_audio.clear();
}

void Recorder::SetMessage(const std::string &m)
{
	std::lock_guard<std::mutex> lock(msg_mutex);
	message = m;
}

std::string Recorder::TakeMessage()
{
	std::lock_guard<std::mutex> lock(msg_mutex);
	std::string m;
	m.swap(message);
	return m;
}

void Recorder::OnVideoPacket(const uint8_t *data, size_t size)
{
	if(!recording && !replay_enabled)
		return;
	uint64_t now = chiaki_time_now_monotonic_us();
	std::lock_guard<std::mutex> lock(mutex);
	bool key = IsKeyframe(data, size, info.hevc);
	if(recording)
	{
		if(waiting_key && key)
		{
			waiting_key = false;
			rec_start_us = now;
			live_job->video.clear();
		}
		if(!waiting_key)
		{
			live_video.push_back({ std::vector<uint8_t>(data, data + size), now, key });
			cv.notify_all();
		}
	}
	if(replay_enabled)
	{
		ring_video.push_back({ std::vector<uint8_t>(data, data + size), now, key });
		TrimReplay(now);
	}
}

void Recorder::OnAudio(const int16_t *pcm, size_t frames, int channels, int rate)
{
	if(!recording && !replay_enabled)
		return;
	uint64_t now = chiaki_time_now_monotonic_us();
	std::lock_guard<std::mutex> lock(mutex);
	audio_channels = channels;
	audio_rate = rate;
	APacket a{ std::vector<int16_t>(pcm, pcm + frames * channels), now };
	if(recording && !waiting_key)
	{
		live_audio.push_back(a);
		cv.notify_all();
	}
	if(replay_enabled)
		ring_audio.push_back(std::move(a));
}

void Recorder::TrimReplay(uint64_t now)
{
	uint64_t window = (uint64_t)replay_seconds * 1000000;
	if(now < window)
		return;
	uint64_t cutoff = now - window;
	// Keep the clip starting at the last keyframe at or before the cutoff
	size_t keep_from = 0;
	for(size_t i = 0; i < ring_video.size(); i++)
	{
		if(ring_video[i].t_us > cutoff)
			break;
		if(ring_video[i].key)
			keep_from = i;
	}
	if(keep_from > 0)
		ring_video.erase(ring_video.begin(), ring_video.begin() + keep_from);
	// Without keyframes the buffer can't be cut; cap it hard at 3x the window
	while(!ring_video.empty() && now - ring_video.front().t_us > window * 3)
		ring_video.pop_front();
	uint64_t first = ring_video.empty() ? now : ring_video.front().t_us;
	while(!ring_audio.empty() && ring_audio.front().t_us + 50000 < first)
		ring_audio.pop_front();
}

void Recorder::SetReplay(bool enabled, int seconds)
{
	std::lock_guard<std::mutex> lock(mutex);
	replay_seconds = std::clamp(seconds, 5, 300);
	if(enabled != replay_enabled)
	{
		ring_video.clear();
		ring_audio.clear();
	}
	replay_enabled = enabled;
}

double Recorder::ReplayBufferedSeconds()
{
	std::lock_guard<std::mutex> lock(mutex);
	for(size_t i = 0; i < ring_video.size(); i++)
		if(ring_video[i].key)
			return (chiaki_time_now_monotonic_us() - ring_video[i].t_us) / 1e6;
	return 0.0;
}

double Recorder::RecordingSeconds() const
{
	if(!recording || waiting_key)
		return 0.0;
	return (chiaki_time_now_monotonic_us() - rec_start_us) / 1e6;
}

bool Recorder::WriteJob(Job &job)
{
	// find the first keyframe
	while(!job.video.empty() && !job.video.front().key)
		job.video.pop_front();
	if(job.video.empty())
	{
		SetMessage("Nothing to save yet - the replay buffer is still filling.");
		return false;
	}
	const VPacket &k = job.video.front();
	std::vector<uint8_t> extradata = ExtractParamSets(k.data.data(), k.data.size(), job.info.hevc);
	Mux mux;
	std::string err;
	if(!mux.Open(job.path, job.info, extradata, k.t_us, job.channels, job.rate, &err))
	{
		mux.Close();
		SetMessage("Recording failed: " + err);
		LOGE("Recorder: %s", err.c_str());
		return false;
	}
	// interleave by time
	size_t ai = 0;
	for(const VPacket &v : job.video)
	{
		while(ai < job.audio.size() && job.audio[ai].t_us <= v.t_us)
		{
			const APacket &a = job.audio[ai++];
			mux.WriteAudio(a.pcm.data(), a.pcm.size() / job.channels, job.channels, a.t_us);
		}
		mux.WriteVideo(v.data.data(), v.data.size(), v.t_us, v.key);
	}
	for(; ai < job.audio.size(); ai++)
		mux.WriteAudio(job.audio[ai].pcm.data(), job.audio[ai].pcm.size() / job.channels, job.channels, job.audio[ai].t_us);
	mux.Close();
	return true;
}

bool Recorder::SaveReplay(const std::string &path)
{
	auto job = std::make_shared<Job>();
	{
		std::lock_guard<std::mutex> lock(mutex);
		if(!replay_enabled || ring_video.empty())
		{
			SetMessage("Instant replay is off or still filling.");
			return false;
		}
		job->path = path;
		job->info = info;
		job->channels = audio_channels;
		job->rate = audio_rate;
		job->video = ring_video;
		job->audio = ring_audio;
		active_writers++;
	}
	std::thread([this, job]() {
		double secs = job->video.size() > 1 ? (job->video.back().t_us - job->video.front().t_us) / 1e6 : 0.0;
		if(WriteJob(*job))
		{
			char m[96];
			snprintf(m, sizeof(m), "Saved the last %.0f seconds to Videos\\Orbit Play", secs);
			SetMessage(m);
			LOGI("Replay saved: %s", job->path.c_str());
		}
		std::lock_guard<std::mutex> lock(mutex);
		active_writers--;
	}).detach();
	return true;
}

bool Recorder::StartRecording(const std::string &path)
{
	std::lock_guard<std::mutex> lock(mutex);
	if(recording)
		return false;
	live_job = std::make_shared<Job>();
	live_job->path = path;
	live_job->info = info;
	live_job->live = true;
	live_video.clear();
	live_audio.clear();
	live_stop = false;
	waiting_key = true;
	recording = true;
	active_writers++;

	std::thread([this]() {
		std::shared_ptr<Job> job;
		Mux mux;
		bool opened = false, failed = false;
		size_t written = 0;
		std::string path_copy;
		for(;;)
		{
			std::deque<VPacket> v;
			std::deque<APacket> a;
			int channels, rate;
			bool stop;
			{
				std::unique_lock<std::mutex> lock(mutex);
				cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return !live_video.empty() || live_stop || quit; });
				v.swap(live_video);
				a.swap(live_audio);
				stop = live_stop || quit;
				job = live_job;
				channels = audio_channels;
				rate = audio_rate;
			}
			path_copy = job->path;
			if(!opened && !failed && !v.empty())
			{
				std::vector<uint8_t> extradata = ExtractParamSets(v.front().data.data(), v.front().data.size(), job->info.hevc);
				std::string err;
				job->channels = channels;
				job->rate = rate;
				if(mux.Open(job->path, job->info, extradata, v.front().t_us, channels, rate, &err))
					opened = true;
				else
				{
					failed = true;
					SetMessage("Recording failed: " + err);
					LOGE("Recorder: %s", err.c_str());
				}
			}
			if(opened)
			{
				size_t ai = 0;
				for(const VPacket &p : v)
				{
					while(ai < a.size() && a[ai].t_us <= p.t_us)
					{
						mux.WriteAudio(a[ai].pcm.data(), a[ai].pcm.size() / job->channels, job->channels, a[ai].t_us);
						ai++;
					}
					mux.WriteVideo(p.data.data(), p.data.size(), p.t_us, p.key);
					written++;
				}
				for(; ai < a.size(); ai++)
					mux.WriteAudio(a[ai].pcm.data(), a[ai].pcm.size() / job->channels, job->channels, a[ai].t_us);
			}
			if(stop)
				break;
		}
		mux.Close();
		if(opened)
		{
			SetMessage("Recording saved to Videos\\Orbit Play");
			LOGI("Recording saved: %s (%zu video packets)", path_copy.c_str(), written);
		}
		else if(!failed)
			SetMessage("Recording stopped before the first keyframe - nothing saved.");
		std::lock_guard<std::mutex> lock(mutex);
		active_writers--;
	}).detach();
	return true;
}

void Recorder::StopRecording()
{
	std::lock_guard<std::mutex> lock(mutex);
	if(!recording)
		return;
	recording = false;
	waiting_key = false;
	live_stop = true;
	cv.notify_all();
}
