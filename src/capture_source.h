// SPDX-License-Identifier: AGPL-3.0-only
// HDMI capture cards (Elgato, AVerMedia, the cheap USB ones - anything Windows lists as a
// camera) through Media Foundation. Frames come out as NV12 AVFrames, so they go through the
// same renderer path as the PS5 stream: upscaling, frame generation, HDR, picture settings.
// The card's audio is a recording device; it is played straight to the speakers.
#pragma once

#ifdef _WIN32
#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

struct AVFrame;
class Audio;

struct CaptureDevice
{
	std::string name;     // as Windows shows it
	std::wstring link;    // symbolic link: identifies the device
};

struct CaptureFormat
{
	int width = 0, height = 0;
	double fps = 0.0;
	std::string subtype;  // NV12, YUY2, MJPG, ...
};

class CaptureSource
{
public:
	~CaptureSource();

	static std::vector<CaptureDevice> EnumerateVideo();
	// Names of audio recording devices (SDL), and the one that most likely belongs to `video_name`.
	static std::vector<std::string> EnumerateAudio();
	static std::string GuessAudio(const std::string &video_name);

	// on_frame: called on the capture thread after each new frame (and once with first = true).
	// on_error: called once if the device fails or disappears.
	bool Start(const CaptureDevice &dev, const std::string &audio_device, Audio *audio, HANDLE frame_event,
		std::function<void(bool first)> on_frame, std::function<void(const std::string &)> on_error, std::string *error);
	void Stop();

	AVFrame *PullFrame();   // newest frame since the last call, or nullptr (av_frame_free it)
	CaptureFormat Format();
	double Fps();           // frames delivered per second (measured)
	const std::string &AudioName() const { return audio_name; }

private:
	void Thread();
	bool Open(std::string *error);
	void StartAudio(const std::string &name);
	static void SDLCALL AudioCb(void *user, SDL_AudioStream *stream, int additional, int total);

	CaptureDevice device;
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

	std::mutex mutex;
	AVFrame *latest = nullptr;
	CaptureFormat format;
	int out_w = 0, out_h = 0;
	LONG stride = 0;
	uint64_t frames = 0, fps_frames = 0, fps_start_ms = 0;
	double fps = 0.0;

	SDL_AudioStream *audio_stream = nullptr;
	std::vector<int16_t> audio_buf;
	std::string audio_name;
};
#endif
