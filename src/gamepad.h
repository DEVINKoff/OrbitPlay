// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <SDL3/SDL.h>
#include <chiaki/controller.h>
#include <chiaki/orientation.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct GamepadOptions
{
	bool motion = true;
	bool view_is_touchpad = false;
	bool adaptive_triggers = true;
	float rumble_gain = 1.0f;
};

// All connected controllers (DualSense, DualShock 4, Xbox, generic), merged into one PS5 controller.
class Gamepads
{
public:
	~Gamepads();

	void SetOptions(const GamepadOptions &o) { options = o; }
	void HandleEvent(const SDL_Event &e);

	// Merged controller state for the console.
	ChiakiControllerState State();

	// Hotkeys (cleared when read)
	bool TakeMenuCombo();   // Create/View + PS/Guide pressed together
	bool TakeMicButton();   // DualSense mute button

	// Feedback from the console
	void SetRumble(uint8_t left, uint8_t right);
	void SetHapticRumble(uint16_t strength);
	void SetTriggerEffects(uint8_t type_left, const uint8_t *left, uint8_t type_right, const uint8_t *right);
	void ClearTriggerEffects();
	void SetLed(const uint8_t rgb[3]);
	void SetPlayerIndex(int index);
	void SetMicLed(bool on);
	void ResetMotion();
	void SetIntensity(uint8_t value) { ds_intensity = value; }
	void StopAllFeedback();

	bool AnyDualSense() const;
	bool AnyWiredDualSense() const;
	size_t Count() const { return pads.size(); }
	std::vector<std::string> Names() const;

private:
	struct Pad
	{
		SDL_Gamepad *gamepad = nullptr;
		SDL_JoystickID id = 0;
		std::string name;
		bool dualsense = false;
		bool has_touchpad = false;
		ChiakiControllerState state;
		ChiakiOrientationTracker orient;
		ChiakiAccelNewZero accel_zero;
		ChiakiAccelNewZero real_accel;
		std::map<int, uint8_t> touch_ids; // (touchpad << 8 | finger) -> chiaki touch id
		bool guide = false, back = false;
		bool combo_active = false;
	};

	void Open(SDL_JoystickID id);
	void Close(SDL_JoystickID id);
	Pad *Find(SDL_JoystickID id);
	void SendDS5Effect(Pad &pad, const void *effect, int size);

	GamepadOptions options;
	std::vector<std::unique_ptr<Pad>> pads;
	bool menu_combo = false;
	bool mic_button = false;
	uint8_t ds_intensity = 0;
	uint8_t led[3] = { 0, 0, 64 };
	int player_index = 0;
};
