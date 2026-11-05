// SPDX-License-Identifier: AGPL-3.0-only
#include "settings.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

// ---------------------------------------------------------------------------
// Encoding helpers

std::string HexEncode(const uint8_t *data, size_t size)
{
	static const char *digits = "0123456789abcdef";
	std::string out;
	out.reserve(size * 2);
	for(size_t i = 0; i < size; i++)
	{
		out.push_back(digits[data[i] >> 4]);
		out.push_back(digits[data[i] & 0xf]);
	}
	return out;
}

static int HexVal(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool HexDecode(const std::string &hex, uint8_t *out, size_t size)
{
	if(hex.size() != size * 2)
		return false;
	for(size_t i = 0; i < size; i++)
	{
		int hi = HexVal(hex[i * 2]), lo = HexVal(hex[i * 2 + 1]);
		if(hi < 0 || lo < 0)
			return false;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return true;
}

static const char *kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t *data, size_t size)
{
	std::string out;
	size_t i = 0;
	while(i + 2 < size)
	{
		uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
		out += kB64[(v >> 18) & 63]; out += kB64[(v >> 12) & 63];
		out += kB64[(v >> 6) & 63];  out += kB64[v & 63];
		i += 3;
	}
	if(i < size)
	{
		uint32_t v = data[i] << 16;
		if(i + 1 < size)
			v |= data[i + 1] << 8;
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += (i + 1 < size) ? kB64[(v >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

std::vector<uint8_t> Base64Decode(const std::string &in)
{
	std::vector<uint8_t> out;
	uint32_t acc = 0;
	int bits = 0;
	for(char c : in)
	{
		if(c == '=' || isspace((unsigned char)c))
			continue;
		const char *p = strchr(kB64, c);
		if(!p || !*p)
			return {};
		acc = (acc << 6) | (uint32_t)(p - kB64);
		bits += 6;
		if(bits >= 8)
		{
			bits -= 8;
			out.push_back((uint8_t)((acc >> bits) & 0xff));
		}
	}
	return out;
}

std::string MacToString(const uint8_t mac[6])
{
	char buf[18];
	snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return buf;
}

std::string NormalizeMac(const std::string &host_id)
{
	std::string hex;
	for(char c : host_id)
		if(HexVal(c) >= 0)
			hex.push_back((char)tolower((unsigned char)c));
	if(hex.size() != 12)
		return host_id;
	uint8_t mac[6];
	HexDecode(hex, mac, 6);
	return MacToString(mac);
}

Settings::Settings()
{
	for(int i = 0; i < KeyboardMouse::ActionCount; i++)
		bindings[i] = KeyboardMouse::DefaultBinding(i);
}

// ---------------------------------------------------------------------------
// Picture presets

void Settings::ApplyMousePreset(int preset)
{
	//                          direct  pc-like smooth  raw
	static const float sm[] = { 0.30f,  0.50f,  0.80f,  0.50f };
	static const float cv[] = { 1.60f,  1.60f,  1.40f,  1.00f };
	static const float dz[] = { 0.18f,  0.18f,  0.15f,  0.00f };
	mouse_preset = preset;
	if(preset < 0 || preset > 3)
		return;
	kbm.smoothing = sm[preset];
	kbm.curve = cv[preset];
	kbm.anti_deadzone = dz[preset];
}

void Settings::ApplyDetailPreset(int preset)
{
	//                         off   natural sharp ultra maximum
	static const float sh[] = { 0.0f, 0.35f, 0.70f, 1.10f, 1.60f };
	static const float tx[] = { 0.0f, 0.00f, 0.25f, 0.50f, 0.85f };
	static const float cl[] = { 0.0f, 0.00f, 0.15f, 0.30f, 0.50f };
	static const float ov[] = { 0.0f, 0.00f, 0.00f, 0.20f, 0.45f };
	detail_preset = preset;
	if(preset < 0 || preset > 4)
		return; // custom keeps the current values
	ai_sharpness = sh[preset];
	sharpness = sh[preset];
	texture_boost = tx[preset];
	clarity = cl[preset];
	overshoot = ov[preset];
}

void Settings::ApplyPicturePreset(int preset)
{
	picture_preset = preset;
	switch(preset)
	{
		case 0: brightness = 0.0f;  contrast = 1.0f;  saturation = 1.0f;  vibrance = 0.0f;  break; // Natural
		case 1: brightness = 0.0f;  contrast = 1.06f; saturation = 1.08f; vibrance = 0.35f; break; // Vivid
		case 2: brightness = -0.02f; contrast = 1.04f; saturation = 0.94f; vibrance = 0.0f; break; // Cinema
		default: break; // Custom keeps the current values
	}
}

// ---------------------------------------------------------------------------
// Hosts

RegisteredHost *Settings::FindHost(const std::string &mac)
{
	for(auto &h : hosts)
		if(h.mac == mac)
			return &h;
	return nullptr;
}

void Settings::AddOrUpdateHost(const RegisteredHost &host)
{
	if(RegisteredHost *existing = FindHost(host.mac))
		*existing = host;
	else
		hosts.push_back(host);
}

void Settings::RemoveHost(const std::string &mac)
{
	hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [&](const RegisteredHost &h) { return h.mac == mac; }), hosts.end());
}

// ---------------------------------------------------------------------------
// Persistence: a small INI file.
//   [general]   key=value
//   [host.N]    key=value for each paired console
//   [manual.N]  address=...

static std::string Trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if(a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

void Settings::Load(const std::string &path)
{
	std::ifstream in(path);
	if(!in)
	{
		LOGI("No settings file yet, using defaults");
		return;
	}

	std::map<std::string, std::map<std::string, std::string>> sections;
	std::vector<std::string> order;
	std::string section = "general", line;
	while(std::getline(in, line))
	{
		line = Trim(line);
		if(line.empty() || line[0] == ';' || line[0] == '#')
			continue;
		if(line.front() == '[' && line.back() == ']')
		{
			section = line.substr(1, line.size() - 2);
			order.push_back(section);
			continue;
		}
		size_t eq = line.find('=');
		if(eq == std::string::npos)
			continue;
		sections[section][Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
	}

	auto &g = sections["general"];
	auto str = [&](const char *k, std::string &v) { if(g.count(k)) v = g[k]; };
	auto num = [&](const char *k, int &v) { if(g.count(k)) v = atoi(g[k].c_str()); };
	auto flt = [&](const char *k, float &v) { if(g.count(k)) v = (float)atof(g[k].c_str()); };
	auto bol = [&](const char *k, bool &v) { if(g.count(k)) v = g[k] == "1" || g[k] == "true"; };

	str("psn_account_id", psn_account_id);
	num("resolution", resolution);
	num("fps", fps);
	num("bitrate_kbps", bitrate_kbps);
	int codec_i = (int)codec; num("codec", codec_i); codec = codec_i == 0 ? VideoCodec::H264 : VideoCodec::H265;
	bol("hdr", hdr);
	bol("hdr_force", hdr_force);
	// v1.0 used booleans for upscaling and frame generation
	if(g.count("upscale") && !g.count("upscale_mode"))
		upscale_mode = (g["upscale"] == "1") ? 2 : 0;
	if(g.count("frame_gen") && !g.count("frame_gen_mode"))
		frame_gen = (g["frame_gen"] == "1") ? 1 : 0;
	num("upscale_mode", upscale_mode);
	num("upscale_target", upscale_target);
	flt("artifact_reduction", artifact_reduction);
	flt("sharpness", sharpness);
	flt("ai_sharpness", ai_sharpness);
	flt("texture_boost", texture_boost);
	flt("clarity", clarity);
	flt("overshoot", overshoot);
	if(g.count("detail_preset"))
		num("detail_preset", detail_preset);
	else
		detail_preset = 5; // settings from an older version: keep their values, show as custom
	flt("deband", deband);
	num("frame_gen_mode", frame_gen);
	frame_gen = frame_gen ? 1 : 0; // older versions: any "on" value becomes predictive
	if(g.count("frame_gen_v2"))
		frame_gen = std::clamp(atoi(g["frame_gen_v2"].c_str()), 0, 2);
	flt("paper_white_nits", paper_white_nits);
	bol("ai_hdr", ai_hdr);
	flt("hdr_boost", hdr_boost);
	num("aspect_mode", aspect_mode);
	bol("ambient", ambient);
	flt("ambient_intensity", ambient_intensity);
	num("ambient_style", ambient_style);
	bol("kbm_enabled", kbm_enabled);
	flt("mouse_sensitivity", kbm.sensitivity);
	flt("mouse_anti_deadzone", kbm.anti_deadzone);
	flt("mouse_smoothing", kbm.smoothing);
	flt("mouse_curve", kbm.curve);
	num("mouse_preset", mouse_preset);
	if(!g.count("mouse_preset"))
		ApplyMousePreset(1); // 1.5 retuned mouse-look: older files get the new defaults
	bol("mouse_invert_y", kbm.invert_y);
	for(int i = 0; i < KeyboardMouse::ActionCount; i++)
	{
		std::string key = "bind." + std::to_string(i);
		if(g.count(key))
			bindings[i] = KeyboardMouse::Parse(g[key]);
	}
	num("picture_preset", picture_preset);
	flt("brightness", brightness);
	flt("contrast", contrast);
	flt("saturation", saturation);
	flt("vibrance", vibrance);
	bol("auto_reconnect", auto_reconnect);
	bol("replay_enabled", replay_enabled);
	num("replay_seconds", replay_seconds);
	bol("fullscreen", fullscreen);
	bol("vsync", vsync);
	bol("show_stats", show_stats);
	bol("dualsense_features", dualsense_features);
	flt("haptics_gain", haptics_gain);
	flt("rumble_gain", rumble_gain);
	bol("adaptive_triggers", adaptive_triggers);
	bol("motion_controls", motion_controls);
	bol("view_is_touchpad", view_is_touchpad);
	flt("volume", volume);
	bol("microphone", microphone);

	hosts.clear();
	manual_hosts.clear();
	for(const auto &name : order)
	{
		auto &s = sections[name];
		if(name.rfind("host.", 0) == 0)
		{
			RegisteredHost h;
			h.mac = s["mac"];
			h.nickname = s["nickname"];
			h.last_address = s["address"];
			h.ps5 = s["ps5"] != "0";
			h.target = s.count("target") ? atoi(s["target"].c_str()) : CHIAKI_TARGET_PS5_1;
			h.rp_key_type = (uint32_t)strtoul(s["rp_key_type"].c_str(), nullptr, 10);
			h.console_pin = (uint32_t)strtoul(s["console_pin"].c_str(), nullptr, 10);
			bool ok = !h.mac.empty()
				&& HexDecode(s["regist_key"], h.regist_key, sizeof(h.regist_key))
				&& HexDecode(s["rp_key"], h.rp_key, sizeof(h.rp_key));
			if(ok)
				hosts.push_back(h);
			else
				LOGW("Skipping invalid saved console entry [%s]", name.c_str());
		}
		else if(name.rfind("manual.", 0) == 0 && !s["address"].empty())
		{
			manual_hosts.push_back({ s["address"] });
		}
	}
	LOGI("Loaded settings (%zu paired console(s))", hosts.size());
}

void Settings::Save(const std::string &path) const
{
	std::ostringstream o;
	o << "[general]\n";
	o << "psn_account_id=" << psn_account_id << "\n";
	o << "resolution=" << resolution << "\n";
	o << "fps=" << fps << "\n";
	o << "bitrate_kbps=" << bitrate_kbps << "\n";
	o << "codec=" << (int)codec << "\n";
	o << "hdr=" << hdr << "\n";
	o << "hdr_force=" << hdr_force << "\n";
	o << "upscale_mode=" << upscale_mode << "\n";
	o << "upscale_target=" << upscale_target << "\n";
	o << "artifact_reduction=" << artifact_reduction << "\n";
	o << "sharpness=" << sharpness << "\n";
	o << "ai_sharpness=" << ai_sharpness << "\n";
	o << "texture_boost=" << texture_boost << "\n";
	o << "clarity=" << clarity << "\n";
	o << "overshoot=" << overshoot << "\n";
	o << "detail_preset=" << detail_preset << "\n";
	o << "deband=" << deband << "\n";
	o << "frame_gen_mode=" << (frame_gen ? 1 : 0) << "\n";
	o << "frame_gen_v2=" << frame_gen << "\n";
	o << "paper_white_nits=" << paper_white_nits << "\n";
	o << "ai_hdr=" << ai_hdr << "\n";
	o << "hdr_boost=" << hdr_boost << "\n";
	o << "aspect_mode=" << aspect_mode << "\n";
	o << "ambient=" << ambient << "\n";
	o << "ambient_intensity=" << ambient_intensity << "\n";
	o << "ambient_style=" << ambient_style << "\n";
	o << "kbm_enabled=" << kbm_enabled << "\n";
	o << "mouse_sensitivity=" << kbm.sensitivity << "\n";
	o << "mouse_anti_deadzone=" << kbm.anti_deadzone << "\n";
	o << "mouse_smoothing=" << kbm.smoothing << "\n";
	o << "mouse_curve=" << kbm.curve << "\n";
	o << "mouse_preset=" << mouse_preset << "\n";
	o << "mouse_invert_y=" << kbm.invert_y << "\n";
	for(int i = 0; i < KeyboardMouse::ActionCount; i++)
		o << "bind." << i << "=" << KeyboardMouse::Serialize(bindings[i]) << "\n";
	o << "picture_preset=" << picture_preset << "\n";
	o << "brightness=" << brightness << "\n";
	o << "contrast=" << contrast << "\n";
	o << "saturation=" << saturation << "\n";
	o << "vibrance=" << vibrance << "\n";
	o << "auto_reconnect=" << auto_reconnect << "\n";
	o << "replay_enabled=" << replay_enabled << "\n";
	o << "replay_seconds=" << replay_seconds << "\n";
	o << "fullscreen=" << fullscreen << "\n";
	o << "vsync=" << vsync << "\n";
	o << "show_stats=" << show_stats << "\n";
	o << "dualsense_features=" << dualsense_features << "\n";
	o << "haptics_gain=" << haptics_gain << "\n";
	o << "rumble_gain=" << rumble_gain << "\n";
	o << "adaptive_triggers=" << adaptive_triggers << "\n";
	o << "motion_controls=" << motion_controls << "\n";
	o << "view_is_touchpad=" << view_is_touchpad << "\n";
	o << "volume=" << volume << "\n";
	o << "microphone=" << microphone << "\n";

	for(size_t i = 0; i < hosts.size(); i++)
	{
		const auto &h = hosts[i];
		o << "\n[host." << i << "]\n";
		o << "mac=" << h.mac << "\n";
		o << "nickname=" << h.nickname << "\n";
		o << "address=" << h.last_address << "\n";
		o << "ps5=" << (h.ps5 ? 1 : 0) << "\n";
		o << "target=" << h.target << "\n";
		o << "regist_key=" << HexEncode(h.regist_key, sizeof(h.regist_key)) << "\n";
		o << "rp_key=" << HexEncode(h.rp_key, sizeof(h.rp_key)) << "\n";
		o << "rp_key_type=" << h.rp_key_type << "\n";
		o << "console_pin=" << h.console_pin << "\n";
	}
	for(size_t i = 0; i < manual_hosts.size(); i++)
		o << "\n[manual." << i << "]\naddress=" << manual_hosts[i].address << "\n";

	std::string tmp = path + ".tmp";
	{
		std::ofstream out(tmp, std::ios::trunc);
		if(!out)
		{
			LOGE("Could not write settings to %s", path.c_str());
			return;
		}
		out << o.str();
	}
	std::remove(path.c_str());
	std::rename(tmp.c_str(), path.c_str());
}
