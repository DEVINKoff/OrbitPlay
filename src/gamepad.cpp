// SPDX-License-Identifier: AGPL-3.0-only
#include "gamepad.h"
#include "log.h"

#include <algorithm>
#include <cstring>

static const float kTouchMaxX = 1919.0f; // DualSense touchpad range used by the PS5
static const float kTouchMaxY = 1079.0f;

// DualSense output report used with SDL_SendGamepadEffect (layout from SDL's test code, zlib license).
struct DS5Effects
{
	uint8_t enable_bits1;           // 0
	uint8_t enable_bits2;           // 1
	uint8_t rumble_right;           // 2
	uint8_t rumble_left;            // 3
	uint8_t headphone_volume;       // 4
	uint8_t speaker_volume;         // 5
	uint8_t mic_volume;             // 6
	uint8_t audio_enable_bits;      // 7
	uint8_t mic_light_mode;         // 8
	uint8_t audio_mute_bits;        // 9
	uint8_t right_trigger[11];      // 10
	uint8_t left_trigger[11];       // 21
	uint8_t unknown1[6];            // 32
	uint8_t enable_bits3;           // 38
	uint8_t unknown2[2];            // 39
	uint8_t led_anim;               // 41
	uint8_t led_brightness;         // 42
	uint8_t pad_lights;             // 43
	uint8_t led_red;                // 44
	uint8_t led_green;              // 45
	uint8_t led_blue;               // 46
};
static_assert(sizeof(DS5Effects) == 47, "DualSense effect report layout");

Gamepads::~Gamepads()
{
	StopAllFeedback();
	for(auto &p : pads)
		SDL_CloseGamepad(p->gamepad);
}

Gamepads::Pad *Gamepads::Find(SDL_JoystickID id)
{
	for(auto &p : pads)
		if(p->id == id)
			return p.get();
	return nullptr;
}

void Gamepads::Open(SDL_JoystickID id)
{
	if(Find(id))
		return;
	SDL_Gamepad *g = SDL_OpenGamepad(id);
	if(!g)
	{
		LOGW("Could not open controller: %s", SDL_GetError());
		return;
	}
	auto pad = std::make_unique<Pad>();
	pad->gamepad = g;
	pad->id = id;
	const char *name = SDL_GetGamepadName(g);
	pad->name = name ? name : "Controller";
	SDL_GamepadType type = SDL_GetGamepadType(g);
	pad->dualsense = type == SDL_GAMEPAD_TYPE_PS5;
	pad->has_touchpad = SDL_GetNumGamepadTouchpads(g) > 0;
	chiaki_controller_state_set_idle(&pad->state);
	chiaki_orientation_tracker_init(&pad->orient);
	chiaki_accel_new_zero_set_inactive(&pad->accel_zero, false);
	chiaki_accel_new_zero_set_inactive(&pad->real_accel, true);
	if(SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO))
		SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true);
	if(SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL))
		SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true);
	SDL_SetGamepadLED(g, led[0], led[1], led[2]);
	SDL_SetGamepadPlayerIndex(g, player_index);
	LOGI("Controller connected: %s%s", pad->name.c_str(), pad->dualsense ? " (DualSense)" : "");
	pads.push_back(std::move(pad));
}

void Gamepads::Close(SDL_JoystickID id)
{
	for(auto it = pads.begin(); it != pads.end(); ++it)
		if((*it)->id == id)
		{
			LOGI("Controller disconnected: %s", (*it)->name.c_str());
			SDL_CloseGamepad((*it)->gamepad);
			pads.erase(it);
			return;
		}
}

static uint32_t MapButton(int button, bool view_is_touchpad)
{
	switch(button)
	{
		case SDL_GAMEPAD_BUTTON_SOUTH: return CHIAKI_CONTROLLER_BUTTON_CROSS;
		case SDL_GAMEPAD_BUTTON_EAST: return CHIAKI_CONTROLLER_BUTTON_MOON;
		case SDL_GAMEPAD_BUTTON_WEST: return CHIAKI_CONTROLLER_BUTTON_BOX;
		case SDL_GAMEPAD_BUTTON_NORTH: return CHIAKI_CONTROLLER_BUTTON_PYRAMID;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return CHIAKI_CONTROLLER_BUTTON_L1;
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return CHIAKI_CONTROLLER_BUTTON_R1;
		case SDL_GAMEPAD_BUTTON_LEFT_STICK: return CHIAKI_CONTROLLER_BUTTON_L3;
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return CHIAKI_CONTROLLER_BUTTON_R3;
		case SDL_GAMEPAD_BUTTON_START: return CHIAKI_CONTROLLER_BUTTON_OPTIONS;
		case SDL_GAMEPAD_BUTTON_BACK: return view_is_touchpad ? CHIAKI_CONTROLLER_BUTTON_TOUCHPAD : CHIAKI_CONTROLLER_BUTTON_SHARE;
		case SDL_GAMEPAD_BUTTON_GUIDE: return CHIAKI_CONTROLLER_BUTTON_PS;
		case SDL_GAMEPAD_BUTTON_TOUCHPAD: return CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
		default: return 0;
	}
}

void Gamepads::HandleEvent(const SDL_Event &e)
{
	switch(e.type)
	{
		case SDL_EVENT_GAMEPAD_ADDED:
			Open(e.gdevice.which);
			return;
		case SDL_EVENT_GAMEPAD_REMOVED:
			Close(e.gdevice.which);
			return;
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
		{
			Pad *p = Find(e.gbutton.which);
			if(!p)
				return;
			bool down = e.gbutton.down;
			if(e.gbutton.button == SDL_GAMEPAD_BUTTON_MISC1)
			{
				if(down && p->dualsense)
					mic_button = true;
				return;
			}
			if(e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) p->guide = down;
			if(e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) p->back = down;
			if(p->guide && p->back && !p->combo_active)
			{
				// Menu combo: don't forward PS/Create to the console
				p->combo_active = true;
				menu_combo = true;
				p->state.buttons &= ~(CHIAKI_CONTROLLER_BUTTON_PS | CHIAKI_CONTROLLER_BUTTON_SHARE | CHIAKI_CONTROLLER_BUTTON_TOUCHPAD);
				return;
			}
			if(p->combo_active)
			{
				if(!p->guide && !p->back)
					p->combo_active = false;
				return;
			}
			uint32_t bit = MapButton(e.gbutton.button, options.view_is_touchpad && !p->has_touchpad);
			if(!bit)
				return;
			if(down)
				p->state.buttons |= bit;
			else
				p->state.buttons &= ~bit;
			return;
		}
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		{
			Pad *p = Find(e.gaxis.which);
			if(!p)
				return;
			int16_t v = e.gaxis.value;
			switch(e.gaxis.axis)
			{
				case SDL_GAMEPAD_AXIS_LEFTX: p->state.left_x = v; break;
				case SDL_GAMEPAD_AXIS_LEFTY: p->state.left_y = v; break;
				case SDL_GAMEPAD_AXIS_RIGHTX: p->state.right_x = v; break;
				case SDL_GAMEPAD_AXIS_RIGHTY: p->state.right_y = v; break;
				case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: p->state.l2_state = (uint8_t)(std::max<int>(0, v) >> 7); break;
				case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: p->state.r2_state = (uint8_t)(std::max<int>(0, v) >> 7); break;
				default: break;
			}
			return;
		}
		case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
		{
			Pad *p = Find(e.gsensor.which);
			if(!p || !options.motion)
				return;
			uint32_t ts_us = (uint32_t)(e.gsensor.sensor_timestamp / 1000);
			if(e.gsensor.sensor == SDL_SENSOR_ACCEL)
			{
				float ax = e.gsensor.data[0] / SDL_STANDARD_GRAVITY;
				float ay = e.gsensor.data[1] / SDL_STANDARD_GRAVITY;
				float az = e.gsensor.data[2] / SDL_STANDARD_GRAVITY;
				chiaki_accel_new_zero_set_active(&p->real_accel, ax, ay, az, true);
				chiaki_orientation_tracker_update(&p->orient, p->state.gyro_x, p->state.gyro_y, p->state.gyro_z,
						ax, ay, az, &p->accel_zero, false, ts_us);
			}
			else if(e.gsensor.sensor == SDL_SENSOR_GYRO)
			{
				chiaki_orientation_tracker_update(&p->orient, e.gsensor.data[0], e.gsensor.data[1], e.gsensor.data[2],
						p->state.accel_x, p->state.accel_y, p->state.accel_z, &p->accel_zero, true, ts_us);
			}
			else
				return;
			chiaki_orientation_tracker_apply_to_controller_state(&p->orient, &p->state);
			return;
		}
		case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
		case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
		case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
		{
			Pad *p = Find(e.gtouchpad.which);
			if(!p)
				return;
			int key = (e.gtouchpad.touchpad << 8) | e.gtouchpad.finger;
			uint16_t x = (uint16_t)(std::clamp(e.gtouchpad.x, 0.0f, 1.0f) * kTouchMaxX);
			uint16_t y = (uint16_t)(std::clamp(e.gtouchpad.y, 0.0f, 1.0f) * kTouchMaxY);
			auto it = p->touch_ids.find(key);
			if(e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN)
			{
				if(it != p->touch_ids.end() || p->touch_ids.size() >= CHIAKI_CONTROLLER_TOUCHES_MAX)
					return;
				int8_t id = chiaki_controller_state_start_touch(&p->state, x, y);
				if(id >= 0)
					p->touch_ids[key] = (uint8_t)id;
			}
			else if(e.type == SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION)
			{
				if(it != p->touch_ids.end())
					chiaki_controller_state_set_touch_pos(&p->state, it->second, x, y);
			}
			else if(it != p->touch_ids.end())
			{
				chiaki_controller_state_stop_touch(&p->state, it->second);
				p->touch_ids.erase(it);
			}
			return;
		}
		default:
			return;
	}
}

ChiakiControllerState Gamepads::State()
{
	ChiakiControllerState merged;
	chiaki_controller_state_set_idle(&merged);
	bool first = true;
	for(auto &p : pads)
	{
		if(first)
		{
			merged = p->state;
			first = false;
		}
		else
		{
			ChiakiControllerState out;
			chiaki_controller_state_or(&out, &merged, &p->state);
			merged = out;
		}
	}
	return merged;
}

bool Gamepads::TakeMenuCombo()
{
	bool v = menu_combo;
	menu_combo = false;
	return v;
}

bool Gamepads::TakeMicButton()
{
	bool v = mic_button;
	mic_button = false;
	return v;
}

void Gamepads::SendDS5Effect(Pad &pad, const void *effect, int size)
{
	SDL_SendGamepadEffect(pad.gamepad, effect, size);
}

void Gamepads::SetRumble(uint8_t left, uint8_t right)
{
	float g = options.rumble_gain;
	uint16_t l = (uint16_t)std::min(65535.0f, (left << 8) * g);
	uint16_t r = (uint16_t)std::min(65535.0f, (right << 8) * g);
	for(auto &p : pads)
		SDL_RumbleGamepad(p->gamepad, l, r, 5000);
}

void Gamepads::SetHapticRumble(uint16_t strength)
{
	float s = std::min(65535.0f, strength * options.rumble_gain);
	uint16_t v = (uint16_t)s;
	// Some controllers ignore very small values
	if(v > 0 && v < (1 << 9))
		v = 1 << 9;
	for(auto &p : pads)
		SDL_RumbleGamepad(p->gamepad, v, v, 100);
}

void Gamepads::SetTriggerEffects(uint8_t type_left, const uint8_t *left, uint8_t type_right, const uint8_t *right)
{
	if(!options.adaptive_triggers)
		return;
	for(auto &p : pads)
	{
		if(!p->dualsense)
			continue;
		DS5Effects fx = {};
		fx.unknown1[4] = ds_intensity;
		fx.enable_bits2 |= 0x40;
		fx.enable_bits1 |= 0x04 | 0x08; // left + right trigger
		fx.left_trigger[0] = type_left;
		memcpy(fx.left_trigger + 1, left, 10);
		fx.right_trigger[0] = type_right;
		memcpy(fx.right_trigger + 1, right, 10);
		SendDS5Effect(*p, &fx, sizeof(fx));
	}
}

void Gamepads::ClearTriggerEffects()
{
	const uint8_t off[10] = {};
	bool saved = options.adaptive_triggers;
	options.adaptive_triggers = true;
	SetTriggerEffects(0x05, off, 0x05, off);
	options.adaptive_triggers = saved;
}

void Gamepads::SetLed(const uint8_t rgb[3])
{
	memcpy(led, rgb, 3);
	for(auto &p : pads)
		SDL_SetGamepadLED(p->gamepad, rgb[0], rgb[1], rgb[2]);
}

void Gamepads::SetPlayerIndex(int index)
{
	player_index = index;
	for(auto &p : pads)
		SDL_SetGamepadPlayerIndex(p->gamepad, index);
}

void Gamepads::SetMicLed(bool on)
{
	for(auto &p : pads)
	{
		if(!p->dualsense)
			continue;
		DS5Effects fx = {};
		fx.enable_bits2 |= 0x01 | 0x02; // mic light + mic
		fx.mic_light_mode = on ? 0x00 : 0x01; // light on = muted (same as a PS5)
		fx.audio_mute_bits = on ? 0x00 : 0x08;
		SendDS5Effect(*p, &fx, sizeof(fx));
	}
}

void Gamepads::ResetMotion()
{
	for(auto &p : pads)
	{
		chiaki_accel_new_zero_set_active(&p->accel_zero, p->real_accel.accel_x, p->real_accel.accel_y, p->real_accel.accel_z, false);
		chiaki_orientation_tracker_init(&p->orient);
	}
}

void Gamepads::StopAllFeedback()
{
	for(auto &p : pads)
		SDL_RumbleGamepad(p->gamepad, 0, 0, 0);
	ClearTriggerEffects();
}

bool Gamepads::AnyDualSense() const
{
	for(auto &p : pads)
		if(p->dualsense)
			return true;
	return false;
}

bool Gamepads::AnyWiredDualSense() const
{
	for(auto &p : pads)
		if(p->dualsense && SDL_GetGamepadConnectionState(p->gamepad) == SDL_JOYSTICK_CONNECTION_WIRED)
			return true;
	return false;
}

std::vector<Gamepads::PadInfo> Gamepads::Info() const
{
	std::vector<PadInfo> out;
	for(auto &p : pads)
	{
		PadInfo i;
		i.name = p->name;
		i.dualsense = p->dualsense;
		i.wired = SDL_GetGamepadConnectionState(p->gamepad) == SDL_JOYSTICK_CONNECTION_WIRED;
		int pct = -1;
		SDL_PowerState ps = SDL_GetGamepadPowerInfo(p->gamepad, &pct);
		i.charging = ps == SDL_POWERSTATE_CHARGING || ps == SDL_POWERSTATE_CHARGED;
		if(ps == SDL_POWERSTATE_ON_BATTERY || ps == SDL_POWERSTATE_CHARGING || ps == SDL_POWERSTATE_CHARGED)
			i.battery = ps == SDL_POWERSTATE_CHARGED ? 100 : pct;
		out.push_back(i);
	}
	return out;
}

std::vector<std::string> Gamepads::Names() const
{
	std::vector<std::string> n;
	for(auto &p : pads)
		n.push_back(p->name);
	return n;
}
