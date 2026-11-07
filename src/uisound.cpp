// SPDX-License-Identifier: AGPL-3.0-only
#include "uisound.h"
#include "log.h"

#include <algorithm>
#include <cmath>

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

	SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, kRate };
	stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if(!stream)
	{
		LOGW("UI sounds unavailable: %s", SDL_GetError());
		return;
	}
	SDL_ResumeAudioStreamDevice(stream);
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
	if(!stream || !enabled || volume <= 0.0f || kind < 0 || kind >= Count)
		return;
	Uint64 now = SDL_GetTicks();
	if(kind == Move)
	{
		if(now - last_move_ms < 45)
			return; // holding a direction: don't machine-gun
		last_move_ms = now;
	}
	// Don't let sounds pile up behind each other: drop what's queued for anything but Move
	if(kind != Move || SDL_GetAudioStreamQueued(stream) > (int)(kRate * sizeof(float) / 10))
		SDL_ClearAudioStream(stream);
	std::vector<float> &c = clips[kind];
	std::vector<float> out(c.size());
	float v = volume * volume;
	for(size_t i = 0; i < c.size(); i++)
		out[i] = std::clamp(c[i] * v, -1.0f, 1.0f);
	SDL_PutAudioStreamData(stream, out.data(), (int)(out.size() * sizeof(float)));
}
