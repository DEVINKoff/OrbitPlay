// SPDX-License-Identifier: AGPL-3.0-only
#include "kbm.h"

#include <chiaki/time.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

static const char *kActionNames[KeyboardMouse::ActionCount] = {
	"Move forward", "Move back", "Move left", "Move right",
	"Cross (X)", "Circle", "Square", "Triangle",
	"L1", "R1", "L2 (aim)", "R2 (fire)", "L3 (sprint)", "R3 (melee)",
	"D-pad up", "D-pad down", "D-pad left", "D-pad right",
	"Options", "Create", "Touchpad press", "PS button",
	"Walk (hold)",
};

KeyboardMouse::KeyboardMouse()
{
	ResetBindings();
}

const char *KeyboardMouse::ActionName(int action)
{
	return (action >= 0 && action < ActionCount) ? kActionNames[action] : "?";
}

KeyboardMouse::Binding KeyboardMouse::DefaultBinding(int action)
{
	auto key = [](SDL_Scancode s) { Binding b; b.type = Binding::Key; b.code = s; return b; };
	auto mouse = [](int button) { Binding b; b.type = Binding::Mouse; b.code = button; return b; };
	switch(action)
	{
		case MoveUp: return key(SDL_SCANCODE_W);
		case MoveDown: return key(SDL_SCANCODE_S);
		case MoveLeft: return key(SDL_SCANCODE_A);
		case MoveRight: return key(SDL_SCANCODE_D);
		case Cross: return key(SDL_SCANCODE_SPACE);
		case Circle: return key(SDL_SCANCODE_C);
		case Square: return key(SDL_SCANCODE_R);
		case Triangle: return key(SDL_SCANCODE_F);
		case L1: return key(SDL_SCANCODE_Q);
		case R1: return key(SDL_SCANCODE_E);
		case L2: return mouse(SDL_BUTTON_RIGHT);
		case R2: return mouse(SDL_BUTTON_LEFT);
		case L3: return key(SDL_SCANCODE_LSHIFT);
		case R3: return key(SDL_SCANCODE_V);
		case DpadUp: return key(SDL_SCANCODE_1);
		case DpadDown: return key(SDL_SCANCODE_3);
		case DpadLeft: return key(SDL_SCANCODE_2);
		case DpadRight: return key(SDL_SCANCODE_4);
		case Options: return key(SDL_SCANCODE_RETURN);
		case Create: return key(SDL_SCANCODE_TAB);
		case Touchpad: return key(SDL_SCANCODE_T);
		case PS: return key(SDL_SCANCODE_HOME);
		case Walk: return key(SDL_SCANCODE_LALT);
		default: return Binding();
	}
}

void KeyboardMouse::ResetBindings()
{
	for(int i = 0; i < ActionCount; i++)
		bindings[i] = DefaultBinding(i);
}

std::string KeyboardMouse::BindingName(const Binding &b)
{
	switch(b.type)
	{
		case Binding::Key:
		{
			const char *n = SDL_GetScancodeName((SDL_Scancode)b.code);
			return (n && *n) ? n : "Key " + std::to_string(b.code);
		}
		case Binding::Mouse:
			switch(b.code)
			{
				case SDL_BUTTON_LEFT: return "Left mouse";
				case SDL_BUTTON_RIGHT: return "Right mouse";
				case SDL_BUTTON_MIDDLE: return "Middle mouse";
				case SDL_BUTTON_X1: return "Mouse 4";
				case SDL_BUTTON_X2: return "Mouse 5";
				default: return "Mouse " + std::to_string(b.code);
			}
		case Binding::WheelUp: return "Wheel up";
		case Binding::WheelDown: return "Wheel down";
		default: return "-";
	}
}

std::string KeyboardMouse::Serialize(const Binding &b)
{
	switch(b.type)
	{
		case Binding::Key: return "k" + std::to_string(b.code);
		case Binding::Mouse: return "m" + std::to_string(b.code);
		case Binding::WheelUp: return "wu";
		case Binding::WheelDown: return "wd";
		default: return "none";
	}
}

KeyboardMouse::Binding KeyboardMouse::Parse(const std::string &s)
{
	Binding b;
	if(s.size() >= 2 && s[0] == 'k')
	{
		b.type = Binding::Key;
		b.code = atoi(s.c_str() + 1);
		if(b.code <= 0 || b.code >= SDL_SCANCODE_COUNT)
			b = Binding();
	}
	else if(s.size() >= 2 && s[0] == 'm')
	{
		b.type = Binding::Mouse;
		b.code = atoi(s.c_str() + 1);
		if(b.code <= 0 || b.code >= 8)
			b = Binding();
	}
	else if(s == "wu")
		b.type = Binding::WheelUp;
	else if(s == "wd")
		b.type = Binding::WheelDown;
	return b;
}

void KeyboardMouse::SetBinding(int action, const Binding &b)
{
	// One input drives one action: clear it from anything else first
	if(b.type != Binding::None)
		for(int i = 0; i < ActionCount; i++)
			if(i != action && bindings[i] == b)
				bindings[i] = Binding();
	bindings[action] = b;
}

bool KeyboardMouse::HandleCapture(const SDL_Event &e)
{
	if(capture_action < 0)
		return false;
	Binding b;
	switch(e.type)
	{
		case SDL_EVENT_KEY_DOWN:
			if(e.key.scancode == SDL_SCANCODE_ESCAPE)
			{
				capture_action = -1; // cancel
				return true;
			}
			if(e.key.scancode == SDL_SCANCODE_DELETE || e.key.scancode == SDL_SCANCODE_BACKSPACE)
			{
				bindings[capture_action] = Binding(); // clear
				capture_action = -1;
				return true;
			}
			b.type = Binding::Key;
			b.code = e.key.scancode;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			b.type = Binding::Mouse;
			b.code = e.button.button;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
			if(e.wheel.y == 0)
				return true;
			b.type = e.wheel.y > 0 ? Binding::WheelUp : Binding::WheelDown;
			break;
		case SDL_EVENT_KEY_UP:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			return true;
		default:
			return false;
	}
	SetBinding(capture_action, b);
	capture_action = -1;
	return true;
}

void KeyboardMouse::HandleEvent(const SDL_Event &e)
{
	switch(e.type)
	{
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			if(e.key.scancode > 0 && e.key.scancode < SDL_SCANCODE_COUNT)
				keys[e.key.scancode] = e.key.down;
			break;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if(e.button.button < 8)
				mouse_buttons[e.button.button] = e.button.down;
			break;
		case SDL_EVENT_MOUSE_MOTION:
			pending_dx += e.motion.xrel;
			pending_dy += e.motion.yrel;
			break;
		case SDL_EVENT_MOUSE_WHEEL:
		{
			uint64_t until = chiaki_time_now_monotonic_ms() + 60; // a wheel notch is a short press
			if(e.wheel.y > 0)
				wheel_up_until = until;
			else if(e.wheel.y < 0)
				wheel_down_until = until;
			break;
		}
		default:
			break;
	}
}

bool KeyboardMouse::Pressed(int action) const
{
	const Binding &b = bindings[action];
	switch(b.type)
	{
		case Binding::Key: return b.code > 0 && b.code < SDL_SCANCODE_COUNT && keys[b.code];
		case Binding::Mouse: return b.code > 0 && b.code < 8 && mouse_buttons[b.code];
		case Binding::WheelUp: return chiaki_time_now_monotonic_ms() < wheel_up_until;
		case Binding::WheelDown: return chiaki_time_now_monotonic_ms() < wheel_down_until;
		default: return false;
	}
}

bool KeyboardMouse::Active() const
{
	for(int i = 0; i < ActionCount; i++)
		if(Pressed(i))
			return true;
	return std::fabs(vel_x) > 1.0f || std::fabs(vel_y) > 1.0f;
}

void KeyboardMouse::Release()
{
	memset(keys, 0, sizeof(keys));
	memset(mouse_buttons, 0, sizeof(mouse_buttons));
	wheel_up_until = wheel_down_until = 0;
	pending_dx = pending_dy = 0;
	vel_x = vel_y = 0;
}

static int16_t ToAxis(float v)
{
	return (int16_t)std::clamp(v * 32767.0f, -32768.0f, 32767.0f);
}

ChiakiControllerState KeyboardMouse::Update()
{
	ChiakiControllerState s;
	chiaki_controller_state_set_idle(&s);

	uint64_t now = chiaki_time_now_monotonic_us();
	float dt = last_update_us ? (float)(now - last_update_us) / 1e6f : 0.002f;
	dt = std::clamp(dt, 0.0005f, 0.05f);
	last_update_us = now;

	// Mouse -> right stick. Velocity = motion over a short sliding window (not an exponential
	// filter, which trails behind your hand). At the default it's ~2 ms: as direct as a stick
	// can be, while still averaging over a few mouse reports so the stick doesn't flicker.
	if(pending_dx != 0.0f || pending_dy != 0.0f)
	{
		hist[hist_head] = { now, pending_dx, pending_dy };
		hist_head = (hist_head + 1) % 128;
		hist_count = std::min(hist_count + 1, 128);
	}
	pending_dx = pending_dy = 0;
	float smooth = std::clamp(options.smoothing, 0.0f, 1.0f);
	uint64_t window_us = (uint64_t)((0.0015 + 0.025 * smooth * smooth) * 1e6);
	float sx = 0, sy = 0;
	for(int i = 0; i < hist_count; i++)
	{
		const Sample &h = hist[(hist_head - 1 - i + 128) % 128];
		if(now - h.t_us > window_us)
			break;
		sx += h.dx;
		sy += h.dy;
	}
	float win_s = std::max((float)window_us / 1e6f, dt);
	vel_x = sx / win_s;
	vel_y = sy / win_s;
	// 1.0 sensitivity: ~1500 px/s of mouse movement = full stick
	float gain = std::max(0.05f, options.sensitivity) / 1500.0f;
	float rx = vel_x * gain, ry = vel_y * gain * (options.invert_y ? -1.0f : 1.0f);
	float mag = std::sqrt(rx * rx + ry * ry);
	if(mag > 0.002f)
	{
		// Games turn stick deflection into camera speed through a curve (often ~squared) and a
		// deadzone. Pre-distort so mouse speed maps to camera speed about linearly, like on PC.
		float m = std::min(mag, 1.0f);
		m = std::pow(m, 1.0f / std::clamp(options.curve, 1.0f, 3.0f));
		float dz = std::clamp(options.anti_deadzone, 0.0f, 0.6f);
		float out = std::min(1.0f, dz + (1.0f - dz) * m);
		rx *= out / mag;
		ry *= out / mag;
		s.right_x = ToAxis(rx);
		s.right_y = ToAxis(ry);
	}

	// Movement keys -> left stick (diagonals normalized)
	float lx = (Pressed(MoveRight) ? 1.0f : 0.0f) - (Pressed(MoveLeft) ? 1.0f : 0.0f);
	float ly = (Pressed(MoveDown) ? 1.0f : 0.0f) - (Pressed(MoveUp) ? 1.0f : 0.0f);
	if(lx != 0.0f || ly != 0.0f)
	{
		float len = std::sqrt(lx * lx + ly * ly);
		float scale = (Pressed(Walk) ? 0.5f : 1.0f) / len;
		s.left_x = ToAxis(lx * scale);
		s.left_y = ToAxis(ly * scale);
	}

	struct { int action; uint32_t bit; } map[] = {
		{ Cross, CHIAKI_CONTROLLER_BUTTON_CROSS }, { Circle, CHIAKI_CONTROLLER_BUTTON_MOON },
		{ Square, CHIAKI_CONTROLLER_BUTTON_BOX }, { Triangle, CHIAKI_CONTROLLER_BUTTON_PYRAMID },
		{ L1, CHIAKI_CONTROLLER_BUTTON_L1 }, { R1, CHIAKI_CONTROLLER_BUTTON_R1 },
		{ L3, CHIAKI_CONTROLLER_BUTTON_L3 }, { R3, CHIAKI_CONTROLLER_BUTTON_R3 },
		{ DpadUp, CHIAKI_CONTROLLER_BUTTON_DPAD_UP }, { DpadDown, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN },
		{ DpadLeft, CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT }, { DpadRight, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT },
		{ Options, CHIAKI_CONTROLLER_BUTTON_OPTIONS }, { Create, CHIAKI_CONTROLLER_BUTTON_SHARE },
		{ Touchpad, CHIAKI_CONTROLLER_BUTTON_TOUCHPAD }, { PS, CHIAKI_CONTROLLER_BUTTON_PS },
	};
	for(auto &m : map)
		if(Pressed(m.action))
			s.buttons |= m.bit;
	// triggers are sent as their analog value (like the gamepad path)
	if(Pressed(L2))
		s.l2_state = 255;
	if(Pressed(R2))
		s.r2_state = 255;
	return s;
}
