// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "audio.h"
#include "discovery.h"
#include "gamepad.h"
#include "registration.h"
#include "renderer.h"
#include "settings.h"
#include "stream_session.h"

#include <SDL3/SDL.h>

#include <memory>
#include <string>

class App
{
public:
	bool Init();
	void Run();
	void Shutdown();

private:
	enum class Screen { Consoles, Settings, Log };
	enum class Phase { Idle, Waking, Connecting, Streaming };

	// main loop pieces
	void PumpEvents();
	void HandleSessionEvents();
	void SendInput();
	void Tick();
	void BuildUi();
	void PresentFrame(bool new_frame);

	// actions
	void BeginConnect(const std::string &mac);
	void StartStream();
	void EndStream(const std::string &message, bool is_error);
	void SetFullscreen(bool fullscreen);
	void MarkSettingsDirty() { settings_dirty = true; }
	int FrameGenMultiplier() const;      // 1 = off / not possible on this display
	ProcessOptions MakeProcessOptions() const;
	PresentOptions MakePresentOptions(int sync_interval) const;
	void TakeScreenshot();

	// UI
	void SetupStyle(float scale);
	void UiSidebar();
	void UiConsoles();
	void UiSettings();
	void UiLog();
	void UiPairDialog();
	void UiStreamOverlay();
	void UiStats();
	void UiStreamDialogs();
	void UiConnecting();
	void UiMessage();
	void UiEnhancementControls(bool compact);
	void UiPictureControls(bool compact);

	std::string data_dir;
	std::string settings_path;
	Settings settings;
	bool settings_dirty = false;
	uint64_t last_settings_save = 0;

	SDL_Window *window = nullptr;
	HWND hwnd = nullptr;
	float ui_scale = 1.0f;
	bool quit = false;

	Renderer renderer;
	Audio audio;
	Gamepads gamepads;
	Discovery discovery;
	std::unique_ptr<StreamSession> session;
	Registration registration;
	PsnLogin psn_login;

	Screen screen = Screen::Consoles;
	Phase phase = Phase::Idle;
	RegisteredHost active_host;
	std::string active_address;
	uint64_t phase_deadline = 0;
	uint64_t last_wakeup = 0;

	// streaming state
	bool overlay_open = false;
	bool input_guard = false;          // wait for all buttons released after closing the overlay
	ChiakiControllerState last_sent = {};
	bool sent_idle = false;
	uint64_t last_haptics_try = 0;
	uint64_t present_count = 0, present_window_start = 0;
	double present_fps = 0.0;

	// dialogs
	bool pair_open = false;
	std::string pair_address;
	int pair_target = 0;
	char pin_buf[16] = {};
	char account_buf[64] = {};
	char redirect_buf[2048] = {};
	char manual_ip_buf[64] = {};
	bool login_pin_open = false, login_pin_incorrect = false;
	char login_pin_buf[16] = {};
	bool keyboard_open = false;
	char keyboard_buf[1024] = {};
	std::string message;
	bool message_is_error = false;

	// auto-reconnect
	int reconnect_attempts = 0;
	uint64_t reconnect_at = 0;

	// performance graph (ms between presented frames)
	float frame_times[180] = {};
	int frame_time_pos = 0;
	uint64_t last_present_us = 0;
	std::string toast;
	uint64_t toast_until = 0;

	uint64_t last_display_refresh = 0;
	uint64_t last_probe = 0;
};
