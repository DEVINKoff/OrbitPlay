// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <SDL3/SDL.h>
#include <chiaki/controller.h>

#include <cstdint>
#include <string>

// Keyboard + mouse -> virtual DualSense.
// Mouse movement drives the right stick (with sensitivity, smoothing and anti-deadzone so
// it feels like mouse-look even though the game only sees a stick), keys and mouse buttons
// are rebindable.
class KeyboardMouse
{
public:
	enum Action
	{
		MoveUp, MoveDown, MoveLeft, MoveRight,
		Cross, Circle, Square, Triangle,
		L1, R1, L2, R2, L3, R3,
		DpadUp, DpadDown, DpadLeft, DpadRight,
		Options, Create, Touchpad, PS,
		Walk,             // held: left stick at half deflection
		ActionCount
	};

	struct Binding
	{
		enum Type : uint8_t { None, Key, Mouse, WheelUp, WheelDown } type = None;
		int code = 0; // SDL_Scancode or mouse button index
		bool operator==(const Binding &o) const { return type == o.type && code == o.code; }
	};

	struct Tuning
	{
		float sensitivity = 1.0f;     // mouse -> stick gain
		float anti_deadzone = 0.15f;  // games ignore small stick input; start right above that
		float smoothing = 0.5f;       // 0 = raw, 1 = very smooth
		bool invert_y = false;
	};

	KeyboardMouse();

	static const char *ActionName(int action);
	static std::string BindingName(const Binding &b);
	static std::string Serialize(const Binding &b);
	static Binding Parse(const std::string &s);
	static Binding DefaultBinding(int action);

	Binding bindings[ActionCount];
	void ResetBindings();

	void SetTuning(const Tuning &o) { options = o; }

	// Feed every SDL event while the game has keyboard/mouse focus.
	void HandleEvent(const SDL_Event &e);
	// Call once per input tick; builds the controller state.
	ChiakiControllerState Update();
	// Drop all held keys (focus lost, menu opened).
	void Release();
	bool Active() const;

	// Rebinding: the next key or mouse button press becomes the binding for `action`.
	void BeginCapture(int action) { capture_action = action; }
	int CapturingAction() const { return capture_action; }
	// Returns true if the event was consumed by a capture.
	bool HandleCapture(const SDL_Event &e);

private:
	bool Pressed(int action) const;
	void SetBinding(int action, const Binding &b);

	Tuning options;
	bool keys[SDL_SCANCODE_COUNT] = {};
	bool mouse_buttons[8] = {};
	uint64_t wheel_up_until = 0, wheel_down_until = 0;
	float pending_dx = 0, pending_dy = 0;   // mouse motion since the last update
	float vel_x = 0, vel_y = 0;            // smoothed mouse velocity (px/s)
	uint64_t last_update_us = 0;
	int capture_action = -1;
};
