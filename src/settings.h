// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chiaki/regist.h>

#include "kbm.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// A console that has been paired (registered) with this PC.
struct RegisteredHost
{
	std::string mac;            // "aa:bb:cc:dd:ee:ff", primary key
	std::string nickname;
	std::string last_address;   // last known IP
	bool ps5 = true;
	int target = CHIAKI_TARGET_PS5_1;
	uint8_t regist_key[CHIAKI_SESSION_AUTH_SIZE] = {};
	uint8_t rp_key[0x10] = {};  // a.k.a. "morning"
	uint32_t rp_key_type = 0;
	uint32_t console_pin = 0;
	std::string last_app;       // last game seen running on it (shown on the home screen)
	uint64_t last_played = 0;   // unix time of the last session
};

// Picture / enhancement / mouse settings remembered for one game.
struct GameProfile
{
	std::string game;
	int upscale_mode = 0, detail_preset = 2, frame_gen = 0, picture_preset = 0, mouse_preset = 1;
	float sharpness = 0.7f, ai_sharpness = 0.7f, texture_boost = 0.25f, clarity = 0.15f, overshoot = 0.0f;
	float brightness = 0.0f, contrast = 1.0f, saturation = 1.0f, vibrance = 0.0f, hdr_boost = 0.0f;
	float mouse_sensitivity = 0.5f;
};

// A console the user added by IP (discovery broadcasts can't always reach it).
struct ManualHost
{
	std::string address;
};

enum class VideoCodec : int { H264 = 0, H265 = 1 };
enum class Scaler : int { Bilinear = 0, LanczosSharpen = 1 };

struct Settings
{
	// Account
	std::string psn_account_id; // base64, 8 bytes

	// Stream
	int resolution = 1080;      // 360 / 540 / 720 / 1080
	int fps = 60;               // 30 / 60
	int bitrate_kbps = 0;       // 0 = preset default
	VideoCodec codec = VideoCodec::H265;
	bool hdr = true;            // request HDR (HEVC Main10, PQ) when the display supports it
	bool hdr_force = false;     // request HDR even if the display reports SDR (tone-mapped locally)

	// Optional enhancements (off by default)
	int upscale_mode = 0;       // 0 off, 1 enhanced (shader), 2 AI super resolution (NVIDIA RTX / Intel, falls back to enhanced)
	int upscale_target = 0;     // output height: 0 = match the display, or 1440 / 1800 / 2160
	float artifact_reduction = 0.4f; // removes compression blocking/noise before upscaling (enhanced mode)
	float sharpness = 0.7f;     // 0..2, enhanced mode
	float ai_sharpness = 0.7f;  // 0..2, sharpening on top of RTX / Intel super resolution
	float texture_boost = 0.25f; // 0..1, fine texture
	float clarity = 0.15f;      // 0..1, large-radius local contrast
	float overshoot = 0.0f;     // 0..1, allow some halo for extra punch
	int detail_preset = 2;      // 0 off, 1 natural, 2 sharp, 3 ultra, 4 maximum, 5 custom
	void ApplyDetailPreset(int preset);
	float deband = 0.3f;        // 0..1, removes banding in gradients (skies, fog, dark scenes)
	int frame_gen = 0;          // 0 off, 1 predictive (no added latency), 2 interpolated (smoothest, +1 frame latency)
	float paper_white_nits = 203.0f; // brightness of SDR content/UI on an HDR display
	bool ai_hdr = false;        // NVIDIA RTX Video HDR: AI SDR->HDR for SDR games on an HDR display
	float hdr_boost = 0.0f;     // shader SDR->HDR highlight expansion (any GPU), 0..1

	// Picture
	int aspect_mode = 0;        // 0 fit, 1 fill (crop), 2 stretch
	bool ambient = true;        // ambient light in the bars (ultrawide / non-16:9 windows)
	float ambient_intensity = 0.7f;
	int ambient_style = 0;      // 0 glow, 1 extend (blurred picture continues into the bars)
	int picture_preset = 0;     // 0 natural, 1 vivid, 2 cinema, 3 custom
	float brightness = 0.0f, contrast = 1.0f, saturation = 1.0f, vibrance = 0.0f;
	void ApplyPicturePreset(int preset);

	// Keyboard & mouse
	bool kbm_enabled = true;
	KeyboardMouse::Tuning kbm;
	int mouse_preset = 1;       // 0 direct, 1 PC-like, 2 smooth, 3 raw stick, 4 custom
	void ApplyMousePreset(int preset);
	KeyboardMouse::Binding bindings[KeyboardMouse::ActionCount];

	// Capture
	bool replay_enabled = false;  // keep the last N seconds for instant replay
	int replay_seconds = 30;

	// Behaviour
	bool auto_reconnect = true;

	// Display
	bool fullscreen = true;
	bool vsync = true;
	bool show_stats = false;

	// Controller
	bool dualsense_features = true;   // haptics audio + adaptive triggers
	float haptics_gain = 1.0f;        // DualSense haptics intensity
	float rumble_gain = 1.0f;         // rumble for non-DualSense pads
	bool adaptive_triggers = true;
	bool motion_controls = true;      // gyro / accelerometer
	bool view_is_touchpad = false;    // Xbox-style pads: View button = touchpad click

	// Audio
	float volume = 1.0f;
	bool microphone = true;

	// Interface
	bool ui_sounds = true;
	float ui_sound_volume = 0.5f;
	bool boot_animation = true;
	bool game_profiles = true;        // apply a game's saved profile when it starts
	bool low_battery_warning = true;
	std::string last_version;         // version that last saved this file

	std::vector<RegisteredHost> hosts;
	std::vector<ManualHost> manual_hosts;
	std::vector<GameProfile> profiles;
	std::map<std::string, uint64_t> playtime; // game -> seconds streamed

	GameProfile *FindProfile(const std::string &game);
	GameProfile CaptureProfile(const std::string &game) const;
	void ApplyProfile(const GameProfile &p);
	void RemoveProfile(const std::string &game);

	RegisteredHost *FindHost(const std::string &mac);
	void AddOrUpdateHost(const RegisteredHost &host);
	void RemoveHost(const std::string &mac);

	Settings();
	void Load(const std::string &path);
	void Save(const std::string &path) const;
};

// Helpers
std::string HexEncode(const uint8_t *data, size_t size);
bool HexDecode(const std::string &hex, uint8_t *out, size_t size);
std::string Base64Encode(const uint8_t *data, size_t size);
std::vector<uint8_t> Base64Decode(const std::string &in);
std::string MacToString(const uint8_t mac[6]);
std::string NormalizeMac(const std::string &host_id); // discovery host_id ("AABBCCDDEEFF") -> mac string
