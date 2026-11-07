// SPDX-License-Identifier: AGPL-3.0-only
#include "app.h"
#include "log.h"
#include "icons.h"
#include "ui_style.h"

#include <chiaki/common.h>
#include <chiaki/time.h>

#include "platform/platform.h"

#ifdef _WIN32
#include <shellapi.h>
#endif

#include <imgui.h>
#include <imgui_impl_sdl3.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

static const char *kAppName = "Orbit Play";
static const uint64_t kWakeTimeoutMs = 60000;

static uint64_t NowMs() { return chiaki_time_now_monotonic_ms(); }

// ---------------------------------------------------------------------------
// Init / shutdown

bool App::Init(const std::string &resume)
{
	resume_mac = resume;
	data_dir = platform::DataDir();
	settings_path = platform::JoinPath(data_dir, "settings.ini");
	Log::Get().Open(platform::JoinPath(data_dir, "orbitplay.log"));
	LOGI("%s starting", kAppName);

	ChiakiErrorCode err = chiaki_lib_init();
	if(err != CHIAKI_ERR_SUCCESS)
	{
		LOGE("chiaki_lib_init failed: %s", chiaki_error_string(err));
		return false;
	}

	settings.Load(settings_path);
	if(settings.last_version.empty() && settings.upscale_mode == 1
		&& platform::FileExists(platform::JoinPath(data_dir, "crash.dmp")))
	{
		settings.upscale_mode = 2;
		LOGI("Restored AI upscaling (1.6.0 had switched it off after a crash report)");
	}
	if(settings.last_version == "1.7.0")
		settings.win_w = settings.win_h = 0; // 1.7.0 saved a window rect that could be off screen: start fresh
	settings.last_version = "1.7.1";
	MarkSettingsDirty();
	CheckCrashMarker();
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

#ifdef _WIN32
	// Created hidden: the app's own title bar is installed and the last size/position restored
	// before the window first appears.
	window = SDL_CreateWindow(kAppName, 1600, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN
		| SDL_WINDOW_BORDERLESS);
	if(!window)
	{
		LOGE("SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}
	hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
	if(hwnd)
	{
		InstallChrome();
		RestoreWindowPlacement();
	}
	if(!hwnd || !renderer.Init(hwnd))
	{
		LOGE("Renderer initialization failed");
		return false;
	}
#else
	// iPad: one full-screen Metal window
	window = SDL_CreateWindow(kAppName, 0, 0, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL
		| SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
	if(!window || !renderer.Init(window))
	{
		LOGE("Window / renderer initialization failed: %s", SDL_GetError());
		return false;
	}
#endif

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	io.IniFilename = nullptr;
#ifdef _WIN32
	// Windows: the interface works in pixels; scale with the display's DPI
	ui_scale = SDL_GetWindowDisplayScale(window);
	font_density = 1.0f;
#else
	// iPad: the interface works in points (fonts are rasterized at the screen's pixel density);
	// a little larger than on a PC for touch
	ui_scale = 1.15f;
	font_density = std::max(1.0f, SDL_GetWindowPixelDensity(window));
#endif
	if(ui_scale <= 0.0f)
		ui_scale = 1.0f;
	SetupStyle(ui_scale);
#ifdef _WIN32
	ImGui_ImplSDL3_InitForD3D(window);
#else
	ImGui_ImplSDL3_InitForMetal(window);
#endif
	renderer.ImGuiInit();

	audio.SetVolume(settings.volume);
	sounds.Init();
	sounds.SetEnabled(settings.ui_sounds);
	sounds.SetVolume(settings.ui_sound_volume);
	discovery.Start();
	if(settings.fullscreen)
		SetFullscreen(true);
	present_window_start = NowMs();
	if(!resume_mac.empty())
	{
		// Restarted after a GPU reset: go straight back into the game
		ShowToast("The graphics driver restarted - reconnecting...");
		toast_until = NowMs() + 5000;
		reconnect_at = NowMs() + 1500;
		if(RegisteredHost *h = settings.FindHost(resume_mac))
			active_host = *h;
	}
	return true;
}

// A crash report from the previous run: say so, and turn off what was running on the GPU
// at the time so the next stream starts safely.
void App::CheckCrashMarker()
{
	std::string marker = platform::JoinPath(data_dir, "crash.txt");
	std::ifstream in(marker);
	if(!in)
		return;
	std::stringstream ss;
	ss << in.rdbuf();
	in.close();
	platform::RemoveFile(marker);
	std::string ctx = ss.str();
	LOGW("Previous run crashed (%s)", ctx.c_str());
	if(ctx.find("exiting") != std::string::npos)
		return; // crashed while closing: nothing was lost, no need to bother anyone
	message = "Orbit Play closed unexpectedly last time. A crash report was saved to " + data_dir + "\\crash.dmp - "
		"please attach it if you report the problem.";
	message_is_error = true;
}

void App::Shutdown()
{
	snprintf(g_crash_context, sizeof(g_crash_context), "exiting");
	FlushPlaytime();
	if(mini_mode)
		ToggleMiniPlayer();
	SaveWindowPlacement();
	if(session)
	{
		session->Stop();
		session.reset();
	}
	gamepads.StopAllFeedback();
	registration.Cancel();
	discovery.Stop();
	settings.Save(settings_path);
	sounds.Shutdown();
	ReleaseBackground();
	renderer.ImGuiShutdown();
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
	std::string icon_path = platform::IconFontPath();   // Segoe Fluent / MDL2 icons on Windows, none on iPad
	const char *icon_font = icon_path.c_str();
	auto exists = [](const char *f) { return platform::FileExists(f); };
	g_icons = !icon_path.empty();
	// fonts are rasterized at the screen's pixel density and drawn at 1/density (sharp on Retina)
	float raster = scale * font_density;
	io.FontGlobalScale = 1.0f / font_density;
	static const ImWchar icon_ranges[] = { ICON_RANGE_MIN, ICON_RANGE_MAX, 0 };
	const ImWchar *text_ranges = io.Fonts->GetGlyphRangesCyrillic(); // Latin + Cyrillic game names

	// 1 light, 2 regular, 3 medium, 4 semibold; fallback: Segoe UI
	auto add = [&](int weight, float px, bool icons) -> ImFont * {
		ImFontConfig cfg;
		cfg.OversampleH = 2;
		cfg.FontDataOwnedByAtlas = false;
		ImFont *f = nullptr;
		const void *data;
		int size;
		if(platform::FontData(weight, &data, &size))
			f = io.Fonts->AddFontFromMemoryTTF((void *)data, size, px * raster, &cfg, text_ranges);
		else
		{
			const char *fallback = weight >= 3 ? "C:\\Windows\\Fonts\\seguisb.ttf" : weight == 1 ? "C:\\Windows\\Fonts\\segoeuil.ttf"
				: "C:\\Windows\\Fonts\\segoeui.ttf";
			if(!exists(fallback))
				fallback = "C:\\Windows\\Fonts\\segoeui.ttf";
			if(exists(fallback))
				f = io.Fonts->AddFontFromFileTTF(fallback, px * raster, &cfg, text_ranges);
			else
			{
				ImFontConfig dc;
				dc.SizePixels = px * 0.8f * raster;
				f = io.Fonts->AddFontDefault(&dc);
			}
		}
		if(icons && g_icons)
		{
			ImFontConfig ic;
			ic.MergeMode = true;
			ic.GlyphOffset = ImVec2(0, px * 0.14f * raster);
			ic.GlyphMinAdvanceX = px * raster;
			io.Fonts->AddFontFromFileTTF(icon_font, px * 0.85f * raster, &ic, icon_ranges);
		}
		return f;
	};
	add(2, 19.0f, true);                // body (first = default)
	g_heading = add(2, 23.0f, true);    // row titles, page descriptions, list entries
	g_title = add(1, 42.0f, false);     // page titles: light weight, large
	g_hero = add(1, 56.0f, false);
	g_display = add(1, 80.0f, false);   // the console name on the home screen
	label_font = add(3, 50.0f, false);  // compare / showcase labels

	ImGuiStyle &st = ImGui::GetStyle();
	st = ImGuiStyle();
	ImGui::StyleColorsDark(&st);
	st.WindowRounding = 8.0f;
	st.ChildRounding = 6.0f;
	st.FrameRounding = 4.0f;
	st.GrabRounding = 4.0f;
	st.PopupRounding = 6.0f;
	st.TabRounding = 4.0f;
	st.ScrollbarRounding = 4.0f;
	st.FramePadding = ImVec2(12, 8);
	st.ItemSpacing = ImVec2(10, 9);
	st.ItemInnerSpacing = ImVec2(8, 6);
	st.WindowPadding = ImVec2(20, 18);
	st.ScrollbarSize = 8.0f;
	st.GrabMinSize = 14.0f;
	st.WindowBorderSize = 1.0f;
	st.ChildBorderSize = 0.0f;
	st.PopupBorderSize = 1.0f;
	st.FrameBorderSize = 0.0f;
	st.TabBarBorderSize = 0.0f;
	st.DisabledAlpha = 0.45f;
	ImVec4 *c = st.Colors;
	const ImVec4 white(1, 1, 1, 1);
	auto w = [](float a) { return ImVec4(1, 1, 1, a); };
	c[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.95f, 1.0f);
	c[ImGuiCol_TextDisabled] = kTextDim;
	c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.082f, 0.105f, 0.97f);
	c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.14f, 0.99f);
	c[ImGuiCol_Border] = w(0.09f);
	c[ImGuiCol_FrameBg] = w(0.06f);
	c[ImGuiCol_FrameBgHovered] = w(0.09f);
	c[ImGuiCol_FrameBgActive] = w(0.12f);
	c[ImGuiCol_Button] = w(0.08f);
	c[ImGuiCol_ButtonHovered] = w(0.13f);
	c[ImGuiCol_ButtonActive] = w(0.18f);
	c[ImGuiCol_CheckMark] = white;
	c[ImGuiCol_SliderGrab] = ImVec4(0.88f, 0.89f, 0.92f, 1.0f);
	c[ImGuiCol_SliderGrabActive] = white;
	c[ImGuiCol_Header] = w(0.08f);
	c[ImGuiCol_HeaderHovered] = w(0.12f);
	c[ImGuiCol_HeaderActive] = w(0.16f);
	c[ImGuiCol_Tab] = w(0.04f);
	c[ImGuiCol_TabHovered] = w(0.12f);
	c[ImGuiCol_TabSelected] = w(0.16f);
	c[ImGuiCol_TabSelectedOverline] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TabDimmed] = w(0.04f);
	c[ImGuiCol_TabDimmedSelected] = w(0.12f);
	c[ImGuiCol_NavCursor] = w(0.80f);
	c[ImGuiCol_Separator] = w(0.08f);
	c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = w(0.12f);
	c[ImGuiCol_ScrollbarGrabHovered] = w(0.20f);
	c[ImGuiCol_ScrollbarGrabActive] = w(0.28f);
	c[ImGuiCol_ResizeGrip] = w(0.05f);
	c[ImGuiCol_ResizeGripHovered] = w(0.25f);
	c[ImGuiCol_ResizeGripActive] = w(0.40f);
	c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
	c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
	c[ImGuiCol_PlotLines] = ImVec4(0.85f, 0.87f, 0.9f, 1.0f);
	st.ScaleAllSizes(scale);
}

void App::SetFullscreen(bool fullscreen)
{
	SDL_SetWindowFullscreen(window, fullscreen);
}

bool App::FrameGenActive() const
{
#ifndef _WIN32
	return false; // not on iPad yet
#endif
	return settings.frame_gen != 0 && !compare_on && !showcase_on;
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
	o.frame_gen = FrameGenActive();
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
	o.compare = (compare_on || showcase_on) && phase == Phase::Streaming;
	o.compare_split = compare_split;
	return o;
}

void App::TakeScreenshot()
{
	if(!renderer.HasVideo())
		return;
	std::string path = platform::JoinPath(platform::CaptureDir(false), platform::TimestampName("OrbitPlay", "png"));
	if(renderer.SaveScreenshot(path, MakePresentOptions(1)))
	{
#ifdef _WIN32
		toast = "Screenshot saved to Pictures\\Orbit Play";
#else
		toast = "Screenshot saved (Files > On My iPad > Orbit Play)";
#endif
		toast_until = NowMs() + 2500;
	}
}

// ---------------------------------------------------------------------------
// Main loop

void App::PullAndSubmit(bool frame_gen)
{
	AVFrame *frame = session->PullFrame();
	if(!frame)
		return;
	double arrival_ms = session->LastFrameTimeUs() / 1000.0;
	SubmitResult r = renderer.SubmitFrame(frame, MakeProcessOptions());
	av_frame_free(&frame);
	if(r.serial && frame_gen)
		pending_frames.push_back({ r.serial, arrival_ms, NowMs() });
	ProcessPendingFrames();
}

// Frames enter the frame-generation timeline once the GPU has said whether they repeat the
// previous frame (30 fps games inside the 60 fps stream). The check is read back without
// waiting, so the GPU is never stalled; it is normally ready within a millisecond or two.
void App::ProcessPendingFrames()
{
	size_t done = 0;
	for(; done < pending_frames.size(); done++)
	{
		PendingFrame &p = pending_frames[done];
		bool dup = false, cut = false;
		int st = renderer.PollFrameDiff(p.serial, &dup, &cut);
		if(st == 0 && NowMs() - p.submitted_ms < 30)
			break; // not ready yet; keep order
		if(st == 0)
			dup = cut = false; // took too long: treat as a normal frame
		if(!renderer.HasFrame(p.serial))
			continue;
		// Never skip more than 2 in a row: in a still scene the picture can still change a little.
		if(dup && dup_streak < 2)
		{
			renderer.DiscardFrame(p.serial);
			dup_streak++;
			dup_count++;
			continue;
		}
		timeline.Add(p.serial, p.arrival_ms, cut, dup);
		if(!dup)
			dup_streak = 0;
	}
	pending_frames.erase(pending_frames.begin(), pending_frames.begin() + done);
}

void App::Run()
{
#ifndef _WIN32
	RunApple();
}
#else
	while(!quit)
	{
		if(renderer.DeviceLost())
		{
			// Driver update / GPU crash: the D3D device is gone. Restart cleanly and reconnect.
			relaunch = true;
			relaunch_mac = (phase == Phase::Streaming || phase == Phase::Connecting) ? active_host.mac : "";
			LOGW("Restarting after a GPU reset%s", relaunch_mac.empty() ? "" : " (will reconnect)");
			quit = true;
			break;
		}
		UpdateMouseCapture();
		PumpEvents();
		Tick();
		HandleSessionEvents();
		SendInput();

		bool streaming = phase == Phase::Streaming && session;
		bool fg = streaming && FrameGenActive();
		if(fg != fg_active_last)
		{
			timeline.Reset();
			pending_frames.clear();
			dup_streak = 0;
			slow_seconds = 0;
			slow_warned = false;
			fg_active_last = fg;
		}

		if(fg)
		{
			// Frame generation: one new picture per display refresh. Wait for either the next
			// refresh slot or a newly decoded frame (store it right away so none are dropped).
			// Wake at least every 2 ms meanwhile so mouse / keyboard input reaches the console
			// without waiting for the next refresh.
			if(HANDLE fw = renderer.FrameWaitHandle())
			{
				HANDLE handles[2] = { fw, session->FrameEvent() };
				DWORD w = WaitForMultipleObjects(2, handles, FALSE, 2);
				if(w == WAIT_OBJECT_0 + 1)
				{
					PullAndSubmit(true);
					continue; // keep waiting for the refresh slot
				}
				if(w == WAIT_TIMEOUT && NowMs() - last_fg_present_ms < 50)
					continue; // input tick
				if(w == WAIT_OBJECT_0)
					renderer.MarkFrameWaited();
			}
			PullAndSubmit(true);
			ProcessPendingFrames();
			PresentFrame(true);
			last_fg_present_ms = NowMs();
			continue;
		}

		bool new_frame = false;
		if(streaming)
		{
			AVFrame *frame = session->PullFrame();
			if(frame)
			{
				new_frame = renderer.SubmitFrame(frame, MakeProcessOptions()).serial != 0;
				av_frame_free(&frame);
			}
		}

		bool ui_needed = !streaming || overlay_open || settings.show_stats || login_pin_open || keyboard_open || !message.empty()
			|| NowMs() < toast_until || showcase_on
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
#endif

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
		// Touches arrive as mouse events too: they drive the interface, never the camera.
		// While playing, tapping the screen opens the menu (iPad has no Esc key on screen).
		bool from_touch = (e.type == SDL_EVENT_MOUSE_MOTION && e.motion.which == SDL_TOUCH_MOUSEID)
			|| ((e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) && e.button.which == SDL_TOUCH_MOUSEID)
			|| (e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.which == SDL_TOUCH_MOUSEID);
		if(from_touch && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && phase == Phase::Streaming && !overlay_open && !keyboard_open
			&& !login_pin_open)
		{
			ShowOverlay(true);
			continue;
		}
		// While the mouse is captured, keyboard and mouse play the game
		if(kbm_event && mouse_captured && !from_touch)
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
			case SDL_EVENT_TERMINATING:
				quit = true;
				break;
			// iPad: leaving the app ends the stream (the system suspends the network anyway)
			case SDL_EVENT_WILL_ENTER_BACKGROUND:
				if(session)
					EndStream("", false);
				FlushPlaytime();
				settings.Save(settings_path);
				in_background = true;
				break;
			case SDL_EVENT_DID_ENTER_FOREGROUND:
				in_background = false;
				break;
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				renderer.Resize(e.window.data1, e.window.data2);
				break;
			case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
			case SDL_EVENT_WINDOW_MOVED:
				last_display_refresh = 0;
				RememberWindowRect();
				break;
			case SDL_EVENT_WINDOW_RESIZED:
				RememberWindowRect();
				break;
			case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
			{
				float s = SDL_GetWindowDisplayScale(window);
				if(s > 0.0f && std::fabs(s - ui_scale) > 0.01f)
				{
					ui_scale = s;
					SetupStyle(ui_scale);
					renderer.ImGuiInvalidate();
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

	// iPad: broadcasts are blocked without a special entitlement, so ask every address of the local
	// network directly, 16 at a time, a full sweep every ~10 s. (Nothing to do on Windows.)
	if(phase != Phase::Streaming && now - last_subnet_probe > 250)
	{
		last_subnet_probe = now;
		if(subnet_pos >= subnet_hosts.size() && now - last_subnet_sweep > 10000)
		{
			subnet_hosts = platform::LocalSubnetHosts();
			subnet_pos = 0;
			last_subnet_sweep = now;
		}
		for(int k = 0; k < 16 && subnet_pos < subnet_hosts.size(); k++)
			discovery.ProbeAddress(subnet_hosts[subnet_pos++]);
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

	if(phase == Phase::Streaming && session)
	{
		// What's running on the console (for play time, profiles, the menu)
		if(now - last_game_check > 3000)
		{
			last_game_check = now;
			discovery.ProbeAddress(active_address);
			DiscoveredHost d;
			if(discovery.Find(active_host.mac, &d) && !d.running_app.empty() && d.running_app != current_game)
				OnGameChanged(d.running_app);
		}
		// save play time every minute, so a crash or power cut loses at most a minute
		if(!current_game.empty() && now - game_since_ms >= 60000)
			FlushPlaytime();
		UpdateWatchdog(now);
		if(!upscale_reported && renderer.HasVideo() && now - stream_start_ms > 4000)
		{
			upscale_reported = true;
			const std::string &st = renderer.UpscalerStatus();
			LOGI("Upscaler: %s", st.c_str());
			bool ai_on = st.rfind("NVIDIA RTX Video Super", 0) == 0 || st.rfind("Intel Video Super", 0) == 0;
			if(settings.upscale_mode == 2 && (!ai_on || st.find("declined") != std::string::npos)
				&& st.rfind("Not needed", 0) != 0)
			{
				ShowToast("AI upscaling isn't running - see Settings > Image > Running now");
				toast_until = now + 6000;
			}
			else if(settings.upscale_mode == 1 && renderer.AiSuperResolutionAvailable() && !enhanced_hint_shown)
			{
				enhanced_hint_shown = true; // once per run: Enhanced may be a deliberate choice
				ShowToast("Upscaling is set to Enhanced. AI super resolution is available: Settings > Image");
				toast_until = now + 6000;
			}
		}
	}
	UpdateBatteryWarnings(now);
	snprintf(g_crash_context, sizeof(g_crash_context), "streaming=%d fg=%d upscale=%d game=%s",
		phase == Phase::Streaming ? 1 : 0, phase == Phase::Streaming && FrameGenActive() ? settings.frame_gen : 0,
		settings.upscale_mode, current_game.c_str());

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

	// Showcase: stay on the enhanced picture for a while, slowly wipe to the original, stay there,
	// then wipe back. (split = share of the picture showing the original, from the left)
	if(showcase_on)
	{
		const double hold_enh = 7000, wipe = 3500, hold_orig = 5000;
		const double cycle = hold_enh + wipe + hold_orig + wipe;
		double t = (double)((now - showcase_start_ms) % (uint64_t)cycle);
		auto ease = [](double x) { x = std::clamp(x, 0.0, 1.0); return x * x * (3.0 - 2.0 * x); };
		double v;
		if(t < hold_enh)
			v = 0.0;
		else if(t < hold_enh + wipe)
			v = ease((t - hold_enh) / wipe);
		else if(t < hold_enh + wipe + hold_orig)
			v = 1.0;
		else
			v = 1.0 - ease((t - hold_enh - wipe - hold_orig) / wipe);
		compare_split = (float)v;
	}

	// Instant replay: ask the PS5 for a keyframe every 5 s so a clip can start close to "now - N s"
	if(phase == Phase::Streaming && session && recorder.ReplayEnabled() && now - last_replay_idr_ms > 5000)
	{
		session->RequestKeyframe();
		last_replay_idr_ms = now;
	}
	{
		std::string m = recorder.TakeMessage();
		if(!m.empty())
		{
			ShowToast(m);
			toast_until = now + 4000;
		}
	}

	if(now - dup_window_start >= 1000)
	{
		dup_rate = dup_count * 1000.0 / (double)std::max<uint64_t>(1, now - dup_window_start);
		dup_count = 0;
		dup_window_start = now;
	}

	if(reconnect_at && now >= reconnect_at && phase == Phase::Idle)
	{
		reconnect_at = 0;
		if(!active_host.mac.empty())
			BeginConnect(active_host.mac);
	}

	if(now - present_window_start >= 1000)
	{
		present_fps = present_count * 1000.0 / (double)(now - present_window_start);
		if(phase == Phase::Streaming && session && renderer.HasVideo())
		{
			StreamStats st = session->Stats();
			stat_fps_sum += st.decode_fps;
			stat_rtt_sum += st.rtt_ms;
			stat_samples++;
			stat_lost = st.frames_lost;
		}
		// Frame generation should run at the display's refresh rate; if it can't, say why.
		if(phase == Phase::Streaming && FrameGenActive() && !overlay_open)
		{
			slow_seconds = present_fps < renderer.DisplayHz() * 0.85 ? slow_seconds + 1 : 0;
			if(slow_seconds >= 3 && !slow_warned)
			{
				char msg[200];
				snprintf(msg, sizeof(msg), "Frame generation reaches only %.0f of %d fps: the GPU is too busy. "
					"Lower the detail level (F7) or upscale target.", present_fps, renderer.DisplayHz());
				ShowToast(msg);
				toast_until = now + 6000;
				slow_warned = true;
			}
		}
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
				stream_start_ms = NowMs();
				upscale_reported = false;
				stat_fps_sum = stat_rtt_sum = 0.0;
				stat_samples = 0;
				stat_lost = 0;
				frozen_warned = false;
				last_keyframe_req = 0;
				current_game.clear();
				last_game_check = 0;
				if(mini_mode)
					ToggleMiniPlayer();
				if(RegisteredHost *h = settings.FindHost(active_host.mac))
				{
					h->last_played = (uint64_t)time(nullptr);
					MarkSettingsDirty();
				}
				overlay_open = false;
				if(settings.kbm_enabled)
					ShowToast("Keyboard & mouse ready - Esc or F1 for the menu");
				{
					Recorder::StreamInfo ri;
					const StreamConfig &c = session->Config();
					ri.hevc = c.codec == VideoCodec::H265 || c.hdr;
					ri.hdr = c.hdr;
					StreamStats st = session->Stats();
					ri.width = st.width > 0 ? st.width : 1920;
					ri.height = st.height > 0 ? st.height : 1080;
					ri.fps = c.fps;
					recorder.SetStreamInfo(ri);
					recorder.SetReplay(settings.replay_enabled, settings.replay_seconds);
					session->SetRecorder(&recorder);
					if(settings.replay_enabled)
						session->RequestKeyframe();
					last_replay_idr_ms = NowMs();
				}
				input_guard = true;
				gamepads.SetMicLed(false);
				if(settings.dualsense_features && gamepads.AnyWiredDualSense())
					audio.OpenHaptics();
				break;
			case SessionEvent::Quit:
			{
				// Unexpected drop (Wi-Fi hiccup, console busy): try to get back in automatically.
				if(e.flag && TryAutoReconnect("Connection lost"))
					return;
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
	renderer.ImGuiNewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	BuildUi();
	ImGui::Render();
	renderer.BeginOverlay();
	renderer.ImGuiRender();

	bool streaming = phase == Phase::Streaming && session;
	bool fg = streaming && FrameGenActive();
	PresentOptions opt = MakePresentOptions((settings.vsync || fg || !streaming) ? 1 : 0);

	auto present = [&](VideoView view, uint64_t serial) {
		renderer.Present(view, opt, serial);
		present_count++;
		uint64_t now_us = chiaki_time_now_monotonic_us();
		if(last_present_us)
		{
			frame_times[frame_time_pos] = (float)((now_us - last_present_us) / 1000.0);
			frame_time_pos = (frame_time_pos + 1) % (int)(sizeof(frame_times) / sizeof(frame_times[0]));
		}
		last_present_us = now_us;
	};

	if(fg)
	{
		bool predictive = settings.frame_gen == 1;
		FrameTimeline::Pick p = timeline.Get(chiaki_time_now_monotonic_us() / 1000.0, predictive);
		// Interpolated on a display that isn't faster than the game: show the real frames
		// (still evenly paced), no in-betweens
		if(p.valid && !predictive && renderer.DisplayHz() < timeline.SourceFps() * 1.25)
			p.t = p.t >= 0.5f ? 1.0f : 0.0f;
		bool generate = p.valid && p.a != p.b && ((p.t > 0.01f && p.t < 0.99f) || p.t > 1.01f);
		if(generate && renderer.BuildInterpolated(p.a, p.b, p.t))
			present(VideoView::Interpolated, p.b);
		else if(p.valid)
			present(VideoView::Latest, p.t >= 0.5f ? p.b : p.a);
		else
			present(VideoView::Latest, 0);
		(void)new_frame;
		return;
	}
	(void)new_frame;
	present(streaming || phase == Phase::Connecting ? VideoView::Latest : VideoView::None, 0);
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
		last_wakeup = 0; // no wake-up step needed
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
	if(!session->Start(cfg, renderer.DecoderDevice(), renderer.DecoderContext(), &audio, &error))
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

bool App::TryAutoReconnect(const std::string &why)
{
	if(phase != Phase::Streaming || !settings.auto_reconnect || reconnect_attempts >= 3)
		return false;
	reconnect_attempts++;
	int attempt = reconnect_attempts;
	EndStream("", false);
	reconnect_attempts = attempt;
	reconnect_at = NowMs() + 2000;
	toast = why + " - reconnecting (" + std::to_string(attempt) + "/3)...";
	toast_until = reconnect_at + 4000;
	LOGW("%s, reconnecting (attempt %d)", why.c_str(), attempt);
	return true;
}

std::string App::SanitizeKey(const std::string &in)
{
	std::string s;
	for(char c : in)
		s += (c == '=' || c == '[' || c == ']' || c == '\n' || c == '\r') ? ' ' : c;
	while(!s.empty() && (s[0] == ';' || s[0] == '#' || s[0] == ' '))
		s.erase(0, 1);
	return s.empty() ? std::string("Unknown game") : s;
}

void App::FlushPlaytime()
{
	if(current_game.empty() || !game_since_ms)
		return;
	uint64_t now = NowMs();
	settings.playtime[SanitizeKey(current_game)] += (now - game_since_ms) / 1000;
	game_since_ms = now - (now - game_since_ms) % 1000;
	MarkSettingsDirty();
}

void App::OnGameChanged(const std::string &game)
{
	FlushPlaytime();
	current_game = game;
	game_since_ms = NowMs();
	LOGI("Now playing: %s", game.c_str());
	if(RegisteredHost *h = settings.FindHost(active_host.mac))
	{
		h->last_app = game;
		MarkSettingsDirty();
	}
	GameProfile *p = settings.game_profiles ? settings.FindProfile(game) : nullptr;
	if(p)
	{
		settings.ApplyProfile(*p);
		MarkSettingsDirty();
		ShowToast("Loaded your settings for " + game);
	}
	else
		ShowToast("Now playing " + game);
	toast_until = NowMs() + 3500;
}

// The picture stopped arriving although the session is still up: ask for a fresh keyframe
// (fixes a stuck decoder after packet loss); after 15 s treat it as a lost connection.
void App::UpdateWatchdog(uint64_t now)
{
	if(!renderer.HasVideo() || !session->LastFrameTimeUs())
		return;
	uint64_t age = chiaki_time_now_monotonic_ms() - session->LastFrameTimeUs() / 1000;
	if(age < 2500)
	{
		frozen_warned = false;
		return;
	}
	if(now - last_keyframe_req > 2000)
	{
		session->RequestKeyframe();
		last_keyframe_req = now;
		LOGW("No picture for %llu ms, requesting a keyframe", (unsigned long long)age);
	}
	if(!frozen_warned)
	{
		ShowToast("Picture paused - asking the console for a fresh frame...");
		frozen_warned = true;
	}
	if(age > 15000 && !TryAutoReconnect("The picture stopped"))
		EndStream("The console stopped sending video. Check the network connection and try again.", true);
}

void App::UpdateBatteryWarnings(uint64_t now)
{
	if(now - last_battery_check < 5000)
		return;
	last_battery_check = now;
	for(auto &p : gamepads.Info())
	{
		bool &warned = battery_warned[p.name];
		if(p.battery < 0 || p.charging || p.battery > 25)
		{
			warned = false;
			continue;
		}
		if(p.battery <= 15 && !warned && settings.low_battery_warning)
		{
			warned = true;
			ShowToast(p.name + ": battery low (" + std::to_string(p.battery) + "%)");
			toast_until = now + 5000;
		}
	}
}

// Small always-on-top window in the corner of the screen (watch while doing something else)
void App::ToggleMiniPlayer()
{
#ifndef _WIN32
	ShowToast("The mini player is only on Windows");
	return;
#endif
	if(!mini_mode)
	{
		mini_was_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
		if(mini_was_fullscreen)
		{
			SDL_SetWindowFullscreen(window, false);
			SDL_SyncWindow(window);
		}
		SDL_GetWindowPosition(window, &mini_prev_x, &mini_prev_y);
		SDL_GetWindowSize(window, &mini_prev_w, &mini_prev_h);
		SDL_DisplayID disp = SDL_GetDisplayForWindow(window);
		SDL_Rect r = { 0, 0, 1920, 1080 };
		SDL_GetDisplayUsableBounds(disp, &r);
		if(SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED)
		{
			SDL_RestoreWindow(window);
			SDL_SyncWindow(window);
			SDL_GetWindowPosition(window, &mini_prev_x, &mini_prev_y);
			SDL_GetWindowSize(window, &mini_prev_w, &mini_prev_h);
			mini_was_maximized = true;
		}
		else
			mini_was_maximized = false;
		int w = std::max(480, r.w / 4), h = (int)std::lround(w / MiniAspect());
		mini_mode = true;
		ShowOverlay(false);
		SDL_SetWindowAlwaysOnTop(window, true);
		SDL_SetWindowSize(window, w, h);
		SDL_SetWindowPosition(window, r.x + r.w - w - 24, r.y + r.h - h - 24);
		ShowToast("Mini player: drag to move, drag an edge to resize, Ctrl+M to go back");
	}
	else
	{
		mini_mode = false;
		SDL_SetWindowAlwaysOnTop(window, false);
		SDL_SetWindowSize(window, mini_prev_w, mini_prev_h);
		SDL_SetWindowPosition(window, mini_prev_x, mini_prev_y);
		if(mini_was_maximized)
			SDL_MaximizeWindow(window);
		if(mini_was_fullscreen)
			SDL_SetWindowFullscreen(window, true);
	}
	sounds.Play(UiSound::Toggle);
}

void App::EndStream(const std::string &msg, bool is_error)
{
	FlushPlaytime();
	bool user_ended = msg.empty() && phase == Phase::Streaming && stream_start_ms && NowMs() - stream_start_ms > 60000;
	std::string summary;
	if(user_ended && stat_samples > 0)
	{
		char b[256];
		snprintf(b, sizeof(b), "Session ended  -  %s  -  %.1f fps  -  %.0f ms network  -  %llu frames lost",
			FormatDuration((NowMs() - stream_start_ms) / 1000).c_str(), stat_fps_sum / stat_samples, stat_rtt_sum / stat_samples,
			(unsigned long long)stat_lost);
		summary = b;
	}
	current_game.clear();
	game_since_ms = 0;
	stream_start_ms = 0;
	if(mini_mode)
		ToggleMiniPlayer();
	home_focus_pending = true;
	if(recorder.Recording())
		recorder.StopRecording();
	if(session)
		session->SetRecorder(nullptr);
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
	if(!summary.empty())
	{
		ShowToast(summary);
		toast_until = NowMs() + 7000;
	}
}

// ---------------------------------------------------------------------------
// UI

void App::NoDragLastItem()
{
	ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax(), o = ImGui::GetMainViewport()->Pos;
	chrome_nodrag.push_back(ImVec4(a.x - o.x, a.y - o.y, b.x - o.x, b.y - o.y));
}

void App::BuildUi()
{
	chrome_nodrag.clear();
	if(phase == Phase::Streaming && renderer.HasVideo())
	{
		if(compare_on || showcase_on)
		{
			// Big labels in the bottom corners: "Original" on the left, the enhanced one on the right.
			// Each fades out when its side is no longer on screen.
			float rect[4];
			renderer.GetVideoRect((AspectMode)settings.aspect_mode, rect);
			ImDrawList *dl = ImGui::GetForegroundDrawList();
			ImFont *font = label_font ? label_font : ImGui::GetFont();
			float size = label_font ? label_font->FontSize : ImGui::GetFontSize() * 2.4f;
			float margin = 36.0f * ui_scale;
			float pad = 16.0f * ui_scale;
			auto smooth = [](float e0, float e1, float x) { float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); };
			auto label = [&](const char *text, bool left, float alpha) {
				if(alpha <= 0.01f)
					return;
				ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
				float x = left ? rect[0] + margin : rect[0] + rect[2] - margin - sz.x;
				float y = rect[1] + rect[3] - margin - sz.y;
				dl->AddRectFilled(ImVec2(x - pad, y - pad * 0.6f), ImVec2(x + sz.x + pad, y + sz.y + pad * 0.6f),
					IM_COL32(0, 0, 0, (int)(140 * alpha)), 12.0f * ui_scale);
				dl->AddText(font, size, ImVec2(x, y), IM_COL32(255, 255, 255, (int)(240 * alpha)), text);
			};
			std::string right = "Enhanced";
			const std::string &st = renderer.UpscalerStatus();
			if(st.rfind("NVIDIA RTX", 0) == 0)
				right = "RTX Enhanced";
			else if(st.rfind("Intel", 0) == 0)
				right = "AI Enhanced";
			label("Original", true, smooth(0.0f, 0.12f, compare_split));
			label(right.c_str(), false, smooth(0.0f, 0.12f, 1.0f - compare_split));
		}
		DrawRecordingBadge();
		if(settings.show_stats && !mini_mode)
			UiStats();
		if(overlay_open)
		{
			UiControlCenterBar();
			UiStreamOverlay();
		}
		UiStreamDialogs();
		UiMessage();
		DrawWindowControls();
		return;
	}

	static Screen last_screen = Screen::Consoles;
	if(screen != last_screen)
	{
		home_focus_pending = screen == Screen::Consoles;
		last_screen = screen;
	}
	UiMain();

	if(phase == Phase::Waking || phase == Phase::Connecting || (phase == Phase::Streaming && !renderer.HasVideo()))
		UiConnecting();
	UiPairDialog();
	UiStreamDialogs();
	UiMessage();
	DrawWindowControls();
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
	changed |= Toggle("Stream in HDR10 when the display is in HDR mode", &settings.hdr);
	ImGui::SetCursorPosX(row_label_w);
	changed |= Toggle("Also on SDR displays (tone-mapped on the PC)", &settings.hdr_force);
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
	Row("Running now");
	if(phase == Phase::Streaming && renderer.HasVideo())
	{
		const std::string &st = renderer.UpscalerStatus();
		bool ai_on = st.rfind("NVIDIA RTX Video Super", 0) == 0 || st.rfind("Intel Video Super", 0) == 0;
		bool problem = settings.upscale_mode == 2 && (!ai_on || st.find("declined") != std::string::npos);
		ImGui::PushStyleColor(ImGuiCol_Text, problem ? kWarn : ImVec4(0.80f, 0.82f, 0.87f, 1.0f));
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted(st.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		if(problem)
			RowHelp(nvidia ? "AI upscaling isn't running. In the NVIDIA App: System > Video > RTX Video Enhancement, turn on Super "
				"Resolution and set quality to 4. For HDR streams you need NVIDIA driver 551 or newer."
				: "AI upscaling isn't running on this GPU, so the Enhanced upscaler is used instead.");
	}
	else
		ImGui::TextDisabled("Shown while you play");

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
	int cmp = showcase_on ? 2 : (compare_on ? 1 : 0);
	const char *cmp_modes[] = { "Off", "Split screen (drag the divider)", "Showcase (divider sweeps by itself)" };
	if(ImGui::Combo("##compare", &cmp, cmp_modes, 3))
	{
		compare_on = cmp == 1;
		showcase_on = cmp == 2;
		if(showcase_on)
			showcase_start_ms = NowMs();
	}
	RowHelp("With the menu open you can drag the divider with the mouse. F7 / F8 step the detail level live.");

	Row("AI HDR for SDR games");
	ImGui::BeginDisabled(!renderer.AiHdrAvailable());
	changed |= Toggle(renderer.AiHdrAvailable() ? "NVIDIA RTX Video HDR##aihdr" : "Needs an NVIDIA RTX GPU##aihdr", &settings.ai_hdr);
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
	const char *fg[] = { "Off", "Predictive (no added latency)", "Interpolated (smoothest)" };
	settings.frame_gen = std::clamp(settings.frame_gen, 0, 2);
	changed |= ImGui::Combo("##fg", &settings.frame_gen, fg, 3);
	char buf[560];
	bool live = phase == Phase::Streaming && session && FrameGenActive();
	if((compare_on || showcase_on) && settings.frame_gen != 0)
		snprintf(buf, sizeof(buf), "Paused while split-screen compare / showcase is on.");
	else if(settings.frame_gen == 0)
		snprintf(buf, sizeof(buf), "Builds a new picture for every refresh of your %d Hz display from the real frames' motion. "
			"Works with 30 fps games too (repeated frames in the stream are detected and skipped).", renderer.DisplayHz());
	else if(settings.frame_gen == 1)
		snprintf(buf, sizeof(buf), "Predicts each refresh by moving the newest frame forward along its motion: smooth like "
			"interpolation but without waiting for the next frame, so it adds no delay (it even shows motion a little sooner). "
			"Edges can stretch briefly when something stops or appears suddenly.%s", live ? "" : "");
	else
		snprintf(buf, sizeof(buf), "Builds the in-between pictures from two real frames: the cleanest motion, but it has to wait "
			"for the next frame, about %.0f ms of extra delay.", live ? timeline.BufferMs(false) : 1000.0 / std::max(30, settings.fps) + 2.0);
	RowHelp(buf);
	if(live)
	{
		char st[200];
		snprintf(st, sizeof(st), "Now: game %.0f fps -> %.0f fps on your %d Hz display%s.", timeline.SourceFps(), present_fps,
			renderer.DisplayHz(), dup_rate > 5.0 ? ", repeated frames skipped (30 fps game)" : "");
		RowHelp(st);
	}
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
	changed |= Toggle("Light the bars with the picture's colors (F5)##amb", &settings.ambient);
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
	SubHeader("Keyboard & mouse");
	Row("Keyboard & mouse");
	changed |= Toggle("Play with keyboard and mouse##kbm", &settings.kbm_enabled);
	RowHelp("While playing, the mouse is captured and moves the right stick. Esc or F1 frees the mouse and opens this menu. "
		"A gamepad keeps working alongside.");
	ImGui::BeginDisabled(!settings.kbm_enabled);
	Row("Mouse feel");
	const char *feels[] = { "Direct (lowest delay)", "PC-like (recommended)", "Smooth (cinematic)", "Raw stick (no curve)", "Custom" };
	int feel = std::clamp(settings.mouse_preset, 0, 4);
	if(ImGui::Combo("##mfeel", &feel, feels, 5))
	{
		settings.ApplyMousePreset(feel);
		changed = true;
	}
	RowHelp("PC-like averages mouse movement over exactly one console input packet (8 ms), so the camera turns by as much as "
		"you moved, and undoes the game's stick curve so slow and fast flicks both feel 1:1.");
	Row("Mouse sensitivity");
	changed |= ImGui::SliderFloat("##msens", &settings.kbm.sensitivity, 0.1f, 5.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
	bool custom = false;
	Row("Aim curve");
	custom |= ImGui::SliderFloat("##mcurve", &settings.kbm.curve, 1.0f, 3.0f, settings.kbm.curve < 1.01f ? "Off (linear stick)" : "%.2f");
	RowHelp("Most console games turn stick deflection into camera speed through a curve. Raise this if small mouse movements "
		"feel sluggish compared to big ones.");
	Row("Anti-deadzone");
	custom |= ImGui::SliderFloat("##madz", &settings.kbm.anti_deadzone, 0.0f, 0.5f, "%.2f");
	RowHelp("Games ignore small stick movements. Raise this if slow mouse movements don't turn the camera, lower it if aiming feels jumpy.");
	Row("Mouse averaging");
	float win_ms = 1.5f + 25.0f * settings.kbm.smoothing * settings.kbm.smoothing;
	char fmt[48];
	snprintf(fmt, sizeof(fmt), "%.1f ms", win_ms);
	custom |= ImGui::SliderFloat("##msmooth", &settings.kbm.smoothing, 0.0f, 1.0f, fmt);
	if(custom)
	{
		settings.mouse_preset = 4;
		changed = true;
	}
	Row("Invert Y");
	changed |= Toggle("##minv", &settings.kbm.invert_y);

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

	SubHeader("Controller");
	Row("DualSense features");
	changed |= Toggle("Haptic feedback and adaptive triggers", &settings.dualsense_features);
	RowHelp("Full haptics need the DualSense on a USB cable; over Bluetooth it falls back to rumble. Adaptive triggers work either way.");
	Row("Haptics intensity");
	changed |= ImGui::SliderFloat("##hg", &settings.haptics_gain, 0.0f, 2.0f, "%.2fx");
	Row("Adaptive triggers");
	changed |= Toggle("Enabled##trig", &settings.adaptive_triggers);
	Row("Motion controls");
	changed |= Toggle("Gyro / accelerometer", &settings.motion_controls);
	Row("Rumble strength");
	changed |= ImGui::SliderFloat("##rg", &settings.rumble_gain, 0.0f, 2.0f, "%.2fx");
	Row("Xbox-style controllers");
	changed |= Toggle("View button acts as touchpad press", &settings.view_is_touchpad);
	RowHelp("Open the stream menu with Create + PS (View + Guide on Xbox pads).");
	if(changed)
		MarkSettingsDirty();
}

void App::UiSystemSection()
{
	bool changed = false;
	Row("Fullscreen");
	bool fs = settings.fullscreen;
	if(Toggle("Borderless fullscreen (F11)", &fs))
	{
		settings.fullscreen = fs;
		SetFullscreen(fs);
		changed = true;
	}
	Row("V-Sync");
	changed |= Toggle("Sync to the display (off = lowest latency, may tear)", &settings.vsync);
	Row("Statistics");
	changed |= Toggle("Show stream statistics while playing (F2)", &settings.show_stats);
	Row("Reconnect");
	changed |= Toggle("Reconnect automatically if the stream drops", &settings.auto_reconnect);
	Row("Volume");
	if(ImGui::SliderFloat("##vol", &settings.volume, 0.0f, 1.5f, "%.2f"))
	{
		audio.SetVolume(settings.volume);
		changed = true;
	}
	Row("Microphone");
	changed |= Toggle("Allow voice chat (DualSense mute button or F10)", &settings.microphone);

	SubHeader("Hotkeys while playing");
	static const char *keys[][2] = {
		{ "Esc / F1", "Stream menu (frees the mouse)" }, { "F2", "Statistics" }, { "F3", "Cycle upscaling" },
		{ "F4", "Cycle frame generation" }, { "F5", "Ambient light on/off" }, { "F6", "Split-screen compare" },
		{ "F7 / F8", "Detail level - / +   (Ctrl: fine sharpness)" }, { "F9", "Start / stop recording" },
		{ "F10", "Save instant replay" }, { "Shift+F9", "Cycle picture mode" }, { "Ctrl+F10", "Microphone on/off" },
		{ "F11", "Fullscreen" }, { "F12", "Screenshot" }, { "Ctrl+M", "Mini player (small always-on-top window)" },
	};
	for(auto &k : keys)
	{
		Row(k[0]);
		ImGui::TextUnformatted(k[1]);
	}
	if(changed)
		MarkSettingsDirty();
}

std::string App::CapturePath(const char *prefix)
{
	return platform::JoinPath(platform::CaptureDir(true), platform::TimestampName(prefix, "mp4"));
}

void App::ToggleRecording()
{
	if(phase != Phase::Streaming || !session)
		return;
	if(recorder.Recording())
	{
		recorder.StopRecording();
		ShowToast("Recording stopped - saving...");
		return;
	}
	if(recorder.StartRecording(CapturePath("OrbitPlay")))
	{
		session->RequestKeyframe(); // recording starts on the next keyframe
		ShowToast("Recording (F9 to stop) - lossless, straight from the PS5 stream");
	}
}

void App::SaveReplay()
{
	if(phase != Phase::Streaming || !session)
		return;
	if(!recorder.ReplayEnabled())
	{
		ShowToast("Instant replay is off - turn it on in the menu (Capture tab)");
		return;
	}
	if(recorder.SaveReplay(CapturePath("Replay")))
		ShowToast("Saving instant replay...");
}

void App::DrawRecordingBadge()
{
	if(!recorder.Recording())
		return;
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	double secs = recorder.RecordingSeconds();
	char txt[48];
	if(secs <= 0.0)
		snprintf(txt, sizeof(txt), "REC  waiting for keyframe");
	else
		snprintf(txt, sizeof(txt), "REC  %02d:%02d", (int)secs / 60, (int)secs % 60);
	ImVec2 sz = ImGui::CalcTextSize(txt);
	float pad = 10.0f * ui_scale, r = 6.0f * ui_scale;
	ImVec2 p(vp->Pos.x + vp->Size.x - sz.x - pad * 3 - r * 2 - 20.0f * ui_scale, vp->Pos.y + 20.0f * ui_scale);
	dl->AddRectFilled(p, ImVec2(p.x + sz.x + pad * 3 + r * 2, p.y + sz.y + pad), IM_COL32(20, 22, 28, 220), 4.0f * ui_scale);
	bool blink = ((int)(ImGui::GetTime() * 2.0)) % 2 == 0;
	dl->AddCircleFilled(ImVec2(p.x + pad + r, p.y + (sz.y + pad) * 0.5f), r, blink ? IM_COL32(235, 60, 60, 255) : IM_COL32(140, 30, 30, 255));
	dl->AddText(ImVec2(p.x + pad * 2 + r * 2, p.y + pad * 0.5f), IM_COL32(255, 255, 255, 235), txt);
}

void App::UiCaptureSection()
{
	bool changed = false;
	bool live = phase == Phase::Streaming && session;
	Row("Record");
	ImGui::BeginDisabled(!live);
	if(PrimaryButton(recorder.Recording() ? "Stop recording (F9)" : "Start recording (F9)"))
		ToggleRecording();
	ImGui::EndDisabled();
	RowHelp("Saves the PS5's own video stream into an MP4 without re-encoding it: identical quality, no performance cost, "
		"HDR kept. Files go to Videos\\Orbit Play.");

	Row("Instant replay");
	if(Toggle("Keep the last moments in memory##replay", &settings.replay_enabled))
	{
		recorder.SetReplay(settings.replay_enabled, settings.replay_seconds);
		if(settings.replay_enabled && session)
			session->RequestKeyframe();
		changed = true;
	}
	Row("Replay length");
	if(ImGui::SliderInt("##replaylen", &settings.replay_seconds, 10, 120, "%d s"))
	{
		recorder.SetReplay(settings.replay_enabled, settings.replay_seconds);
		changed = true;
	}
	ImGui::SetCursorPosX(row_label_w);
	ImGui::BeginDisabled(!live || !settings.replay_enabled);
	if(ImGui::Button("Save replay now (F10)"))
		SaveReplay();
	ImGui::EndDisabled();
	if(live && settings.replay_enabled)
	{
		char b[96];
		snprintf(b, sizeof(b), "Buffered: %.0f s", recorder.ReplayBufferedSeconds());
		RowHelp(b);
	}
	RowHelp("Missed a great moment? Press F10 and the last seconds are saved as a clip. "
		"Uses about 2 MB of memory per second of replay.");
	Row("Folder");
#ifdef _WIN32
	if(ImGui::Button("Open Videos\\Orbit Play"))
		platform::OpenFolder(platform::CaptureDir(true));
#else
	ImGui::TextUnformatted("Files app > On My iPad > Orbit Play");
#endif
	if(changed)
		MarkSettingsDirty();
}

void App::UiLog()
{
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

// Connecting: a full-window view with the console's name and three steps, instead of a dialog.
void App::UiConnecting()
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	float s = ui_scale;
	ImDrawList *bg = ImGui::GetForegroundDrawList();
	ImVec2 a = vp->Pos, b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
	(void)bg;
	ImGui::SetNextWindowPos(a);
	ImGui::SetNextWindowSize(vp->Size);
	ImGui::SetNextWindowBgAlpha(0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::Begin("##connecting", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleVar();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, b, IM_COL32(10, 11, 15, 236));

	float mx = std::clamp(vp->Size.x * 0.06f, 48.0f * s, 128.0f * s);
	float x = a.x + mx, y = a.y + std::max(150.0f * s, vp->Size.y * 0.27f);
	ImFont *df = g_display ? g_display : (g_title ? g_title : ImGui::GetFont());
	ImFont *hf = g_heading ? g_heading : ImGui::GetFont();
	std::string name = active_host.nickname.empty() ? "PlayStation 5" : active_host.nickname;
	dl->AddText(ImVec2(x + 2 * s, y), IM_COL32(150, 156, 168, 255), "CONNECTING");
	y += ImGui::GetFontSize() + 8 * s;
	dl->AddText(df, df->FontSize, ImVec2(x - 3 * s, y), IM_COL32(244, 245, 248, 255), name.c_str());
	y += df->FontSize + 34 * s;

	// steps: done (check), current (spinner), upcoming (dim)
	int cur = phase == Phase::Waking ? 0 : phase == Phase::Connecting ? 1 : 2;
	const char *steps[] = { "Waking the console", "Connecting and signing in", "Starting the picture" };
	const char *hints[] = { "From rest mode this can take up to a minute.", "", "" };
	float t = (float)ImGui::GetTime();
	for(int i = 0; i < 3; i++)
	{
		ImVec2 c(x + 10 * s, y + hf->FontSize * 0.5f);
		bool skipped = i == 0 && cur > 0 && last_wakeup == 0;
		if(i < cur)
		{
			ImU32 col = skipped ? IM_COL32(110, 116, 128, 255) : IM_COL32(236, 238, 242, 255);
			dl->AddLine(ImVec2(c.x - 6 * s, c.y), ImVec2(c.x - 2 * s, c.y + 4 * s), col, 2.0f * s);
			dl->AddLine(ImVec2(c.x - 2 * s, c.y + 4 * s), ImVec2(c.x + 7 * s, c.y - 5 * s), col, 2.0f * s);
		}
		else if(i == cur)
		{
			dl->PathArcTo(c, 8 * s, 0, 6.2831853f, 32);
			dl->PathStroke(IM_COL32(255, 255, 255, 30), 0, 2.0f * s);
			dl->PathArcTo(c, 8 * s, t * 5.0f, t * 5.0f + 1.8f + 0.7f * sinf(t * 2.0f), 24);
			dl->PathStroke(IM_COL32(236, 238, 242, 255), 0, 2.0f * s);
		}
		else
			dl->AddCircle(c, 3.0f * s, IM_COL32(110, 116, 128, 255), 12, 1.5f * s);
		ImU32 tc = i == cur ? IM_COL32(242, 244, 248, 255) : i < cur ? IM_COL32(170, 176, 188, 255) : IM_COL32(110, 116, 128, 255);
		dl->AddText(hf, hf->FontSize, ImVec2(x + 34 * s, y), tc, steps[i]);
		y += hf->FontSize + 6 * s;
		if(i == cur && *hints[i])
		{
			dl->AddText(ImVec2(x + 34 * s, y), IM_COL32(140, 146, 158, 255), hints[i]);
			y += ImGui::GetFontSize() + 6 * s;
		}
		y += 12 * s;
	}
	y += 18 * s;
	ImGui::SetCursorScreenPos(ImVec2(x, y));
	if(hf)
		ImGui::PushFont(hf);
	if(ImGui::Button("Cancel", ImVec2(180 * s, 50 * s)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)
		|| ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
	{
		if(phase == Phase::Waking)
			phase = Phase::Idle;
		else
			EndStream("", false);
		sounds.Play(UiSound::Back);
	}
	if(hf)
		ImGui::PopFont();
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
	ImGui::Text("Decode   %.1f fps    Present %.1f fps", s.decode_fps, present_fps);
	if(FrameGenActive())
		ImGui::Text("FrameGen %s  game %.0f fps -> %.0f fps   buffer %.0f ms   repeats skipped %.0f/s",
			settings.frame_gen == 1 ? "predictive" : "interpolated", timeline.SourceFps(), present_fps,
			timeline.BufferMs(settings.frame_gen == 1), dup_rate);
	{
		// Rough end-to-end picture of where the delay comes from (the PS5's own capture/encode
		// time isn't visible to the PC and comes on top).
		double net = s.rtt_ms * 0.5;
		double fgb = FrameGenActive() ? (settings.frame_gen == 1 ? timeline.BufferMs(true) - 1000.0 / std::max(1.0, present_fps) * 0.5
			: timeline.BufferMs(false)) : 0.0;
		double disp = 1000.0 / std::max(1, renderer.DisplayHz()) * ((settings.vsync || FrameGenActive()) ? 1.5 : 0.5);
		double total = net + 2.0 + std::max(0.0, fgb) + disp + 4.0;
		ImGui::Text("Latency  ~%.0f ms on the PC side: network %.0f + decode ~2 + frame gen %.0f + display %.0f + input ~4",
			total, net, std::max(0.0, fgb), disp);
	}
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
	ImVec2 size(std::min(1100.0f * ui_scale, vp->Size.x - 40.0f), std::min(vp->Size.y * 0.84f, 940.0f * ui_scale));
	// Opens centered; afterwards it stays where you drag it (grab the header or any empty area)
	if(menu_reset_pos)
	{
		ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(size, ImGuiCond_Always);
		menu_reset_pos = false;
	}
	ImGui::SetNextWindowSizeConstraints(ImVec2(720 * ui_scale, 460 * ui_scale), ImVec2(vp->Size.x, vp->Size.y));
	ImGui::SetNextWindowBgAlpha(0.96f);
	// Focus only when the menu opens: focusing every frame would close dropdowns as soon as they open.
	if(overlay_just_opened)
	{
		ImGui::SetNextWindowFocus();
		overlay_just_opened = false;
	}
	ImGui::Begin("##streammenu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoScrollbar);
	{
		ImVec2 a = ImGui::GetWindowPos(), b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight()), o = vp->Pos;
		chrome_nodrag.push_back(ImVec4(a.x - o.x, a.y - o.y, b.x - o.x, b.y - o.y));
	}

	// B on the pad closes the menu, unless it is closing a dropdown
	bool popup_open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
	if(!popup_open && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && kbm.CapturingAction() < 0)
		ShowOverlay(false);

	// ---- header: grip, console name, live info, center + close buttons
	{
		ImDrawList *dl = ImGui::GetWindowDrawList();
		ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
		dl->AddRectFilled(ImVec2(wp.x + ws.x * 0.5f - 28 * ui_scale, wp.y + 8 * ui_scale),
			ImVec2(wp.x + ws.x * 0.5f + 28 * ui_scale, wp.y + 12 * ui_scale), IM_COL32(255, 255, 255, 50), 2.0f);
		ImGui::Dummy(ImVec2(0, 2 * ui_scale));
		if(g_heading)
			ImGui::PushFont(g_heading);
		ImGui::TextUnformatted(!current_game.empty() ? current_game.c_str() : active_host.nickname.empty() ? "PlayStation 5" : active_host.nickname.c_str());
		if(g_heading)
			ImGui::PopFont();
		ImGui::SameLine();
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4 * ui_scale);
		ImGui::TextDisabled("  %dx%d -> %dx%d   %s   %.0f fps%s", renderer.VideoWidth(), renderer.VideoHeight(),
			renderer.OutputVideoWidth(), renderer.OutputVideoHeight(), renderer.VideoHdr() ? "HDR" : "SDR", present_fps,
			FrameGenActive() ? (settings.frame_gen == 1 ? "  predictive" : "  interpolated") : "");
		float bs = ImGui::GetFrameHeight();
		ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - bs * 2 - 8 * ui_scale);
		ImGui::SetCursorPosY(ImGui::GetStyle().WindowPadding.y);
		if(ImGui::Button(g_icons ? ICON_HOME "##center" : "C##center", ImVec2(bs, bs)))
			menu_reset_pos = true;
		if(ImGui::IsItemHovered())
			ImGui::SetTooltip("Move the menu back to the center");
		ImGui::SameLine(0, 8 * ui_scale);
		if(ImGui::Button(g_icons ? ICON_CLOSE "##close" : "X##close", ImVec2(bs, bs)))
			ShowOverlay(false);
		if(ImGui::IsItemHovered())
			ImGui::SetTooltip("Back to the game (Esc)");
		if(ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::GetMousePos().y < wp.y + 60 * ui_scale)
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
	}
	ImGui::Dummy(ImVec2(0, 4 * ui_scale));

	// ---- quick actions
	{
		struct QA { const char *icon; std::string label; bool on; int id; };
		bool muted = !session || session->MicMuted();
		std::vector<QA> qa = {
			{ ICON_PLAY, "Resume", false, 0 },
			{ ICON_MIC, muted ? "Mic off" : "Mic on", !muted, 1 },
			{ ICON_CAMERA, "Screenshot", false, 2 },
			{ ICON_RECORD, recorder.Recording() ? "Stop rec" : "Record", recorder.Recording(), 3 },
			{ ICON_VIDEO, "Save replay", false, 4 },
			{ ICON_SPLIT, showcase_on ? "Showcase" : "Compare", compare_on || showcase_on, 5 },
			{ ICON_DISPLAY, "Mini player", false, 6 },
		};
		float n = (float)qa.size();
		float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * (n - 1)) / n;
		float bh = 54.0f * ui_scale;
		for(size_t i = 0; i < qa.size(); i++)
		{
			if(i)
				ImGui::SameLine();
			ImGui::PushID(qa[i].id);
			bool dis = (qa[i].id == 1 && !(session && settings.microphone)) || (qa[i].id == 4 && !recorder.ReplayEnabled());
			ImGui::BeginDisabled(dis);
			ImVec2 p = ImGui::GetCursorScreenPos();
			bool on = qa[i].on, rec = qa[i].id == 3 && on;
			if(on)
			{
				ImGui::PushStyleColor(ImGuiCol_Button, rec ? ImVec4(0.70f, 0.16f, 0.18f, 1) : ImVec4(0.90f, 0.91f, 0.93f, 1));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rec ? ImVec4(0.80f, 0.22f, 0.24f, 1) : ImVec4(1, 1, 1, 1));
			}
			bool clicked = ImGui::Button("##qa", ImVec2(bw, bh));
			if(on)
				ImGui::PopStyleColor(2);
			ImDrawList *dl = ImGui::GetWindowDrawList();
			const char *ic = Ico(qa[i].icon);
			ImVec2 isz = ImGui::CalcTextSize(ic), lsz = ImGui::CalcTextSize(qa[i].label.c_str());
			// icon and label side by side
			float gapx = *ic ? 8 * ui_scale : 0;
			float tw = isz.x + gapx + lsz.x;
			float tx = p.x + (bw - tw) * 0.5f, ty = p.y + (bh - lsz.y) * 0.5f;
			ImU32 tc = on && !rec ? IM_COL32(14, 16, 22, 255) : ImGui::GetColorU32(ImGuiCol_Text, dis ? 0.4f : 1.0f);
			dl->PushClipRect(p, ImVec2(p.x + bw, p.y + bh), true);
			if(*ic)
				dl->AddText(ImVec2(tx, ty), tc, ic);
			dl->AddText(ImVec2(tx + isz.x + gapx, ty), tc, qa[i].label.c_str());
			dl->PopClipRect();
			ImGui::EndDisabled();
			ImGui::PopID();
			if(!clicked)
				continue;
			switch(qa[i].id)
			{
				case 0: ShowOverlay(false); break;
				case 1:
					session->SetMicMuted(!muted);
					gamepads.SetMicLed(muted);
					break;
				case 2: TakeScreenshot(); break;
				case 3: ToggleRecording(); break;
				case 4: SaveReplay(); break;
				case 6: ToggleMiniPlayer(); break;
				case 5:
					if(showcase_on)
						showcase_on = false;
					else if(compare_on)
					{
						compare_on = false;
						showcase_on = true;
						showcase_start_ms = NowMs();
					}
					else
						compare_on = true;
					break;
			}
		}
	}
	if(!overlay_open || !session)
	{
		ImGui::End();
		return;
	}

	if(StreamSettingsChanged())
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
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
	ImGui::Dummy(ImVec2(0, 6 * ui_scale));
	{
		// left: sections, right: the section's rows (same layout as Settings)
		const int cats[] = { Cat_Game, Cat_Image, Cat_Motion, Cat_Picture, Cat_Controls, Cat_Capture, Cat_Stream, Cat_System, Cat_Interface };
		float body_h = ImGui::GetContentRegionAvail().y - footer;
		float lw = std::clamp(ImGui::GetContentRegionAvail().x * 0.22f, 170.0f * ui_scale, 240.0f * ui_scale);
		ImGui::BeginChild("##menucats", ImVec2(lw, body_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
		for(int c : cats)
			if(ListItem(CategoryName(c), menu_cat == c))
				menu_cat = c;
		ImGui::EndChild();
		ImGui::SameLine(0, 18 * ui_scale);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::BeginChild("##menupage", ImVec2(0, body_h), ImGuiChildFlags_None, ImGuiWindowFlags_None);
		ImGui::PopStyleVar();
		DrawCategory(menu_cat, true);
		ImGui::EndChild();
	}

	ImGui::Separator();
	float fw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
	char hb[48], rb[64], db[48];
	snprintf(hb, sizeof(hb), "%s%sPS Home screen", Ico(ICON_HOME), g_icons ? "  " : "");
	snprintf(rb, sizeof(rb), "%s%sPut console in rest mode", Ico(ICON_POWER), g_icons ? "  " : "");
	snprintf(db, sizeof(db), "%s%sDisconnect", Ico(ICON_CANCEL), g_icons ? "  " : "");
	if(ImGui::Button(hb, ImVec2(fw, 0)) && session)
	{
		session->GoHome();
		ShowOverlay(false);
	}
	ImGui::SameLine();
	if(ImGui::Button(rb, ImVec2(fw, 0)) && session)
	{
		session->GotoBed();
		EndStream("", false);
	}
	ImGui::SameLine();
	if(ImGui::Button(db, ImVec2(fw, 0)))
		EndStream("", false);
	ImGui::End();

	// Drag the compare divider with the mouse (outside the menu)
	if(compare_on && !showcase_on && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::GetIO().WantCaptureMouse)
	{
		float rect[4];
		renderer.GetVideoRect((AspectMode)settings.aspect_mode, rect);
		if(rect[2] > 0)
			compare_split = std::clamp((ImGui::GetIO().MousePos.x - rect[0]) / rect[2], 0.0f, 1.0f);
	}
}

void App::ShowOverlay(bool open)
{
	if(open == overlay_open)
		return;
	if(open && mini_mode)
		ToggleMiniPlayer();
	overlay_open = open;
	sounds.Play(open ? UiSound::Toggle : UiSound::Back);
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
			settings.frame_gen = (settings.frame_gen + 1) % 3;
			if(settings.frame_gen == 0)
				ShowToast("Frame generation: off");
			else
			{
				snprintf(buf, sizeof(buf), "Frame generation: %s (up to %d fps)", settings.frame_gen == 1 ? "predictive, no added delay"
					: "interpolated, smoothest", renderer.DisplayHz());
				ShowToast(buf);
			}
			break;
		}
		case SDLK_F5:
			settings.ambient = !settings.ambient;
			ShowToast(settings.ambient ? "Ambient light on" : "Ambient light off");
			break;
		case SDLK_F6:
			if(!compare_on && !showcase_on)
			{
				compare_on = true;
				ShowToast("Compare: original left | enhanced right (F6 again: showcase)");
			}
			else if(compare_on)
			{
				compare_on = false;
				showcase_on = true;
				showcase_start_ms = NowMs();
				ShowToast("Showcase: the divider sweeps on its own (F6 again: off)");
			}
			else
			{
				showcase_on = false;
				ShowToast("Compare off");
			}
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
			if(!(SDL_GetModState() & SDL_KMOD_SHIFT))
			{
				ToggleRecording();
				return true;
			}
			// Shift+F9: cycle picture mode
			settings.ApplyPicturePreset((std::min(settings.picture_preset, 2) + 1) % 3);
			const char *names[] = { "Natural", "Vivid", "Cinema" };
			ShowToast(std::string("Picture mode: ") + names[settings.picture_preset]);
			break;
		}
		case SDLK_F10:
			if(!(SDL_GetModState() & SDL_KMOD_CTRL))
			{
				SaveReplay();
				return true;
			}
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
		case SDLK_M:
			if(!(SDL_GetModState() & SDL_KMOD_CTRL))
				return false;
			ToggleMiniPlayer();
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
	return phase == Phase::Streaming && session && settings.kbm_enabled && window_focused && !mini_mode
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
		// a plain notification card in the top-right corner; fades out
		uint64_t left = toast_until - NowMs();
		float alpha = std::min(1.0f, left / 300.0f);
		ImGuiViewport *tv = ImGui::GetMainViewport();
		ImDrawList *dl = ImGui::GetForegroundDrawList();
		float maxw = std::min(560.0f * ui_scale, tv->Size.x - 48 * ui_scale);
		float padx = 20 * ui_scale, pady = 14 * ui_scale;
		ImVec2 ts = ImGui::CalcTextSize(toast.c_str(), nullptr, false, maxw - padx * 2);
		float w = std::min(maxw, ts.x + padx * 2);
		ImVec2 a(tv->Pos.x + tv->Size.x - w - 24 * ui_scale, tv->Pos.y + 24 * ui_scale), b(a.x + w, a.y + ts.y + pady * 2);
		dl->AddRectFilled(a, b, IM_COL32(28, 30, 37, (int)(245 * alpha)), 6.0f * ui_scale);
		dl->AddRect(a, b, IM_COL32(255, 255, 255, (int)(26 * alpha)), 6.0f * ui_scale, 0, 1.0f);
		dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + padx, a.y + pady), IM_COL32(236, 238, 242, (int)(255 * alpha)),
			toast.c_str(), nullptr, maxw - padx * 2);
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
