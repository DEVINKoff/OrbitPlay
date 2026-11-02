// SPDX-License-Identifier: AGPL-3.0-only
#include "audio.h"
#include "log.h"

#include <algorithm>
#include <cstring>

// Keep game audio latency low: if more than this is queued (e.g. after a hitch), drop it.
static const int kMaxQueuedMs = 120;

Audio::~Audio()
{
	CloseMic();
	CloseHaptics();
	CloseGame();
}

// ---------------------------------------------------------------------------
// Game audio

bool Audio::OpenGame(int channels, int rate)
{
	std::lock_guard<std::mutex> lock(game_mutex);
	if(game_stream && channels == game_channels && rate == game_rate)
		return true;
	if(game_stream)
	{
		SDL_DestroyAudioStream(game_stream);
		game_stream = nullptr;
	}
	SDL_AudioSpec spec = { SDL_AUDIO_S16, channels, rate };
	game_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if(!game_stream)
	{
		LOGE("Could not open audio output: %s", SDL_GetError());
		return false;
	}
	game_channels = channels;
	game_rate = rate;
	SDL_SetAudioStreamGain(game_stream, volume);
	SDL_ResumeAudioStreamDevice(game_stream);
	LOGI("Audio output: %d ch @ %d Hz", channels, rate);
	return true;
}

void Audio::CloseGame()
{
	std::lock_guard<std::mutex> lock(game_mutex);
	if(game_stream)
	{
		SDL_DestroyAudioStream(game_stream);
		game_stream = nullptr;
	}
}

void Audio::PushGame(const int16_t *samples, size_t frames)
{
	std::lock_guard<std::mutex> lock(game_mutex);
	if(!game_stream)
		return;
	int bytes_per_ms = game_rate / 1000 * game_channels * (int)sizeof(int16_t);
	if(bytes_per_ms > 0 && SDL_GetAudioStreamQueued(game_stream) / bytes_per_ms > kMaxQueuedMs)
		SDL_ClearAudioStream(game_stream);
	SDL_PutAudioStreamData(game_stream, samples, (int)(frames * game_channels * sizeof(int16_t)));
}

void Audio::SetVolume(float v)
{
	std::lock_guard<std::mutex> lock(game_mutex);
	volume = std::max(0.0f, v);
	if(game_stream)
		SDL_SetAudioStreamGain(game_stream, volume);
}

int Audio::QueuedMs()
{
	std::lock_guard<std::mutex> lock(game_mutex);
	if(!game_stream)
		return 0;
	int bytes_per_ms = game_rate / 1000 * game_channels * (int)sizeof(int16_t);
	return bytes_per_ms ? SDL_GetAudioStreamQueued(game_stream) / bytes_per_ms : 0;
}

// ---------------------------------------------------------------------------
// DualSense haptics

static bool IsDualSenseAudioName(const char *name)
{
	if(!name)
		return false;
	std::string n = name;
	std::transform(n.begin(), n.end(), n.begin(), ::tolower);
	return n.find("dualsense") != std::string::npos || n.find("wireless controller") != std::string::npos;
}

bool Audio::OpenHaptics()
{
	if(haptics_stream)
		return true;
	int count = 0;
	SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&count);
	if(!devices)
		return false;
	SDL_AudioDeviceID found = 0;
	for(int i = 0; i < count; i++)
	{
		if(IsDualSenseAudioName(SDL_GetAudioDeviceName(devices[i])))
		{
			found = devices[i];
			break;
		}
	}
	SDL_free(devices);
	if(!found)
		return false;

	SDL_AudioSpec dev_spec = { SDL_AUDIO_S16, 4, 48000 };
	haptics_device = SDL_OpenAudioDevice(found, &dev_spec);
	if(!haptics_device)
	{
		LOGW("Could not open DualSense audio device for haptics: %s", SDL_GetError());
		return false;
	}
	// Input is 4 channels at 3 kHz (front L/R silent, haptics on 3/4); SDL resamples to the device.
	SDL_AudioSpec src_spec = { SDL_AUDIO_S16, 4, 3000 };
	haptics_stream = SDL_CreateAudioStream(&src_spec, &dev_spec);
	if(!haptics_stream || !SDL_BindAudioStream(haptics_device, haptics_stream))
	{
		LOGW("Could not set up haptics stream: %s", SDL_GetError());
		CloseHaptics();
		return false;
	}
	SDL_ResumeAudioDevice(haptics_device);
	LOGI("DualSense haptics enabled (%s)", SDL_GetAudioDeviceName(found));
	return true;
}

void Audio::CloseHaptics()
{
	if(haptics_stream)
	{
		SDL_DestroyAudioStream(haptics_stream);
		haptics_stream = nullptr;
	}
	if(haptics_device)
	{
		SDL_CloseAudioDevice(haptics_device);
		haptics_device = 0;
	}
}

void Audio::PushHaptics(const int16_t *stereo, size_t frames, float gain)
{
	if(!haptics_stream)
		return;
	haptics_buf.resize(frames * 4);
	for(size_t i = 0; i < frames; i++)
	{
		auto scale = [gain](int16_t s) { return (int16_t)std::clamp((int)(s * gain), -32768, 32767); };
		haptics_buf[i * 4 + 0] = 0;
		haptics_buf[i * 4 + 1] = 0;
		haptics_buf[i * 4 + 2] = scale(stereo[i * 2 + 0]);
		haptics_buf[i * 4 + 3] = scale(stereo[i * 2 + 1]);
	}
	// Haptics must stay in sync with the picture: never let the queue grow beyond ~60 ms.
	if(SDL_GetAudioStreamQueued(haptics_stream) > 3000 * 4 * 2 * 60 / 1000)
		SDL_ClearAudioStream(haptics_stream);
	SDL_PutAudioStreamData(haptics_stream, haptics_buf.data(), (int)(haptics_buf.size() * sizeof(int16_t)));
}

// ---------------------------------------------------------------------------
// Microphone

bool Audio::OpenMic(int block_frames, MicCallback cb)
{
	if(mic_stream)
		return true;
	mic_cb = std::move(cb);
	mic_block = block_frames;
	mic_buf.resize((size_t)block_frames * 2);
	SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, 48000 };
	mic_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, MicCb, this);
	if(!mic_stream)
	{
		LOGE("Could not open microphone: %s", SDL_GetError());
		return false;
	}
	SDL_ResumeAudioStreamDevice(mic_stream);
	LOGI("Microphone opened");
	return true;
}

void Audio::CloseMic()
{
	if(mic_stream)
	{
		SDL_DestroyAudioStream(mic_stream);
		mic_stream = nullptr;
	}
}

void SDLCALL Audio::MicCb(void *user, SDL_AudioStream *stream, int /*additional*/, int /*total*/)
{
	auto *self = static_cast<Audio *>(user);
	const int block_bytes = self->mic_block * 2 * (int)sizeof(int16_t);
	while(SDL_GetAudioStreamAvailable(stream) >= block_bytes)
	{
		if(SDL_GetAudioStreamData(stream, self->mic_buf.data(), block_bytes) != block_bytes)
			break;
		if(self->mic_cb)
			self->mic_cb(self->mic_buf.data(), (size_t)self->mic_block);
	}
}
