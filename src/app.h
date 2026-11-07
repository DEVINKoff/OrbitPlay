// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "audio.h"
#include "discovery.h"
#include "gamepad.h"
#include "kbm.h"
#include "frametimeline.h"
#include "recorder.h"
#include "registration.h"
#include "renderer.h"
#include "settings.h"
#include "stream_session.h"
#include "uisound.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <map>
#include <memory>
#include <string>

// What the app was doing, written into crash reports (set by the app, read by the crash handler).
extern char g_crash_context[256];

class App
{
public:
	bool Init(const std::string &resume_mac = "");
	void Run();
	void Shutdown();

	// The GPU was reset: main() restarts the app, which then reconnects to this console.
	bool RelaunchRequested() const { return relaunch; }
	const std::string &RelaunchResumeMac() const { return relaunch_mac; }
	// Mini player hit-testing (drag the borderless window)
	bool MiniMode() const { return mini_mode; }

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
	void UiTopBar();
	void UiHome();
	void UiBootIntro();
	void UiControlCenterBar();
	void UiInterfaceSection();
	void UiGameSection();
	void DrawPs5Background(const ImVec4 &tint);
	static ImVec4 HostColor(const std::string &id);
	static std::string FormatDuration(uint64_t seconds);
	static std::string SanitizeKey(const std::string &s);
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
	std::string CapturePath(const char *prefix);
	void ToggleRecording();
	void SaveReplay();
	void UiCaptureSection();
	void DrawRecordingBadge();

	// v1.6
	void ToggleMiniPlayer();
	bool TryAutoReconnect(const std::string &why);
	void OnGameChanged(const std::string &game);
	void FlushPlaytime();
	void CheckCrashMarker();
	void UpdateWatchdog(uint64_t now);
	void UpdateBatteryWarnings(uint64_t now);

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
	bool menu_reset_pos = true;          // center the stream menu next time it opens
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
	uint64_t last_fg_present_ms = 0;
	struct PendingFrame { uint64_t serial; double arrival_ms; uint64_t submitted_ms; };
	std::vector<PendingFrame> pending_frames;  // waiting for the GPU's repeated-frame check
	int slow_seconds = 0;                     // frame generation below the display rate
	bool slow_warned = false;
	Discovery discovery;
	Recorder recorder;
	uint64_t last_replay_idr_ms = 0;
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

	// v1.6: PS5-style home, sounds, intro
	UiSound sounds;
	uint64_t boot_start_ms = 0;
	int home_sel = 0;
	bool home_focus_pending = true;
	float home_scroll = 0.0f;
	std::map<std::string, float> tile_anim;
	ImVec4 bg_tint = ImVec4(0.0f, 0.35f, 0.85f, 1.0f);
	std::string forget_mac;

	// mini player
	bool mini_mode = false;
	bool mini_was_fullscreen = false;
	int mini_prev_x = 0, mini_prev_y = 0, mini_prev_w = 1600, mini_prev_h = 900;

	// current game, play time, session summary
	std::string current_game;
	uint64_t game_since_ms = 0;
	uint64_t last_game_check = 0;
	uint64_t stream_start_ms = 0;
	double stat_fps_sum = 0.0, stat_rtt_sum = 0.0;
	int stat_samples = 0;
	uint64_t stat_lost = 0;

	// stability
	uint64_t last_keyframe_req = 0;
	bool frozen_warned = false;
	uint64_t last_battery_check = 0;
	std::map<std::string, bool> battery_warned;
	bool relaunch = false;
	std::string relaunch_mac;
	std::string resume_mac;
};
