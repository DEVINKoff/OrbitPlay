// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <SDL3/SDL.h>

#include <vector>

// Soft console-style interface sounds, synthesized at startup (no sound files shipped).
class UiSound
{
public:
	enum Kind { Move, Select, Back, Boot, Toggle, Count };

	~UiSound();
	void Init();
	void Shutdown();
	void SetVolume(float v) { volume = v; }
	void SetEnabled(bool e) { enabled = e; }
	void Play(Kind kind);

private:
	bool Route();                       // (re)open on a device that isn't a game controller
	SDL_AudioStream *stream = nullptr;
	SDL_AudioDeviceID device = 0;
	Uint64 last_route_ms = 0;
	std::vector<float> clips[Count];
	float volume = 0.5f;
	bool enabled = true;
	Uint64 last_move_ms = 0;
};
