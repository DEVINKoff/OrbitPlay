// SPDX-License-Identifier: AGPL-3.0-only
#include "uisound.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>

static const int kRate = 48000;
static const float kPi = 3.14159265f;

// A short tone: sum of partials with an exponential decay and a few-ms attack (no clicks).
static void AddTone(std::vector<float> &buf, float start_s, float dur_s, float f0, float f1, float amp, float decay,
	float attack_s = 0.004f)
{
	size_t s0 = (size_t)(start_s * kRate), n = (size_t)(dur_s * kRate);
	if(buf.size() < s0 + n)
		buf.resize(s0 + n, 0.0f);
	double phase = 0.0;
	for(size_t i = 0; i < n; i++)
	{
		float t = (float)i / kRate;
		float f = f0 + (f1 - f0) * std::min(1.0f, t / dur_s);
		phase += 2.0 * kPi * f / kRate;
		float env = std::min(1.0f, t / attack_s) * std::exp(-t * decay);
		float tail = std::min(1.0f, (dur_s - t) / 0.01f); // fade the last 10 ms
		buf[s0 + i] += amp * env * tail * (float)(std::sin(phase) + 0.18 * std::sin(phase * 2.0));
	}
}

UiSound::~UiSound() { Shutdown(); }

void UiSound::Init()
{
	// Move: a soft, short "tick"
	AddTone(clips[Move], 0.0f, 0.06f, 2100, 1900, 0.18f, 70.0f, 0.002f);
	// Select: two rising notes
	AddTone(clips[Select], 0.0f, 0.16f, 880, 880, 0.22f, 22.0f);
	AddTone(clips[Select], 0.055f, 0.22f, 1318.5f, 1318.5f, 0.20f, 16.0f);
	// Back: a falling note
	AddTone(clips[Back], 0.0f, 0.14f, 990, 740, 0.20f, 26.0f);
	// Toggle: one gentle blip
	AddTone(clips[Toggle], 0.0f, 0.09f, 1480, 1480, 0.16f, 40.0f, 0.003f);
	// Boot: a slow, warm swell (A major add9) that fades out
	const float notes[] = { 220.0f, 277.18f, 329.63f, 493.88f, 659.25f };
	for(int i = 0; i < 5; i++)
		AddTone(clips[Boot], i * 0.09f, 2.4f - i * 0.09f, notes[i], notes[i] * 1.002f, 0.09f, 1.6f, 0.35f);
	// shimmer
	AddTone(clips[Boot], 0.45f, 1.4f, 1760.0f, 1760.0f, 0.03f, 2.8f, 0.2f);

	Route();
}

static bool IsControllerAudio(const char *name)
{
	if(!name)
		return false;
	std::string n = name;
	for(char &c : n)
		c = (char)tolower((unsigned char)c);
	return n.find("dualsense") != std::string::npos || n.find("wireless controller") != std::string::npos
		|| n.find("dualshock") != std::string::npos;
}

// Interface sounds must never go to a DualSense: when it's plugged in by USB, Windows often makes
// it the default speaker, and its extra channels drive the haptic motors (that's the buzzing).
// Pick the default device unless it looks like the controller, else the first real speaker.
bool UiSound::Route()
{
	last_route_ms = SDL_GetTicks();
	int count = 0;
	SDL_AudioDeviceID *devs = SDL_GetAudioPlaybackDevices(&count);
	bool controller_present = false;
	SDL_AudioDeviceID speaker = 0;
	for(int i = 0; i < count; i++)
	{
		if(IsControllerAudio(SDL_GetAudioDeviceName(devs[i])))
			controller_present = true;
		else if(!speaker)
			speaker = devs[i];
	}
	SDL_free(devs);
	SDL_AudioDeviceID want = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
	if(controller_present)
	{
		// the DualSense audio device has 4 channels; ordinary speakers/headsets have 2, 6 or 8
		SDL_AudioSpec ds;
		int frames = 0;
		bool default_is_controller = SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &ds, &frames) && ds.channels == 4;
		if(default_is_controller)
			want = speaker; // 0 = nowhere safe to play
	}
	if(stream && want == device)
		return true;
	if(stream)
	{
		SDL_DestroyAudioStream(stream);
		stream = nullptr;
	}
	device = want;
	if(!want)
		return false;
	SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, kRate };
	stream = SDL_OpenAudioDeviceStream(want, &spec, nullptr, nullptr);
	if(!stream)
	{
		LOGW("UI sounds unavailable: %s", SDL_GetError());
		return false;
	}
	SDL_ResumeAudioStreamDevice(stream);
	return true;
}

void UiSound::Shutdown()
{
	if(stream)
	{
		SDL_DestroyAudioStream(stream);
		stream = nullptr;
	}
}

void UiSound::Play(Kind kind)
{
	if(!enabled || volume <= 0.0f || kind < 0 || kind >= Count)
		return;
	Uint64 now = SDL_GetTicks();
	if(now - last_route_ms > 2000)
		Route(); // controllers come and go
	if(!stream)
		return;
	if(kind == Move)
	{
		if(now - last_move_ms < 45)
			return; // holding a direction: don't machine-gun
		last_move_ms = now;
	}
	// Don't let sounds pile up behind each other: drop what's queued for anything but Move
	if(kind != Move || SDL_GetAudioStreamQueued(stream) > (int)(kRate * 2 * sizeof(float) / 10))
		SDL_ClearAudioStream(stream);
	std::vector<float> &c = clips[kind];
	std::vector<float> out(c.size() * 2);
	float v = volume * volume;
	for(size_t i = 0; i < c.size(); i++)
		out[i * 2] = out[i * 2 + 1] = std::clamp(c[i] * v, -1.0f, 1.0f);
	SDL_PutAudioStreamData(stream, out.data(), (int)(out.size() * sizeof(float)));
}
