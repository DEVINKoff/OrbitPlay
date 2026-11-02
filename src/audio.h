// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Game audio out, DualSense haptics out and microphone in, all through SDL3 audio streams.
class Audio
{
public:
	~Audio();

	// Game audio --------------------------------------------------------------
	bool OpenGame(int channels, int rate);
	void CloseGame();
	void PushGame(const int16_t *samples, size_t frames);
	void SetVolume(float volume);

	// DualSense haptics -------------------------------------------------------
	// The PS5 streams haptics as 3 kHz stereo audio. A wired DualSense exposes a
	// 4-channel audio device whose channels 3/4 drive the left/right actuators.
	bool OpenHaptics();
	void CloseHaptics();
	bool HapticsActive() const { return haptics_stream != nullptr; }
	void PushHaptics(const int16_t *stereo, size_t frames, float gain);

	// Microphone ---------------------------------------------------------------
	// Delivers 48 kHz stereo blocks of exactly `block_frames` frames to `cb` (on SDL's audio thread).
	using MicCallback = std::function<void(int16_t *samples, size_t frames)>;
	bool OpenMic(int block_frames, MicCallback cb);
	void CloseMic();
	bool MicOpen() const { return mic_stream != nullptr; }

	// Latency of the game audio queue in milliseconds.
	int QueuedMs();

private:
	static void SDLCALL MicCb(void *user, SDL_AudioStream *stream, int additional, int total);

	std::mutex game_mutex;
	SDL_AudioStream *game_stream = nullptr;
	int game_channels = 2, game_rate = 48000;
	float volume = 1.0f;

	SDL_AudioDeviceID haptics_device = 0;
	SDL_AudioStream *haptics_stream = nullptr;
	std::vector<int16_t> haptics_buf;

	SDL_AudioStream *mic_stream = nullptr;
	MicCallback mic_cb;
	int mic_block = 480;
	std::vector<int16_t> mic_buf;
};
