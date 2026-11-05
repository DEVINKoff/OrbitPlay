// SPDX-License-Identifier: AGPL-3.0-only
#include "app.h"
#include "log.h"

#include <chiaki/common.h>
#include <chiaki/time.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_sdl3.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

static const char *kAppName = "Orbit Play";
static const uint64_t kWakeTimeoutMs = 60000;

static uint64_t NowMs() { return chiaki_time_now_monotonic_ms(); }

// ---------------------------------------------------------------------------
// Init / shutdown

bool App::Init()
{
	const char *appdata = getenv("APPDATA");
	data_dir = std::string(appdata ? appdata : ".") + "\\OrbitPlay";
	CreateDirectoryA(data_dir.c_str(), nullptr);
	settings_path = data_dir + "\\settings.ini";
	Log::Get().Open(data_dir + "\\orbitplay.log");
	LOGI("%s starting", kAppName);

	ChiakiErrorCode err = chiaki_lib_init();
	if(err != CHIAKI_ERR_SUCCESS)
	{
		LOGE("chiaki_lib_init failed: %s", chiaki_error_string(err));
		return false;
	}

	settings.Load(settings_path);
	for(int i = 0; i < KeyboardMouse::ActionCount; i++)
		kbm.bindings[i] = settings.bindings[i];
	strncpy(account_buf, settings.psn_account_id.c_str(), sizeof(account_buf) - 1);

	// DualSense: enhanced reports are needed for gyro/touchpad/effects over Bluetooth.
	SDL_SetHint("SDL_JOYSTICK_ENHANCED_REPORTS", "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
	{
		LOGE("SDL_Init failed: %s", SDL_GetError());
		return false;
	}

	window = SDL_CreateWindow(kAppName, 1600, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if(!window)
	{
		LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}
	hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
	if(!hwnd || !renderer.Init(hwnd))
	{
		LOGE("Renderer initialization failed");
		return false;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	io.IniFilename = nullptr;
	ui_scale = SDL_GetWindowDisplayScale(window);
	if(ui_scale <= 0.0f)
		ui_scale = 1.0f;
	SetupStyle(ui_scale);
	ImGui_ImplSDL3_InitForD3D(window);
	ImGui_ImplDX11_Init(renderer.Device(), renderer.Context());

	audio.SetVolume(settings.volume);
	discovery.Start();
	if(settings.fullscreen)
		SetFullscreen(true);
	present_window_start = NowMs();
	return true;
}

void App::Shutdown()
{
	if(session)
	{
		session->Stop();
		session.reset();
	}
	gamepads.StopAllFeedback();
	registration.Cancel();
	discovery.Stop();
	settings.Save(settings_path);
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	renderer.Shutdown();
	if(window)
		SDL_DestroyWindow(window);
	SDL_Quit();
	LOGI("Bye");
}

void App::SetupStyle(float scale)
{
	ImGuiIO &io = ImGui::GetIO();
	io.Fonts->Clear();
	ImFontConfig cfg;
	cfg.OversampleH = 2;
	const char *font = "C:\\Windows\\Fonts\\segoeui.ttf";
	if(GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES)
		io.Fonts->AddFontFromFileTTF(font, 19.0f * scale, &cfg);
	else
	{
		cfg.SizePixels = 15.0f * scale;
		io.Fonts->AddFontDefault(&cfg);
	}

	ImGuiStyle &s = ImGui::GetStyle();
	s = ImGuiStyle();
	ImGui::StyleColorsDark(&s);
	s.WindowRounding = 10.0f;
	s.ChildRounding = 10.0f;
	s.FrameRounding = 7.0f;
	s.GrabRounding = 7.0f;
	s.PopupRounding = 8.0f;
	s.FramePadding = ImVec2(12, 8);
	s.ItemSpacing = ImVec2(10, 9);
	s.WindowPadding = ImVec2(18, 16);
	s.ScrollbarSize = 12.0f;
	ImVec4 *c = s.Colors;
	ImVec4 accent(0.0f, 0.44f, 0.82f, 1.0f); // PlayStation blue
	c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.06f, 0.075f, 1.0f);
	c[ImGuiCol_ChildBg] = ImVec4(0.09f, 0.10f, 0.125f, 1.0f);
	c[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.98f);
	c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.07f);
	c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.19f, 1.0f);
	c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.20f, 0.25f, 1.0f);
	c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.23f, 0.30f, 1.0f);
	c[ImGuiCol_Button] = ImVec4(0.16f, 0.18f, 0.23f, 1.0f);
	c[ImGuiCol_ButtonHovered] = accent;
	c[ImGuiCol_ButtonActive] = ImVec4(0.0f, 0.36f, 0.70f, 1.0f);
	c[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.65f, 1.0f, 1.0f);
	c[ImGuiCol_SliderGrab] = accent;
	c[ImGuiCol_SliderGrabActive] = ImVec4(0.35f, 0.65f, 1.0f, 1.0f);
	c[ImGuiCol_Header] = ImVec4(0.16f, 0.18f, 0.23f, 1.0f);
	c[ImGuiCol_HeaderHovered] = ImVec4(0.0f, 0.44f, 0.82f, 0.6f);
	c[ImGuiCol_HeaderActive] = accent;
	c[ImGuiCol_NavCursor] = ImVec4(0.35f, 0.65f, 1.0f, 1.0f);
	c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
	c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
	s.ScaleAllSizes(scale);
}

void App::SetFullscreen(bool fullscreen)
{
	SDL_SetWindowFullscreen(window, fullscreen);
}

int App::FrameGenMultiplier() const
{
	if(settings.frame_gen == 0 || compare_on)
		return 1;
	int fps = session ? session->Config().fps : settings.fps;
	int hz = renderer.DisplayHz();
	int max_mult = 1;
	for(int m = 4; m >= 2; m--)
		if(fps * m <= hz + 5) // e.g. 60 fps x 4 = 240 Hz; tolerate 239.76 Hz modes
		{
			max_mult = m;
			break;
		}
	if(settings.frame_gen == 1)
		return max_mult;
	return std::min(settings.frame_gen, max_mult);
}

ProcessOptions App::MakeProcessOptions() const
{
	ProcessOptions o;
	o.upscale = (UpscaleMode)std::clamp(settings.upscale_mode, 0, 2);
	o.target_height = settings.upscale_target;
	o.artifact_reduction = settings.artifact_reduction;
	o.sharpness = settings.sharpness;
	o.ai_sharpness = settings.ai_sharpness;
	o.texture_boost = settings.texture_boost;
	o.clarity = settings.clarity;
	o.overshoot = settings.overshoot;
	o.deband = settings.deband;
	o.paper_white = settings.paper_white_nits;
	o.ai_hdr = settings.ai_hdr;
	o.frame_gen = FrameGenMultiplier() > 1;
	o.ambient = settings.ambient && settings.aspect_mode == 0;
	o.aspect = (AspectMode)std::clamp(settings.aspect_mode, 0, 2);
	return o;
}

PresentOptions App::MakePresentOptions(int sync_interval) const
{
	PresentOptions o;
	o.sync_interval = sync_interval;
	o.paper_white = settings.paper_white_nits;
	o.hdr_boost = settings.hdr_boost;
	o.ambient = (settings.ambient && settings.aspect_mode == 0) ? settings.ambient_intensity : 0.0f;
	o.aspect = (AspectMode)std::clamp(settings.aspect_mode, 0, 2);
	o.brightness = settings.brightness;
	o.contrast = settings.contrast;
	o.saturation = settings.saturation;
	o.vibrance = settings.vibrance;
	o.ambient_style = settings.ambient_style;
	o.compare = compare_on && phase == Phase::Streaming;
	o.compare_split = compare_split;
	return o;
}

void App::TakeScreenshot()
{
	if(!renderer.HasVideo())
		return;
	wchar_t profile[MAX_PATH] = {};
	DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
	std::wstring dir = (n ? std::wstring(profile) : std::wstring(L".")) + L"\\Pictures\\Orbit Play";
	CreateDirectoryW(dir.c_str(), nullptr);
	SYSTEMTIME st;
	GetLocalTime(&st);
	wchar_t name[64];
	swprintf(name, 64, L"\\OrbitPlay_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	if(renderer.SaveScreenshot(dir + name, MakePresentOptions(1)))
	{
		toast = "Screenshot saved to Pictures\\Orbit Play";
		toast_until = NowMs() + 2500;
	}
}

// ---------------------------------------------------------------------------
// Main loop

void App::Run()
{
	while(!quit)
	{
		UpdateMouseCapture();
		PumpEvents();
		Tick();
		HandleSessionEvents();
		SendInput();

		bool streaming = phase == Phase::Streaming && session;
		bool new_frame = false;
		if(streaming)
		{
			AVFrame *frame = session->PullFrame();
			if(frame)
			{
				new_frame = renderer.SubmitFrame(frame, MakeProcessOptions());
				av_frame_free(&frame);
			}
		}

		bool ui_needed = !streaming || overlay_open || settings.show_stats || login_pin_open || keyboard_open || !message.empty()
			|| NowMs() < toast_until
			|| !renderer.HasVideo();
		if(streaming && !new_frame && !ui_needed)
		{
			// Nothing to draw: wait briefly for the decoder, keep polling input at ~500 Hz.
			WaitForSingleObject(session->FrameEvent(), 2);
			continue;
		}
		PresentFrame(new_frame);
	}
}

void App::PumpEvents()
{
	SDL_Event e;
	while(SDL_PollEvent(&e))
	{
		bool kbm_event = e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_MOUSE_MOTION
			|| e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL;
		// Rebinding a key: the next key / button goes to the binding, not the UI or the game
		if(kbm_event && kbm.CapturingAction() >= 0 && kbm.HandleCapture(e))
		{
			if(kbm.CapturingAction() < 0)
				SyncBindingsToSettings();
			continue;
		}
		// While the mouse is captured, keyboard and mouse play the game
		if(kbm_event && mouse_captured)
			kbm.HandleEvent(e);
		else
			ImGui_ImplSDL3_ProcessEvent(&e);
		gamepads.HandleEvent(e);
		switch(e.type)
		{
			case SDL_EVENT_WINDOW_FOCUS_GAINED:
				window_focused = true;
				break;
			case SDL_EVENT_WINDOW_FOCUS_LOST:
				window_focused = false;
				kbm.Release();
				break;
			case SDL_EVENT_QUIT:
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				quit = true;
				break;
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				renderer.Resize(e.window.data1, e.window.data2);
				break;
			case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
			case SDL_EVENT_WINDOW_MOVED:
				last_display_refresh = 0;
				break;
			case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
			{
				float s = SDL_GetWindowDisplayScale(window);
				if(s > 0.0f && std::fabs(s - ui_scale) > 0.01f)
				{
					ui_scale = s;
					SetupStyle(ui_scale);
					ImGui_ImplDX11_InvalidateDeviceObjects();
				}
				break;
			}
			case SDL_EVENT_KEY_DOWN:
				if(e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))
				{
					settings.fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
					SetFullscreen(settings.fullscreen);
					MarkSettingsDirty();
				}
				else if(phase == Phase::Streaming && (e.key.key == SDLK_F1 || e.key.key == SDLK_ESCAPE) && !keyboard_open && !login_pin_open
					&& !e.key.repeat)
				{
					// Esc inside an open dropdown just closes the dropdown
					if(!(overlay_open && e.key.key == SDLK_ESCAPE && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)))
						ShowOverlay(!overlay_open);
				}
				else if(!e.key.repeat && !keyboard_open && !login_pin_open)
					HandleHotkey(e.key.key);
				break;
			default:
				break;
		}
	}
	gamepads.SetOptions({ settings.motion_controls, settings.view_is_touchpad, settings.adaptive_triggers, settings.rumble_gain });
}

void App::Tick()
{
	uint64_t now = NowMs();

	if(now - last_display_refresh > 2000)
	{
		renderer.RefreshDisplayInfo();
		last_display_refresh = now;
	}

	// Probe consoles by IP: manual entries and the last address of paired consoles.
	if(now - last_probe > 1500 && phase != Phase::Streaming)
	{
		for(const auto &m : settings.manual_hosts)
			discovery.ProbeAddress(m.address);
		for(const auto &h : settings.hosts)
			discovery.ProbeAddress(h.last_address);
		last_probe = now;
	}

	if(settings_dirty && now - last_settings_save > 1000)
	{
		settings.Save(settings_path);
		settings_dirty = false;
		last_settings_save = now;
	}

	// Waking: wait for the console to report "ready", re-sending the wakeup packet now and then.
	if(phase == Phase::Waking)
	{
		DiscoveredHost d;
		bool found = discovery.Find(active_host.mac, &d);
		if(found && d.ready)
		{
			active_address = d.address;
			StartStream();
		}
		else if(now > phase_deadline)
		{
			phase = Phase::Idle;
			message = "The console didn't wake up. Check that \"Enable turning on PS5 from network\" is on in "
				"Settings > System > Power Saving > Features Available in Rest Mode.";
			message_is_error = true;
		}
		else if(now - last_wakeup > 5000)
		{
			Discovery::Wakeup(found ? d.address : active_address, active_host.regist_key, active_host.ps5);
			last_wakeup = now;
		}
	}

	// Connecting -> Streaming once the first frame arrives (handled in HandleSessionEvents)
	if(phase == Phase::Connecting && now > phase_deadline)
		EndStream("Connection timed out. Make sure Remote Play is enabled on the console.", true);

	// Hot-plugged DualSense: (re)try opening the haptics audio device
	if(phase == Phase::Streaming && session && settings.dualsense_features && !audio.HapticsActive()
		&& now - last_haptics_try > 3000 && gamepads.AnyWiredDualSense())
	{
		audio.OpenHaptics();
		last_haptics_try = now;
	}

	if(session)
		session->SetHapticsGain(settings.haptics_gain);

	if(reconnect_at && now >= reconnect_at && phase == Phase::Idle)
	{
		reconnect_at = 0;
		BeginConnect(active_host.mac);
	}

	if(now - present_window_start >= 1000)
	{
		present_fps = present_count * 1000.0 / (double)(now - present_window_start);
		present_count = 0;
		present_window_start = now;
	}
}

void App::HandleSessionEvents()
{
	if(!session)
		return;
	for(const SessionEvent &e : session->TakeEvents())
	{
		switch(e.type)
		{
			case SessionEvent::Connected:
				phase = Phase::Streaming;
				reconnect_attempts = 0;
				overlay_open = false;
				if(settings.kbm_enabled)
					ShowToast("Keyboard & mouse ready - Esc or F1 for the menu");
				input_guard = true;
				gamepads.SetMicLed(false);
				if(settings.dualsense_features && gamepads.AnyWiredDualSense())
					audio.OpenHaptics();
				break;
			case SessionEvent::Quit:
			{
				// Unexpected drop (Wi-Fi hiccup, console busy): try to get back in automatically.
				bool was_streaming = phase == Phase::Streaming;
				if(e.flag && was_streaming && settings.auto_reconnect && reconnect_attempts < 3)
				{
					reconnect_attempts++;
					EndStream("", false);
					reconnect_at = NowMs() + 2000;
					toast = "Connection lost - reconnecting (" + std::to_string(reconnect_attempts) + "/3)...";
					toast_until = reconnect_at + 4000;
					return;
				}
				EndStream(e.flag ? "Disconnected: " + e.text : "", e.flag);
				return; // session is gone
			}
			case SessionEvent::Rumble:
				gamepads.SetRumble(e.a, e.b);
				break;
			case SessionEvent::HapticRumble:
				gamepads.SetHapticRumble(e.strength);
				break;
			case SessionEvent::TriggerEffects:
				gamepads.SetTriggerEffects(e.a, e.left, e.b, e.right);
				break;
			case SessionEvent::LedColor:
				gamepads.SetLed(e.rgb);
				break;
			case SessionEvent::PlayerIndex:
				gamepads.SetPlayerIndex(e.a);
				break;
			case SessionEvent::MotionReset:
				gamepads.ResetMotion();
				break;
			case SessionEvent::Intensity:
				gamepads.SetIntensity(e.a);
				break;
			case SessionEvent::LoginPinRequest:
				login_pin_open = true;
				login_pin_incorrect = e.flag;
				memset(login_pin_buf, 0, sizeof(login_pin_buf));
				break;
			case SessionEvent::KeyboardOpen:
				keyboard_open = true;
				memset(keyboard_buf, 0, sizeof(keyboard_buf));
				strncpy(keyboard_buf, e.text.c_str(), sizeof(keyboard_buf) - 1);
				break;
			case SessionEvent::KeyboardClose:
				keyboard_open = false;
				break;
			case SessionEvent::Nickname:
				if(!e.text.empty())
					active_host.nickname = e.text;
				break;
		}
	}
}

void App::SendInput()
{
	if(!session || phase != Phase::Streaming)
		return;

	if(gamepads.TakeMenuCombo())
		ShowOverlay(!overlay_open);
	if(gamepads.TakeMicButton())
	{
		bool muted = !session->MicMuted();
		session->SetMicMuted(muted);
		gamepads.SetMicLed(!muted);
	}

	// While the overlay or a dialog has focus, the pad drives the UI instead of the console.
	ImGuiIO &io = ImGui::GetIO();
	bool ui_focus = overlay_open || keyboard_open || login_pin_open;
	if(ui_focus)
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
	else
		io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);

	ChiakiControllerState state = gamepads.State();
	if(mouse_captured)
	{
		kbm.SetTuning(settings.kbm);
		ChiakiControllerState k = kbm.Update();
		ChiakiControllerState merged;
		chiaki_controller_state_or(&merged, &state, &k);
		state = merged;
	}
	if(input_guard)
	{
		if(state.buttons == 0 && state.l2_state < 16 && state.r2_state < 16)
			input_guard = false;
		else
			ui_focus = true;
	}
	if(ui_focus)
	{
		if(!sent_idle)
		{
			ChiakiControllerState idle;
			chiaki_controller_state_set_idle(&idle);
			session->SetControllerState(idle);
			last_sent = idle;
			sent_idle = true;
		}
		return;
	}
	sent_idle = false;
	if(!chiaki_controller_state_equals(&state, &last_sent))
	{
		session->SetControllerState(state);
		last_sent = state;
	}
}

void App::PresentFrame(bool new_frame)
{
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	BuildUi();
	ImGui::Render();
	renderer.BeginOverlay();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	bool streaming = phase == Phase::Streaming && session;
	int mult = streaming ? FrameGenMultiplier() : 1;
	PresentOptions opt = MakePresentOptions((settings.vsync || mult > 1 || !streaming) ? 1 : 0);

	auto present = [&](VideoView view) {
		renderer.Present(view, opt);
		present_count++;
		uint64_t now_us = chiaki_time_now_monotonic_us();
		if(last_present_us)
		{
			frame_times[frame_time_pos] = (float)((now_us - last_present_us) / 1000.0);
			frame_time_pos = (frame_time_pos + 1) % (int)(sizeof(frame_times) / sizeof(frame_times[0]));
		}
		last_present_us = now_us;
	};

	if(mult > 1 && new_frame && renderer.PrepareMotion())
	{
		// Show (mult - 1) generated frames on the next refreshes, then the real frame:
		// 2x = 120 fps, 3x = 180 fps, 4x = 240 fps from a 60 fps stream.
		for(int k = 1; k < mult; k++)
		{
			if(!renderer.BuildInterpolatedFrame((float)k / (float)mult))
				break;
			present(VideoView::Interpolated);
			PumpEvents();
			SendInput();
		}
		present(VideoView::Latest);
	}
	else
		present(streaming || phase == Phase::Connecting ? VideoView::Latest : VideoView::None);
}

// ---------------------------------------------------------------------------
// Connecting

void App::BeginConnect(const std::string &mac)
{
	RegisteredHost *h = settings.FindHost(mac);
	if(!h)
		return;
	active_host = *h;
	active_address = h->last_address;
	message.clear();

	DiscoveredHost d;
	bool found = discovery.Find(mac, &d);
	if(found)
	{
		active_address = d.address;
		if(d.address != h->last_address)
		{
			h->last_address = d.address;
			MarkSettingsDirty();
		}
	}
	if(active_address.empty())
	{
		message = "This console hasn't been seen on the network yet. Turn it on, or add it by IP address.";
		message_is_error = true;
		return;
	}
	if(found && d.ready)
	{
		StartStream();
		return;
	}
	// In rest mode (or not answering discovery): wake it and wait.
	Discovery::Wakeup(active_address, active_host.regist_key, active_host.ps5);
	last_wakeup = NowMs();
	phase = Phase::Waking;
	phase_deadline = NowMs() + kWakeTimeoutMs;
}

void App::StartStream()
{
	StreamConfig cfg;
	cfg.host = active_host;
	cfg.address = active_address;
	cfg.psn_account_id = settings.psn_account_id;
	cfg.resolution = settings.resolution;
	cfg.fps = settings.fps;
	cfg.bitrate_kbps = settings.bitrate_kbps;
	cfg.codec = settings.codec;
	cfg.hdr = settings.hdr && (renderer.DisplayHdr() || settings.hdr_force);
	cfg.dualsense = settings.dualsense_features;
	cfg.haptics_gain = settings.haptics_gain;

	session = std::make_unique<StreamSession>();
	renderer.ResetVideo();
	std::string error;
	if(!session->Start(cfg, renderer.Device(), renderer.Context(), &audio, &error))
	{
		session.reset();
		phase = Phase::Idle;
		message = error;
		message_is_error = true;
		return;
	}
	phase = Phase::Connecting;
	phase_deadline = NowMs() + 30000;
	chiaki_controller_state_set_idle(&last_sent);
	sent_idle = false;
}

void App::EndStream(const std::string &msg, bool is_error)
{
	reconnect_at = 0;
	if(session)
	{
		session->Stop();
		session.reset();
	}
	gamepads.StopAllFeedback();
	audio.CloseGame();
	audio.CloseHaptics();
	renderer.ResetVideo();
	phase = Phase::Idle;
	overlay_open = keyboard_open = login_pin_open = false;
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
	kbm.Release();
	kbm.BeginCapture(-1);
	UpdateMouseCapture();
	message = msg;
	message_is_error = is_error;
}

// ---------------------------------------------------------------------------
// UI

static void SectionTitle(const char *text)
{
	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.55f, 0.72f, 1.0f, 1.0f), "%s", text);
	ImGui::Separator();
}

static bool PrimaryButton(const char *label, const ImVec2 &size = ImVec2(0, 0))
{
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.44f, 0.82f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.53f, 0.92f, 1.0f));
	bool r = ImGui::Button(label, size);
	ImGui::PopStyleColor(2);
	return r;
}

static void HelpText(const char *text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.65f, 0.72f, 1.0f));
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

void App::BuildUi()
{
	if(phase == Phase::Streaming && renderer.HasVideo())
	{
		if(settings.show_stats)
			UiStats();
		if(overlay_open)
			UiStreamOverlay();
		UiStreamDialogs();
		UiMessage();
		return;
	}

	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->Pos);
	ImGui::SetNextWindowSize(vp->Size);
	ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoBringToFrontOnFocus);

	UiSidebar();
	ImGui::SameLine();
	ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);
	switch(screen)
	{
		case Screen::Consoles: UiConsoles(); break;
		case Screen::Settings: UiSettings(); break;
		case Screen::Log: UiLog(); break;
	}
	ImGui::EndChild();
	ImGui::End();

	if(phase == Phase::Waking || phase == Phase::Connecting || (phase == Phase::Streaming && !renderer.HasVideo()))
		UiConnecting();
	UiPairDialog();
	UiStreamDialogs();
	UiMessage();
}

void App::UiSidebar()
{
	float w = 230.0f * ui_scale;
	ImGui::BeginChild("##sidebar", ImVec2(w, 0), ImGuiChildFlags_Borders);
	ImGui::Dummy(ImVec2(0, 6 * ui_scale));
	ImGui::SetWindowFontScale(1.35f);
	ImGui::TextColored(ImVec4(0.35f, 0.65f, 1.0f, 1.0f), "Orbit Play");
	ImGui::SetWindowFontScale(1.0f);
	HelpText("PS5 Remote Play");
	ImGui::Dummy(ImVec2(0, 14 * ui_scale));

	auto nav = [&](const char *label, Screen s) {
		bool sel = screen == s;
		if(ImGui::Selectable(label, sel, 0, ImVec2(0, 34 * ui_scale)))
			screen = s;
	};
	nav("  Consoles", Screen::Consoles);
	nav("  Settings", Screen::Settings);
	nav("  Log", Screen::Log);

	ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 150 * ui_scale);
	ImGui::Separator();
	HelpText("Controllers");
	auto names = gamepads.Names();
	if(names.empty())
		ImGui::TextDisabled("None connected");
	for(auto &n : names)
		ImGui::TextUnformatted(n.c_str());
	ImGui::Text("Display: %d Hz %s", renderer.DisplayHz(), renderer.DisplayHdr() ? "HDR" : "SDR");
	ImGui::EndChild();
}

void App::UiConsoles()
{
	ImGui::SetWindowFontScale(1.25f);
	ImGui::TextUnformatted("Consoles");
	ImGui::SetWindowFontScale(1.0f);
	HelpText("Your PS5 must be on the same network with Remote Play enabled (Settings > System > Remote Play).");
	ImGui::Spacing();

	std::vector<DiscoveredHost> found = discovery.Hosts();
	auto discovered = [&](const std::string &mac) -> const DiscoveredHost * {
		for(auto &d : found)
			if(d.mac == mac)
				return &d;
		return nullptr;
	};

	float card_h = 128.0f * ui_scale;
	int idx = 0;
	std::string forget_mac;

	// Paired consoles first
	for(auto &h : settings.hosts)
	{
		const DiscoveredHost *d = discovered(h.mac);
		ImGui::PushID(idx++);
		ImGui::BeginChild("##card", ImVec2(0, card_h), ImGuiChildFlags_Borders);
		ImGui::SetWindowFontScale(1.15f);
		ImGui::TextUnformatted(h.nickname.empty() ? "PlayStation 5" : h.nickname.c_str());
		ImGui::SetWindowFontScale(1.0f);
		if(d)
		{
			ImGui::TextColored(d->ready ? ImVec4(0.4f, 0.9f, 0.5f, 1) : ImVec4(1.0f, 0.75f, 0.3f, 1),
				d->ready ? "Ready" : "Rest mode");
			ImGui::SameLine();
			ImGui::TextDisabled("%s  |  %s", d->address.c_str(), d->ps5 ? "PS5" : "PS4");
			if(!d->running_app.empty())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("|  Playing %s", d->running_app.c_str());
			}
		}
		else
			ImGui::TextDisabled("Not found right now  |  last seen at %s", h.last_address.empty() ? "?" : h.last_address.c_str());

		ImGui::Spacing();
		bool busy = phase != Phase::Idle;
		ImGui::BeginDisabled(busy);
		if(PrimaryButton(d && !d->ready ? "Wake & Play" : "Play", ImVec2(150 * ui_scale, 0)))
			BeginConnect(h.mac);
		ImGui::SameLine();
		if(ImGui::Button("Wake"))
			Discovery::Wakeup(d ? d->address : h.last_address, h.regist_key, h.ps5);
		ImGui::SameLine();
		if(ImGui::Button("Forget"))
			forget_mac = h.mac;
		ImGui::EndDisabled();
		ImGui::EndChild();
		ImGui::PopID();
	}
	if(!forget_mac.empty())
	{
		settings.RemoveHost(forget_mac);
		MarkSettingsDirty();
	}

	// Discovered but not paired
	for(auto &d : found)
	{
		if(settings.FindHost(d.mac))
			continue;
		ImGui::PushID(idx++);
		ImGui::BeginChild("##card", ImVec2(0, card_h), ImGuiChildFlags_Borders);
		ImGui::SetWindowFontScale(1.15f);
		ImGui::TextUnformatted(d.name.empty() ? "PlayStation" : d.name.c_str());
		ImGui::SetWindowFontScale(1.0f);
		ImGui::TextDisabled("%s  |  %s  |  %s  |  not paired", d.address.c_str(), d.ps5 ? "PS5" : "PS4", d.ready ? "Ready" : "Rest mode");
		ImGui::Spacing();
		ImGui::BeginDisabled(!d.ready || phase != Phase::Idle);
		if(PrimaryButton("Pair", ImVec2(150 * ui_scale, 0)))
		{
			pair_open = true;
			pair_address = d.address;
			pair_target = chiaki_target_is_unknown((ChiakiTarget)d.target) ? (d.ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10) : d.target;
			memset(pin_buf, 0, sizeof(pin_buf));
		}
		ImGui::EndDisabled();
		if(!d.ready)
		{
			ImGui::SameLine();
			HelpText("Turn the console fully on to pair it.");
		}
		ImGui::EndChild();
		ImGui::PopID();
	}

	if(settings.hosts.empty() && found.empty())
	{
		ImGui::Spacing();
		ImGui::TextDisabled("Searching for consoles on your network...");
	}

	SectionTitle("Add a console by IP address");
	HelpText("Use this if your PS5 doesn't show up (e.g. it's on another subnet, or the PC has VPN adapters). "
		"The PS5's IP is in Settings > Network > Connection Status.");
	ImGui::SetNextItemWidth(260 * ui_scale);
	ImGui::InputTextWithHint("##ip", "192.168.1.50", manual_ip_buf, sizeof(manual_ip_buf));
	ImGui::SameLine();
	if(ImGui::Button("Add") && manual_ip_buf[0])
	{
		std::string ip = manual_ip_buf;
		bool exists = false;
		for(auto &m : settings.manual_hosts)
			exists |= m.address == ip;
		if(!exists)
			settings.manual_hosts.push_back({ ip });
		discovery.ProbeAddress(ip);
		memset(manual_ip_buf, 0, sizeof(manual_ip_buf));
		MarkSettingsDirty();
	}
	for(size_t i = 0; i < settings.manual_hosts.size(); i++)
	{
		ImGui::PushID((int)(1000 + i));
		ImGui::BulletText("%s", settings.manual_hosts[i].address.c_str());
		ImGui::SameLine();
		if(ImGui::SmallButton("Remove"))
		{
			settings.manual_hosts.erase(settings.manual_hosts.begin() + i);
			MarkSettingsDirty();
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}

}

void App::UiPairDialog()
{
	if(!pair_open)
		return;
	ImGui::SetNextWindowSize(ImVec2(620 * ui_scale, 0));
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	if(!ImGui::IsPopupOpen("Pair console"))
		ImGui::OpenPopup("Pair console");
	if(!ImGui::BeginPopupModal("Pair console", nullptr, ImGuiWindowFlags_NoResize))
		return;

	ImGui::Text("Console: %s", pair_address.c_str());
	ImGui::Spacing();

	SectionTitle("1. Your PSN Account ID");
	HelpText("Remote Play needs the ID of the PSN account signed in on the PS5. Sign in once to fetch it.");
	ImGui::SetNextItemWidth(-1);
	if(ImGui::InputTextWithHint("##acc", "Account ID (base64, e.g. AbCdEfGhIjk=)", account_buf, sizeof(account_buf)))
	{
		settings.psn_account_id = account_buf;
		MarkSettingsDirty();
	}
	if(ImGui::Button("Sign in with PSN in browser"))
		PsnLogin::OpenLoginPage();
	HelpText("After signing in you'll land on a blank page. Copy that page's full address and paste it here:");
	ImGui::SetNextItemWidth(-140 * ui_scale);
	ImGui::InputTextWithHint("##redir", "https://remoteplay.dl.playstation.net/remoteplay/redirect?code=...", redirect_buf, sizeof(redirect_buf));
	ImGui::SameLine();
	ImGui::BeginDisabled(psn_login.GetState() == PsnLogin::State::Running);
	if(ImGui::Button("Get ID", ImVec2(-1, 0)))
		psn_login.Start(redirect_buf);
	ImGui::EndDisabled();
	switch(psn_login.GetState())
	{
		case PsnLogin::State::Running:
			ImGui::TextDisabled("Contacting Sony...");
			break;
		case PsnLogin::State::Success:
		{
			std::string id = psn_login.AccountId();
			if(id != account_buf)
			{
				strncpy(account_buf, id.c_str(), sizeof(account_buf) - 1);
				settings.psn_account_id = id;
				MarkSettingsDirty();
			}
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), "Account ID fetched.");
			break;
		}
		case PsnLogin::State::Failed:
			ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", psn_login.Error().c_str());
			break;
		default:
			break;
	}

	SectionTitle("2. Link code from the PS5");
	HelpText("On the PS5: Settings > System > Remote Play > Link Device. Enter the 8-digit code shown.");
	ImGui::SetNextItemWidth(220 * ui_scale);
	ImGui::InputTextWithHint("##pin", "12345678", pin_buf, 9, ImGuiInputTextFlags_CharsDecimal);

	ImGui::Spacing();
	Registration::State rs = registration.GetState();
	bool can_pair = strlen(pin_buf) == 8 && strlen(account_buf) > 0 && rs != Registration::State::Running;
	ImGui::BeginDisabled(!can_pair);
	if(PrimaryButton("Pair", ImVec2(160 * ui_scale, 0)))
		registration.Start(pair_address, pair_target, account_buf, (uint32_t)strtoul(pin_buf, nullptr, 10));
	ImGui::EndDisabled();
	ImGui::SameLine();
	if(ImGui::Button("Close", ImVec2(120 * ui_scale, 0)))
	{
		registration.Cancel();
		pair_open = false;
		ImGui::CloseCurrentPopup();
	}

	if(rs == Registration::State::Running)
		ImGui::TextDisabled("Pairing...");
	else if(rs == Registration::State::Failed)
		ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", registration.Error().c_str());
	else if(rs == Registration::State::Success)
	{
		RegisteredHost h = registration.Result();
		settings.AddOrUpdateHost(h);
		settings.Save(settings_path);
		registration.Cancel();
		pair_open = false;
		message = "Paired with " + (h.nickname.empty() ? std::string("your console") : h.nickname) + ". Press Play to start.";
		message_is_error = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}

void App::Row(const char *label)
{
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	ImGui::SameLine(row_label_w);
	ImGui::SetNextItemWidth(row_item_w);
}

void App::RowHelp(const char *text)
{
	ImGui::SetCursorPosX(row_label_w);
	HelpText(text);
}

void App::SetRowLayout(bool compact)
{
	row_label_w = (compact ? 210.0f : 270.0f) * ui_scale;
	row_item_w = compact ? -FLT_MIN : 380.0f * ui_scale;
}

void App::UiSettings()
{
	ImGui::SetWindowFontScale(1.25f);
	ImGui::TextUnformatted("Settings");
	ImGui::SetWindowFontScale(1.0f);
	HelpText("Everything here can also be changed while playing: open the stream menu with Create + PS, F1 or Esc.");
	SetRowLayout(false);

	SectionTitle("Stream");
	UiStreamSection();
	SectionTitle("Upscaling");
	UiUpscaleSection();
	SectionTitle("Frame generation");
	UiFrameGenSection();
	SectionTitle("Picture & ambient light");
	UiPictureSection();
	SectionTitle("Keyboard, mouse & controller");
	UiControlsSection();
	SectionTitle("Display & audio");
	UiSystemSection();

	SectionTitle("Account");
	Row("PSN Account ID");
	if(ImGui::InputText("##acc2", account_buf, sizeof(account_buf)))
	{
		settings.psn_account_id = account_buf;
		MarkSettingsDirty();
	}
	ImGui::Dummy(ImVec2(0, 30 * ui_scale));
}

bool App::StreamSettingsChanged() const
{
	if(!session)
		return false;
	const StreamConfig &c = session->Config();
	bool want_hdr = settings.hdr && (renderer.DisplayHdr() || settings.hdr_force);
	return c.resolution != settings.resolution || c.fps != settings.fps || c.bitrate_kbps != settings.bitrate_kbps
		|| c.codec != settings.codec || c.hdr != want_hdr || c.dualsense != settings.dualsense_features;
}

void App::ReconnectNow()
{
	EndStream("", false);
	reconnect_attempts = 0;
	reconnect_at = NowMs() + 300;
	ShowToast("Reconnecting with the new settings...");
}

void App::UiStreamSection()
{
	bool changed = false;
	Row("Resolution");
	const char *res_names[] = { "720p", "1080p (max for PS5 Remote Play)" };
	int res_idx = settings.resolution >= 1080 ? 1 : 0;
	if(ImGui::Combo("##res", &res_idx, res_names, 2))
	{
		settings.resolution = res_idx == 1 ? 1080 : 720;
		changed = true;
	}
	Row("Frame rate");
	const char *fps_names[] = { "30 fps", "60 fps" };
	int fps_idx = settings.fps >= 60 ? 1 : 0;
	if(ImGui::Combo("##fps", &fps_idx, fps_names, 2))
	{
		settings.fps = fps_idx == 1 ? 60 : 30;
		changed = true;
	}
	Row("Codec");
	const char *codec_names[] = { "H.264", "H.265 / HEVC (better quality, needed for HDR)" };
	int codec_idx = (int)settings.codec;
	if(ImGui::Combo("##codec", &codec_idx, codec_names, 2))
	{
		settings.codec = (VideoCodec)codec_idx;
		changed = true;
	}
	Row("Bitrate");
	int mbps = settings.bitrate_kbps / 1000;
	if(ImGui::SliderInt("##bitrate", &mbps, 0, 50, mbps == 0 ? "Auto (preset)" : "%d Mbps"))
	{
		settings.bitrate_kbps = mbps * 1000;
		changed = true;
	}
	Row("HDR");
	changed |= ImGui::Checkbox("Stream in HDR10 when the display is in HDR mode", &settings.hdr);
	ImGui::SetCursorPosX(row_label_w);
	changed |= ImGui::Checkbox("Also on SDR displays (tone-mapped on the PC)", &settings.hdr_force);
	RowHelp(renderer.DisplayHdr() ? "This display is in HDR mode." :
		"This display is in SDR mode. Turn on \"Use HDR\" in Windows Settings > Display to get real HDR.");
	Row("SDR / UI brightness in HDR");
	changed |= ImGui::SliderFloat("##pw", &settings.paper_white_nits, 80.0f, 400.0f, "%.0f nits");
	if(session)
	{
		if(StreamSettingsChanged())
		{
			ImGui::SetCursorPosX(row_label_w);
			if(PrimaryButton("Reconnect now to apply"))
				ReconnectNow();
			RowHelp("Resolution, frame rate, codec, bitrate and HDR are agreed with the PS5 when connecting. "
				"Reconnecting takes a few seconds and keeps your game running.");
		}
		else
			RowHelp("These are the settings of the current stream.");
	}
	if(changed)
		MarkSettingsDirty();
}

void App::UiUpscaleSection()
{
	bool changed = false;
	bool ai_available = renderer.AiSuperResolutionAvailable();
	bool nvidia = renderer.GpuVendorName() == "NVIDIA";

	Row("Upscaling");
	std::string ai_name = ai_available ? "AI super resolution (" + std::string(nvidia ? "NVIDIA RTX" : "Intel") + ")"
		: std::string("AI super resolution (needs NVIDIA RTX / Intel)");
	const char *modes[] = { "Off", "Enhanced (any GPU)", ai_name.c_str() };
	changed |= ImGui::Combo("##upmode", &settings.upscale_mode, modes, 3);

	ImGui::BeginDisabled(settings.upscale_mode == 0);
	Row("Upscale to");
	const char *targets[] = { "Match display", "1440p (2K)", "1800p", "2160p (4K)" };
	const int target_values[] = { 0, 1440, 1800, 2160 };
	int ti = 0;
	for(int i = 0; i < 4; i++)
		if(settings.upscale_target == target_values[i])
			ti = i;
	if(ImGui::Combo("##uptarget", &ti, targets, 4))
	{
		settings.upscale_target = target_values[ti];
		changed = true;
	}

	bool ai = settings.upscale_mode == 2 && ai_available;
	Row("Detail level");
	const char *levels[] = { "Off", "Natural", "Sharp", "Ultra", "Maximum", "Custom" };
	int level = std::clamp(settings.detail_preset, 0, 5);
	if(ImGui::Combo("##detail", &level, levels, 6))
	{
		settings.ApplyDetailPreset(level);
		changed = true;
	}
	bool custom = false;
	Row("Sharpness");
	float &sh = ai ? settings.ai_sharpness : settings.sharpness;
	custom |= ImGui::SliderFloat("##sharp", &sh, 0.0f, 2.0f, sh < 0.01f ? "Off" : (sh > 1.0f ? "%.2f (extreme)" : "%.2f"));
	Row("Texture");
	custom |= ImGui::SliderFloat("##texture", &settings.texture_boost, 0.0f, 1.0f, settings.texture_boost < 0.01f ? "Off" : "%.2f");
	Row("Clarity");
	custom |= ImGui::SliderFloat("##clarity", &settings.clarity, 0.0f, 1.0f, settings.clarity < 0.01f ? "Off" : "%.2f");
	Row("Punch (allow halo)");
	custom |= ImGui::SliderFloat("##overshoot", &settings.overshoot, 0.0f, 1.0f, settings.overshoot < 0.01f ? "Off (no halos)" : "%.2f");
	if(custom)
	{
		settings.detail_preset = 5;
		changed = true;
	}
	RowHelp("Sharpness tightens edges, Texture brings out fine surface detail, Clarity adds depth with local contrast. "
		"Ultra and Maximum push far past what RTX does alone; they also show more compression noise.");
	Row("Deband");
	changed |= ImGui::SliderFloat("##deband", &settings.deband, 0.0f, 1.0f, settings.deband < 0.01f ? "Off" : "%.2f");
	ImGui::BeginDisabled(ai);
	Row("Artifact reduction");
	changed |= ImGui::SliderFloat("##artifact", &settings.artifact_reduction, 0.0f, 1.0f, "%.2f");
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	if(settings.upscale_mode == 2 && ai_available && nvidia)
		RowHelp("RTX Video Super Resolution rebuilds detail, then sharpening (contrast-adaptive, ignores noise) adds bite on top. "
			"In the NVIDIA App > Graphics > RTX Video Enhancement, set Super Resolution to quality 4 for the cleanest result.");
	else if(settings.upscale_mode == 2 && ai_available)
		RowHelp("Uses Intel Video Super Resolution (Arc or Iris Xe with a current driver), plus contrast-adaptive sharpening.");
	else if(settings.upscale_mode == 2)
		RowHelp("This GPU has no driver AI upscaler, so the Enhanced upscaler is used.");
	else if(settings.upscale_mode == 1)
		RowHelp("Cleans compression artifacts and banding, upscales along edges, then sharpens only real detail.");
	RowHelp("Deband smooths the stair-step bands compression leaves in skies, fog and dark scenes.");

	Row("Compare");
	if(ImGui::Checkbox("Split screen: original | enhanced (F6)", &compare_on))
		ShowToast(compare_on ? "Compare on: original left, enhanced right" : "Compare off");
	RowHelp("With the menu open you can drag the divider with the mouse. F7 / F8 step the detail level live.");

	Row("AI HDR for SDR games");
	ImGui::BeginDisabled(!renderer.AiHdrAvailable());
	changed |= ImGui::Checkbox(renderer.AiHdrAvailable() ? "NVIDIA RTX Video HDR##aihdr" : "Needs an NVIDIA RTX GPU##aihdr", &settings.ai_hdr);
	ImGui::EndDisabled();
	Row("HDR boost (any GPU)");
	changed |= ImGui::SliderFloat("##hdrboost", &settings.hdr_boost, 0.0f, 1.0f, settings.hdr_boost < 0.01f ? "Off" : "%.2f");
	RowHelp("For SDR games on an HDR display: expands highlights toward your display's peak brightness.");

	ImGui::Spacing();
	RowHelp(("Now: " + renderer.UpscalerStatus()).c_str());
	if(changed)
		MarkSettingsDirty();
}

void App::UiFrameGenSection()
{
	bool changed = false;
	Row("Frame generation");
	const char *fg[] = { "Off", "Auto (best for this display)", "2x (120 fps)", "3x (180 fps)", "4x (240 fps)" };
	changed |= ImGui::Combo("##fg", &settings.frame_gen, fg, 5);
	int mult = FrameGenMultiplier();
	int fps = session ? session->Config().fps : settings.fps;
	char buf[320];
	if(compare_on && settings.frame_gen != 0)
		snprintf(buf, sizeof(buf), "Paused while split-screen compare is on.");
	else if(settings.frame_gen == 0)
		snprintf(buf, sizeof(buf), "Your display runs at %d Hz.", renderer.DisplayHz());
	else if(mult <= 1)
		snprintf(buf, sizeof(buf), "Needs a display at %d Hz or more (this one is %d Hz).", fps * 2, renderer.DisplayHz());
	else
		snprintf(buf, sizeof(buf), "Active: %d fps on your %d Hz display. Generated frames add about %.0f ms of latency "
			"and can show artifacts on fast, complex motion.", fps * mult, renderer.DisplayHz(), 1000.0 / fps * (mult - 1) / mult);
	RowHelp(buf);
	if(changed)
		MarkSettingsDirty();
}

void App::UiPictureSection()
{
	bool changed = false;
	Row("Aspect ratio");
	const char *aspects[] = { "Fit (16:9, with ambient bars)", "Fill (crop to screen)", "Stretch" };
	changed |= ImGui::Combo("##aspect", &settings.aspect_mode, aspects, 3);
	ImGui::BeginDisabled(settings.aspect_mode != 0);
	Row("Ambient lighting");
	changed |= ImGui::Checkbox("Light the bars with the picture's colors (F5)##amb", &settings.ambient);
	ImGui::BeginDisabled(!settings.ambient);
	Row("Ambient style");
	const char *styles[] = { "Glow (soft colored light)", "Extend (blurred picture continues)" };
	changed |= ImGui::Combo("##ambstyle", &settings.ambient_style, styles, 2);
	Row("Ambient intensity");
	changed |= ImGui::SliderFloat("##ambi", &settings.ambient_intensity, 0.1f, 1.0f, "%.2f");
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	RowHelp("Shows on ultrawide screens or any window that isn't 16:9. Calculated in real light levels, so it matches in SDR and HDR.");

	Row("Picture mode");
	const char *presets[] = { "Natural", "Vivid", "Cinema", "Custom" };
	int preset = settings.picture_preset;
	if(ImGui::Combo("##preset", &preset, presets, 4))
	{
		settings.ApplyPicturePreset(preset);
		changed = true;
	}
	bool custom = false;
	Row("Brightness");
	custom |= ImGui::SliderFloat("##bri", &settings.brightness, -0.2f, 0.2f, "%+.2f");
	Row("Contrast");
	custom |= ImGui::SliderFloat("##con", &settings.contrast, 0.7f, 1.3f, "%.2f");
	Row("Saturation");
	custom |= ImGui::SliderFloat("##sat", &settings.saturation, 0.0f, 1.6f, "%.2f");
	Row("Vibrance");
	custom |= ImGui::SliderFloat("##vib", &settings.vibrance, 0.0f, 1.0f, "%.2f");
	if(custom)
	{
		settings.picture_preset = 3;
		changed = true;
	}
	if(changed)
		MarkSettingsDirty();
}

void App::UiControlsSection()
{
	bool changed = false;
	ImGui::TextColored(ImVec4(0.55f, 0.72f, 1.0f, 1.0f), "Keyboard & mouse");
	Row("Keyboard & mouse");
	changed |= ImGui::Checkbox("Play with keyboard and mouse##kbm", &settings.kbm_enabled);
	RowHelp("While playing, the mouse is captured and moves the right stick. Esc or F1 frees the mouse and opens this menu. "
		"A gamepad keeps working alongside.");
	ImGui::BeginDisabled(!settings.kbm_enabled);
	Row("Mouse sensitivity");
	changed |= ImGui::SliderFloat("##msens", &settings.kbm.sensitivity, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
	Row("Anti-deadzone");
	changed |= ImGui::SliderFloat("##madz", &settings.kbm.anti_deadzone, 0.0f, 0.5f, "%.2f");
	RowHelp("Games ignore small stick movements. Raise this if slow mouse movements don't turn the camera, lower it if aiming feels jumpy.");
	Row("Mouse smoothing");
	changed |= ImGui::SliderFloat("##msmooth", &settings.kbm.smoothing, 0.0f, 1.0f, "%.2f");
	Row("Invert Y");
	changed |= ImGui::Checkbox("##minv", &settings.kbm.invert_y);

	Row("Key bindings");
	if(ImGui::Button("Reset to defaults"))
	{
		kbm.ResetBindings();
		SyncBindingsToSettings();
	}
	RowHelp("Click a binding, then press a key, mouse button or scroll the wheel. Esc cancels, Delete clears.");
	for(int i = 0; i < KeyboardMouse::ActionCount; i++)
	{
		ImGui::PushID(2000 + i);
		Row(KeyboardMouse::ActionName(i));
		bool capturing = kbm.CapturingAction() == i;
		std::string label = capturing ? std::string("Press a key...") : KeyboardMouse::BindingName(kbm.bindings[i]);
		if(capturing)
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.44f, 0.82f, 1.0f));
		if(ImGui::Button(label.c_str(), ImVec2(row_item_w < 0 ? -FLT_MIN : row_item_w, 0)) && !capturing)
			kbm.BeginCapture(i);
		if(capturing)
			ImGui::PopStyleColor();
		ImGui::PopID();
	}
	ImGui::EndDisabled();

	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.55f, 0.72f, 1.0f, 1.0f), "Controller");
	Row("DualSense features");
	changed |= ImGui::Checkbox("Haptic feedback and adaptive triggers", &settings.dualsense_features);
	RowHelp("Full haptics need the DualSense on a USB cable; over Bluetooth it falls back to rumble. Adaptive triggers work either way.");
	Row("Haptics intensity");
	changed |= ImGui::SliderFloat("##hg", &settings.haptics_gain, 0.0f, 2.0f, "%.2fx");
	Row("Adaptive triggers");
	changed |= ImGui::Checkbox("Enabled##trig", &settings.adaptive_triggers);
	Row("Motion controls");
	changed |= ImGui::Checkbox("Gyro / accelerometer", &settings.motion_controls);
	Row("Rumble strength");
	changed |= ImGui::SliderFloat("##rg", &settings.rumble_gain, 0.0f, 2.0f, "%.2fx");
	Row("Xbox-style controllers");
	changed |= ImGui::Checkbox("View button acts as touchpad press", &settings.view_is_touchpad);
	RowHelp("Open the stream menu with Create + PS (View + Guide on Xbox pads).");
	if(changed)
		MarkSettingsDirty();
}

void App::UiSystemSection()
{
	bool changed = false;
	Row("Fullscreen");
	bool fs = settings.fullscreen;
	if(ImGui::Checkbox("Borderless fullscreen (F11)", &fs))
	{
		settings.fullscreen = fs;
		SetFullscreen(fs);
		changed = true;
	}
	Row("V-Sync");
	changed |= ImGui::Checkbox("Sync to the display (off = lowest latency, may tear)", &settings.vsync);
	Row("Statistics");
	changed |= ImGui::Checkbox("Show stream statistics while playing (F2)", &settings.show_stats);
	Row("Reconnect");
	changed |= ImGui::Checkbox("Reconnect automatically if the stream drops", &settings.auto_reconnect);
	Row("Volume");
	if(ImGui::SliderFloat("##vol", &settings.volume, 0.0f, 1.5f, "%.2f"))
	{
		audio.SetVolume(settings.volume);
		changed = true;
	}
	Row("Microphone");
	changed |= ImGui::Checkbox("Allow voice chat (DualSense mute button or F10)", &settings.microphone);

	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.55f, 0.72f, 1.0f, 1.0f), "Hotkeys while playing");
	static const char *keys[][2] = {
		{ "Esc / F1", "Stream menu (frees the mouse)" }, { "F2", "Statistics" }, { "F3", "Cycle upscaling" },
		{ "F4", "Cycle frame generation" }, { "F5", "Ambient light on/off" }, { "F6", "Split-screen compare" },
		{ "F7 / F8", "Detail level - / +   (Ctrl: fine sharpness)" }, { "F9", "Cycle picture mode" }, { "F10", "Microphone on/off" },
		{ "F11", "Fullscreen" }, { "F12", "Screenshot" },
	};
	for(auto &k : keys)
	{
		Row(k[0]);
		ImGui::TextUnformatted(k[1]);
	}
	if(changed)
		MarkSettingsDirty();
}

void App::UiLog()
{
	ImGui::SetWindowFontScale(1.25f);
	ImGui::TextUnformatted("Log");
	ImGui::SetWindowFontScale(1.0f);
	HelpText(("Saved to " + data_dir + "\\orbitplay.log").c_str());
	ImGui::BeginChild("##logtext", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	for(const auto &line : Log::Get().Recent())
	{
		bool err = line.find("[E]") != std::string::npos;
		bool warn = line.find("[W]") != std::string::npos;
		if(err)
			ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "%s", line.c_str());
		else if(warn)
			ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", line.c_str());
		else
			ImGui::TextUnformatted(line.c_str());
	}
	if(ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
		ImGui::SetScrollHereY(1.0f);
	ImGui::EndChild();
}

void App::UiConnecting()
{
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(440 * ui_scale, 0));
	ImGui::Begin("##connecting", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	const char *what = phase == Phase::Waking ? "Waking up" : "Connecting to";
	ImGui::Text("%s %s...", what, active_host.nickname.empty() ? "your PS5" : active_host.nickname.c_str());
	float t = (float)ImGui::GetTime();
	ImGui::ProgressBar(-1.0f * t, ImVec2(-1, 6 * ui_scale), "");
	if(ImGui::Button("Cancel", ImVec2(-1, 0)))
	{
		if(phase == Phase::Waking)
			phase = Phase::Idle;
		else
			EndStream("", false);
	}
	ImGui::End();
}

void App::UiStats()
{
	if(!session)
		return;
	StreamStats s = session->Stats();
	ImGui::SetNextWindowPos(ImVec2(16 * ui_scale, 16 * ui_scale));
	ImGui::SetNextWindowBgAlpha(0.65f);
	ImGui::Begin("##stats", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
	ImGui::Text("Stream   %dx%d  %s  %s", renderer.VideoWidth(), renderer.VideoHeight(), s.codec.c_str(),
		renderer.VideoHdr() ? "HDR10" : "SDR");
	ImGui::Text("Output   %dx%d  %s  %d Hz", renderer.OutputVideoWidth(), renderer.OutputVideoHeight(),
		renderer.OutputHdr() ? "HDR10" : "SDR", renderer.DisplayHz());
	ImGui::Text("Upscale  %s", renderer.UpscalerStatus().c_str());
	int mult = FrameGenMultiplier();
	char fg[48] = "";
	if(mult > 1)
		snprintf(fg, sizeof(fg), "  (frame gen %dx)", mult);
	ImGui::Text("Decode   %.1f fps    Present %.1f fps%s", s.decode_fps, present_fps, fg);
	ImGui::Text("Network  %.1f Mbps   RTT %.1f ms   lost frames %llu", s.mbps, s.rtt_ms, (unsigned long long)s.frames_lost);
	ImGui::Text("Audio    %d ms queued   Haptics %s   Mic %s", audio.QueuedMs(),
		audio.HapticsActive() ? "DualSense" : "rumble", session->MicMuted() ? "off" : "on");
	ImGui::Text("Input    %zu controller(s)   keyboard & mouse %s", gamepads.Count(),
		!settings.kbm_enabled ? "off" : (mouse_captured ? "active" : "paused (menu open)"));
	// Frame pacing graph: time between presented frames (flat line = smooth)
	const int n = (int)(sizeof(frame_times) / sizeof(frame_times[0]));
	float worst = 0.0f;
	for(int i = 0; i < n; i++)
		worst = std::max(worst, frame_times[i]);
	char overlay_txt[48];
	snprintf(overlay_txt, sizeof(overlay_txt), "frame time (max %.1f ms)", worst);
	ImGui::PlotLines("##ft", frame_times, n, frame_time_pos, overlay_txt, 0.0f, std::max(34.0f, worst),
		ImVec2(420 * ui_scale, 60 * ui_scale));
	ImGui::End();
}

void App::UiStreamOverlay()
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImVec2 size(std::min(860.0f * ui_scale, vp->Size.x - 40.0f), std::min(vp->Size.y * 0.88f, 1000.0f * ui_scale));
	ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(size, ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.94f);
	// Focus only when the menu opens: focusing every frame would close dropdowns as soon as they open.
	if(overlay_just_opened)
	{
		ImGui::SetNextWindowFocus();
		overlay_just_opened = false;
	}
	ImGui::Begin("##streammenu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

	// B on the pad closes the menu, unless it is closing a dropdown
	bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
	if(!popup_open && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && kbm.CapturingAction() < 0)
		ShowOverlay(false);

	ImGui::SetWindowFontScale(1.2f);
	ImGui::TextUnformatted(active_host.nickname.empty() ? "PlayStation 5" : active_host.nickname.c_str());
	ImGui::SetWindowFontScale(1.0f);
	ImGui::SameLine();
	int mult = FrameGenMultiplier();
	ImGui::TextDisabled("   %dx%d -> %dx%d   %s   %d fps", renderer.VideoWidth(), renderer.VideoHeight(),
		renderer.OutputVideoWidth(), renderer.OutputVideoHeight(), renderer.VideoHdr() ? "HDR" : "SDR",
		(session ? session->Config().fps : 60) * std::max(1, mult));

	float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
	if(PrimaryButton("Resume", ImVec2(bw, 0)))
		ShowOverlay(false);
	ImGui::SameLine();
	bool mic_ok = session && settings.microphone;
	ImGui::BeginDisabled(!mic_ok);
	bool muted = !session || session->MicMuted();
	if(ImGui::Button(muted ? "Mic: off" : "Mic: on", ImVec2(bw, 0)) && session)
	{
		session->SetMicMuted(!muted);
		gamepads.SetMicLed(muted);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if(ImGui::Button("Screenshot", ImVec2(bw, 0)))
		TakeScreenshot();
	ImGui::SameLine();
	if(ImGui::Button(compare_on ? "Compare: on" : "Compare: off", ImVec2(bw, 0)))
		compare_on = !compare_on;

	if(StreamSettingsChanged())
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.20f, 0.35f, 1.0f));
		ImGui::BeginChild("##reconnect", ImVec2(0, ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2),
			ImGuiChildFlags_Borders);
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Stream settings changed - they apply after reconnecting.");
		ImGui::SameLine();
		if(PrimaryButton("Reconnect now"))
			ReconnectNow();
		ImGui::EndChild();
		ImGui::PopStyleColor();
		if(!session)
		{
			ImGui::End();
			return;
		}
	}

	float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
	SetRowLayout(true);
	if(ImGui::BeginTabBar("##menutabs"))
	{
		auto tab = [&](const char *name, auto &&body) {
			if(ImGui::BeginTabItem(name))
			{
				ImGui::BeginChild("##tabbody", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_None);
				body();
				ImGui::EndChild();
				ImGui::EndTabItem();
			}
		};
		tab("Image", [&] { UiUpscaleSection(); });
		tab("Motion", [&] { UiFrameGenSection(); });
		tab("Picture", [&] { UiPictureSection(); });
		tab("Controls", [&] { UiControlsSection(); });
		tab("Stream", [&] { UiStreamSection(); });
		tab("System", [&] { UiSystemSection(); });
		ImGui::EndTabBar();
	}

	ImGui::Separator();
	float fw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
	if(ImGui::Button("PS Home screen", ImVec2(fw, 0)) && session)
	{
		session->GoHome();
		ShowOverlay(false);
	}
	ImGui::SameLine();
	if(ImGui::Button("Put console in rest mode", ImVec2(fw, 0)) && session)
	{
		session->GotoBed();
		EndStream("", false);
	}
	ImGui::SameLine();
	if(ImGui::Button("Disconnect", ImVec2(fw, 0)))
		EndStream("", false);
	ImGui::End();

	// Drag the compare divider with the mouse (outside the menu)
	if(compare_on && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureMouse)
	{
		float rect[4];
		renderer.GetVideoRect((AspectMode)settings.aspect_mode, rect);
		if(rect[2] > 0)
			compare_split = std::clamp((ImGui::GetIO().MousePos.x - rect[0]) / rect[2], 0.02f, 0.98f);
	}
}

void App::ShowOverlay(bool open)
{
	if(open == overlay_open)
		return;
	overlay_open = open;
	if(open)
	{
		overlay_just_opened = true;
		kbm.Release();
	}
	else
	{
		kbm.BeginCapture(-1);
		input_guard = true;
	}
}

void App::ShowToast(const std::string &text)
{
	toast = text;
	toast_until = NowMs() + 2200;
}

bool App::HandleHotkey(SDL_Keycode key)
{
	if(phase != Phase::Streaming || !session)
		return false;
	char buf[160];
	switch(key)
	{
		case SDLK_F2:
			settings.show_stats = !settings.show_stats;
			break;
		case SDLK_F3:
		{
			settings.upscale_mode = (settings.upscale_mode + 1) % 3;
			const char *names[] = { "Off", "Enhanced", renderer.AiSuperResolutionAvailable() ? "AI super resolution" : "AI (not available - Enhanced)" };
			ShowToast(std::string("Upscaling: ") + names[settings.upscale_mode]);
			break;
		}
		case SDLK_F4:
		{
			settings.frame_gen = (settings.frame_gen + 1) % 5;
			int mult = FrameGenMultiplier();
			if(settings.frame_gen == 0)
				ShowToast("Frame generation: off");
			else
			{
				snprintf(buf, sizeof(buf), "Frame generation: %s -> %d fps", settings.frame_gen == 1 ? "auto" : "manual",
					session->Config().fps * std::max(1, mult));
				ShowToast(buf);
			}
			break;
		}
		case SDLK_F5:
			settings.ambient = !settings.ambient;
			ShowToast(settings.ambient ? "Ambient light on" : "Ambient light off");
			break;
		case SDLK_F6:
			compare_on = !compare_on;
			ShowToast(compare_on ? "Compare: original left | enhanced right (F7/F8 detail level)" : "Compare off");
			return true;
		case SDLK_F7:
		case SDLK_F8:
		{
			if(settings.upscale_mode == 0)
			{
				ShowToast("Turn on upscaling (F3) to use detail enhancement");
				return true;
			}
			bool ai = settings.upscale_mode == 2 && renderer.AiSuperResolutionAvailable();
			const char *levels[] = { "Off", "Natural", "Sharp", "Ultra", "Maximum", "Custom" };
			if(SDL_GetModState() & SDL_KMOD_CTRL)
			{
				// fine tune sharpness
				float &v = ai ? settings.ai_sharpness : settings.sharpness;
				v = std::clamp(v + (key == SDLK_F8 ? 0.05f : -0.05f), 0.0f, 2.0f);
				settings.detail_preset = 5;
				snprintf(buf, sizeof(buf), "Sharpness %.2f%s", v, ai ? " (on top of RTX)" : "");
			}
			else
			{
				int cur = settings.detail_preset > 4 ? 2 : settings.detail_preset;
				settings.ApplyDetailPreset(std::clamp(cur + (key == SDLK_F8 ? 1 : -1), 0, 4));
				snprintf(buf, sizeof(buf), "Detail: %s%s", levels[settings.detail_preset], ai ? " (on top of RTX)" : "");
			}
			ShowToast(buf);
			break;
		}
		case SDLK_F9:
		{
			settings.ApplyPicturePreset((std::min(settings.picture_preset, 2) + 1) % 3);
			const char *names[] = { "Natural", "Vivid", "Cinema" };
			ShowToast(std::string("Picture mode: ") + names[settings.picture_preset]);
			break;
		}
		case SDLK_F10:
			if(settings.microphone)
			{
				bool m = !session->MicMuted();
				session->SetMicMuted(m);
				gamepads.SetMicLed(!m);
				ShowToast(m ? "Microphone off" : "Microphone on");
			}
			return true;
		case SDLK_F12:
			TakeScreenshot();
			return true;
		default:
			return false;
	}
	MarkSettingsDirty();
	return true;
}

void App::SyncBindingsToSettings()
{
	for(int i = 0; i < KeyboardMouse::ActionCount; i++)
		settings.bindings[i] = kbm.bindings[i];
	MarkSettingsDirty();
}

bool App::WantMouseCapture() const
{
	return phase == Phase::Streaming && session && settings.kbm_enabled && window_focused
		&& !overlay_open && !keyboard_open && !login_pin_open && renderer.HasVideo();
}

void App::UpdateMouseCapture()
{
	bool want = WantMouseCapture();
	if(want == mouse_captured)
		return;
	SDL_SetWindowRelativeMouseMode(window, want);
	mouse_captured = want;
	if(!want)
		kbm.Release();
}

void App::UiStreamDialogs()
{
	if(login_pin_open && session)
	{
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(440 * ui_scale, 0));
		ImGui::Begin("Console login passcode", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
		if(login_pin_incorrect)
			ImGui::TextColored(ImVec4(1, 0.45f, 0.45f, 1), "Incorrect passcode, try again.");
		HelpText("This PS5 user has a login passcode. Enter it to continue.");
		ImGui::SetNextItemWidth(-1);
		if(ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputText("##lpin", login_pin_buf, 5, ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_Password
			| ImGuiInputTextFlags_EnterReturnsTrue);
		if((PrimaryButton("OK", ImVec2(120 * ui_scale, 0)) || enter) && strlen(login_pin_buf) == 4)
		{
			session->SetLoginPin(login_pin_buf);
			login_pin_open = false;
			input_guard = true;
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(120 * ui_scale, 0)))
		{
			login_pin_open = false;
			EndStream("", false);
		}
		ImGui::End();
	}

	if(keyboard_open && session)
	{
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(560 * ui_scale, 0));
		ImGui::Begin("Text input", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
		HelpText("The PS5 asked for text. Type it here and press Enter.");
		ImGui::SetNextItemWidth(-1);
		if(ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputText("##kb", keyboard_buf, sizeof(keyboard_buf), ImGuiInputTextFlags_EnterReturnsTrue);
		if(PrimaryButton("Send", ImVec2(120 * ui_scale, 0)) || enter)
		{
			session->KeyboardSetText(keyboard_buf);
			session->KeyboardAccept();
			keyboard_open = false;
			input_guard = true;
		}
		ImGui::SameLine();
		if(ImGui::Button("Cancel", ImVec2(120 * ui_scale, 0)))
		{
			session->KeyboardReject();
			keyboard_open = false;
			input_guard = true;
		}
		ImGui::End();
	}
}

void App::UiMessage()
{
	if(!toast.empty() && NowMs() < toast_until)
	{
		ImGuiViewport *tv = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(tv->GetCenter().x, tv->Pos.y + 24 * ui_scale), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.8f);
		ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
		ImGui::TextUnformatted(toast.c_str());
		ImGui::End();
	}
	if(message.empty())
		return;
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->Pos.y + vp->Size.y - 40 * ui_scale), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(std::min(760.0f * ui_scale, vp->Size.x - 40), 0));
	ImGui::PushStyleColor(ImGuiCol_WindowBg, message_is_error ? ImVec4(0.30f, 0.08f, 0.09f, 0.96f) : ImVec4(0.06f, 0.20f, 0.35f, 0.96f));
	ImGui::Begin("##message", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_AlwaysAutoResize);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(message.c_str());
	ImGui::PopTextWrapPos();
	if(ImGui::Button("OK", ImVec2(100 * ui_scale, 0)))
		message.clear();
	ImGui::End();
	ImGui::PopStyleColor();
}
