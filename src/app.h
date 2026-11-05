// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "audio.h"
#include "discovery.h"
#include "gamepad.h"
#include "kbm.h"
#include "frametimeline.h"
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
	bool FrameGenActive() const;
	void PullAndSubmit(bool frame_gen);
	void ProcessPendingFrames();
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
	void UiStreamSection();
	void UiUpscaleSection();
	void UiFrameGenSection();
	void UiPictureSection();
	void UiControlsSection();
	void UiSystemSection();
	void Row(const char *label);
	void RowHelp(const char *text);
	void SetRowLayout(bool compact);
	float row_label_w = 270.0f, row_item_w = 380.0f;

	// streaming helpers
	void ShowOverlay(bool open);
	void ShowToast(const std::string &text);
	bool HandleHotkey(SDL_Keycode key);
	bool StreamSettingsChanged() const;
	void ReconnectNow();
	void SyncBindingsToSettings();
	bool WantMouseCapture() const;
	void UpdateMouseCapture();

	std::string data_dir;
	std::string settings_path;
	Settings settings;
	bool settings_dirty = false;
	uint64_t last_settings_save = 0;

	SDL_Window *window = nullptr;
	HWND hwnd = nullptr;
	float ui_scale = 1.0f;
	struct ImFont *label_font = nullptr;   // large font for the compare / showcase labels
	bool quit = false;

	Renderer renderer;
	Audio audio;
	Gamepads gamepads;
	KeyboardMouse kbm;
	bool mouse_captured = false;
	bool window_focused = true;
	bool overlay_just_opened = false;
	bool compare_on = false;
	bool showcase_on = false;            // compare divider sweeps by itself
	uint64_t showcase_start_ms = 0;
	float compare_split = 0.5f;

	// frame generation
	FrameTimeline timeline;
	bool fg_active_last = false;
	int dup_streak = 0;
	uint64_t dup_count = 0, dup_window_start = 0;
	double dup_rate = 0.0;               // repeated frames skipped per second
	uint64_t last_shown_serial = 0;
	struct PendingFrame { uint64_t serial; double arrival_ms; uint64_t submitted_ms; };
	std::vector<PendingFrame> pending_frames;  // waiting for the GPU's repeated-frame check
	int slow_seconds = 0;                     // frame generation below the display rate
	bool slow_warned = false;
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
