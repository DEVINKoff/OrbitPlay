// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chiaki/regist.h>

#include <cstdint>
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

	// Optional enhancements (both off by default)
	bool upscale = false;       // upscale the 1080p stream to the display (e.g. 4K) with Lanczos + sharpening
	float sharpness = 0.5f;     // 0..1
	bool frame_gen = false;     // motion-interpolated frames: 60 fps stream -> 120 fps output
	float paper_white_nits = 203.0f; // brightness of SDR content/UI on an HDR display

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

	std::vector<RegisteredHost> hosts;
	std::vector<ManualHost> manual_hosts;

	RegisteredHost *FindHost(const std::string &mac);
	void AddOrUpdateHost(const RegisteredHost &host);
	void RemoveHost(const std::string &mac);

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
