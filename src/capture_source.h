// SPDX-License-Identifier: AGPL-3.0-only
// HDMI capture cards (anything Windows lists as a camera: Fifine, Elgato, AVerMedia, the USB 3.0
// sticks) through Media Foundation. The card's own uncompressed picture (YUY2 / UYVY / NV12) is
// read as is and turned into NV12, the format the stream's hardware decoder produces; the session
// then uploads it into the same kind of GPU frame the decoder makes, so a capture card goes
// through exactly the stream's path (AI upscaling, frame generation, picture settings).
// The card's sound is a recording device, played as the game audio. Recording and instant
// replay encode the picture with the GPU's video encoder (NVENC, AMF or Quick Sync).
#pragma once

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

struct AVFrame;
struct AVCodecContext;
struct AVPacket;
class Audio;
class Recorder;

struct CaptureDevice
{
	std::string name;     // as Windows shows it
	std::wstring link;    // symbolic link: identifies the device
};

struct CaptureMode
{
	int width = 0, height = 0;
	double fps = 0.0;
	std::string subtype;  // YUY2, NV12, MJPG, ...
	bool compressed = false;
	std::string Label() const;
	std::string Key() const;  // saved in settings
};

using CaptureFormat = CaptureMode;

class CaptureSource
{
public:
	~CaptureSource();

	static std::vector<CaptureDevice> EnumerateVideo();
	// The card's modes (opens it briefly). Sorted best first.
	static std::vector<CaptureMode> EnumerateModes(const CaptureDevice &dev);
	// Names of audio recording devices (SDL), and the one that most likely belongs to `video_name`.
	static std::vector<std::string> EnumerateAudio();
	static std::string GuessAudio(const std::string &video_name);

	struct Options
	{
		std::string mode_key;      // "" = automatic
		std::string audio_device;  // "" = no sound
		bool full_range = false;   // the card sends full-range (0-255) video
	};
	// on_frame: called on the capture thread after each new frame (first = true once).
	// on_error: called once if the device fails or disappears.
	bool Start(const CaptureDevice &dev, const Options &opt, Audio *audio, HANDLE frame_event,
		std::function<void(bool first)> on_frame, std::function<void(const std::string &)> on_error, std::string *error);
	void Stop();

	AVFrame *PullFrame();   // newest frame since the last call, or nullptr (av_frame_free it)
	CaptureFormat Format();
	double Fps();           // frames delivered per second (measured)
	const std::string &AudioName() const { return audio_name; }

	// Recording / instant replay: the recorder gets H.264 packets and the card's sound.
	void SetRecorder(Recorder *r);
	void RequestKeyframe() { force_key = true; }
	const std::string &EncoderName() const { return enc_name; }

private:
	void Thread();
	bool Open(std::string *error);
	void StartAudio(const std::string &name);
	static void SDLCALL AudioCb(void *user, SDL_AudioStream *stream, int additional, int total);
	AVFrame *Convert(const uint8_t *data, long pitch, size_t len);

	// encoder (own thread)
	void EncoderThread();
	bool OpenEncoder(int w, int h, double fps);
	void CloseEncoder();
	void FeedEncoder(const AVFrame *f);

	CaptureDevice device;
	Options opt;
	Audio *audio = nullptr;
	HANDLE frame_event = nullptr;
	std::function<void(bool)> on_frame;
	std::function<void(const std::string &)> on_error;

	std::thread thread;
	std::atomic<bool> stop{ false };
	struct IMFMediaSource *source = nullptr;
	struct IMFSourceReader *reader = nullptr;
	HANDLE opened_event = nullptr;
	std::string open_error;
	bool open_ok = false;
	bool processed = false;    // the source reader converts to NV12 (compressed modes only)
	int in_kind = 0;           // 0 NV12, 1 YUY2, 2 UYVY
	int out_w = 0, out_h = 0;
	LONG stride = 0;

	std::mutex mutex;
	AVFrame *latest = nullptr;
	CaptureFormat format;
	uint64_t frames = 0, fps_frames = 0, fps_start_ms = 0;
	double fps = 0.0;

	SDL_AudioStream *audio_stream = nullptr;
	std::vector<int16_t> audio_buf;
	std::string audio_name;

	std::atomic<Recorder *> recorder{ nullptr };
	std::thread enc_thread;
	std::mutex enc_mutex;
	std::condition_variable enc_cv;
	std::deque<AVFrame *> enc_queue;
	bool enc_quit = false;
	AVCodecContext *enc = nullptr;
	AVPacket *enc_pkt = nullptr;
	std::string enc_name;
	bool enc_failed = false;
	std::atomic<bool> force_key{ false };
	int64_t enc_pts = 0;
};
#endif
