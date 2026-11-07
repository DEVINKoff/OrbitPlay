// SPDX-License-Identifier: AGPL-3.0-only
// The app draws its own title bar. The window is an SDL "borderless" window (its size math has no
// frame) that still has the caption/resize styles, so snapping, the drop shadow, rounded corners
// and resizing from the edges keep working; this procedure removes the frame and does hit-testing. Minimize / maximize / close are drawn in the app's
// top bar; the empty parts of that bar drag the window.
#include "app.h"
#include "ui_style.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

static LRESULT CALLBACK ChromeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
	App *app = (App *)ref;
	switch(msg)
	{
		// WM_NCCALCSIZE: SDL removes the frame of borderless windows (and fits maximized ones to the work area)
		case WM_NCHITTEST:
		{
			POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
			ScreenToClient(h, &pt);
			return app->ChromeHitTest(pt.x, pt.y);
		}
		case WM_NCLBUTTONDBLCLK:
			if(app->MiniMode())
				return 0; // double-clicking the mini player must not maximize it
			break;
		case WM_SIZING:
			if(app->MiniMode())
			{
				// mini player keeps the picture's aspect ratio while resizing
				RECT *r = (RECT *)lp;
				float ar = app->MiniAspect();
				int w = std::max(320, (int)(r->right - r->left)), hh = std::max(180, (int)(r->bottom - r->top));
				switch(wp)
				{
					case WMSZ_TOP:
					case WMSZ_BOTTOM:
						w = (int)std::lround(hh * ar);
						r->right = r->left + w;
						break;
					default:
						hh = (int)std::lround(w / ar);
						if(wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT)
							r->top = r->bottom - hh;
						else
							r->bottom = r->top + hh;
						if(wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT)
							r->left = r->right - w;
						else
							r->right = r->left + w;
						break;
				}
				return TRUE;
			}
			break;
		case WM_NCDESTROY:
			RemoveWindowSubclass(h, ChromeProc, id);
			break;
	}
	return DefSubclassProc(h, msg, wp, lp);
}

void App::InstallChrome()
{
	SetWindowSubclass(hwnd, ChromeProc, 1, (DWORD_PTR)this);
	DWORD corner = 2; // DWMWCP_ROUND (Windows 11)
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
	SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

float App::MiniAspect() const
{
	int w = renderer.VideoWidth(), h = renderer.VideoHeight();
	return (w > 0 && h > 0) ? (float)w / (float)h : 16.0f / 9.0f;
}

bool App::WindowFullscreen() const
{
	return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
}

LRESULT App::ChromeHitTest(int x, int y)
{
	if(WindowFullscreen())
		return HTCLIENT;
	RECT rc;
	GetClientRect(hwnd, &rc);
	int w = rc.right, h = rc.bottom;
	if(!IsZoomed(hwnd))
	{
		int e = std::max(5, (int)(7 * ui_scale));
		bool l = x < e, r = x >= w - e, t = y < e, b = y >= h - e;
		if(t && l) return HTTOPLEFT;
		if(t && r) return HTTOPRIGHT;
		if(b && l) return HTBOTTOMLEFT;
		if(b && r) return HTBOTTOMRIGHT;
		if(t) return HTTOP;
		if(b) return HTBOTTOM;
		if(l) return HTLEFT;
		if(r) return HTRIGHT;
	}
	for(const ImVec4 &r : chrome_nodrag)
		if(x >= r.x && x < r.z && y >= r.y && y < r.w)
			return HTCLIENT;
	if(mini_mode)
		return HTCAPTION; // drag the mini player from anywhere
	if(y < caption_h)
		return HTCAPTION;
	return HTCLIENT;
}

// Is the mouse inside the window, in its top band? (Works over the drag area too, where the app
// gets no mouse-move events.)
static bool CursorInBand(HWND hwnd, float band, bool anywhere)
{
	POINT p;
	if(!GetCursorPos(&p) || WindowFromPoint(p) != hwnd)
		return false;
	ScreenToClient(hwnd, &p);
	RECT rc;
	GetClientRect(hwnd, &rc);
	return p.x >= 0 && p.x < rc.right && p.y >= 0 && (anywhere ? p.y < rc.bottom : p.y < band);
}

// Minimize / maximize (restore) / close, flush with the top-right corner.
void App::DrawWindowControls()
{
	bool streaming = phase == Phase::Streaming && renderer.HasVideo();
	float s = ui_scale;
	float bw = 46.0f * s, bh = 34.0f * s;
	ImGuiViewport *vp = ImGui::GetMainViewport();

	// while playing they appear only when the mouse is free and near the top (or the menu is open);
	// in the mini player, when the mouse is over it
	bool want = true;
	if(mini_mode)
		want = CursorInBand(hwnd, 0, true);
	else if(streaming)
		want = !mouse_captured && (overlay_open || CursorInBand(hwnd, 64.0f * s, false));
	float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
	chrome_alpha += ((want ? 1.0f : 0.0f) - chrome_alpha) * std::min(1.0f, dt * 12.0f);
	caption_h = mini_mode ? 0.0f : streaming ? (mouse_captured ? 0.0f : 40.0f * s) : 96.0f * s;
	if(chrome_alpha < 0.02f)
		return;

	int n = mini_mode ? 2 : 3;
	ImVec2 size(bw * n, bh);
	ImVec2 pos(vp->Pos.x + vp->Size.x - size.x, vp->Pos.y);
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
	ImGui::Begin("##windowcontrols", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
	ImGui::PopStyleVar(3);
	ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow()); // above full-window views like "Connecting"
	ImDrawList *dl = ImGui::GetWindowDrawList();
	bool fullscreen = WindowFullscreen(), maximized = IsZoomed(hwnd) != 0;
	auto glyph_col = [&](bool hot) { return IM_COL32(236, 238, 242, (int)(255 * chrome_alpha * (hot ? 1.0f : 0.85f))); };
	float lw = std::max(1.0f, 1.0f * s);
	for(int i = 0; i < n; i++)
	{
		int kind = mini_mode ? (i == 0 ? 3 : 2) : i; // 0 minimize, 1 maximize/restore, 2 close, 3 leave mini player
		ImVec2 p(pos.x + bw * i, pos.y), q(p.x + bw, p.y + bh);
		ImGui::SetCursorScreenPos(p);
		ImGui::PushID(i);
		bool clicked = ImGui::InvisibleButton("##wc", ImVec2(bw, bh));
		bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
		ImGui::PopID();
		chrome_nodrag.push_back(ImVec4(p.x - vp->Pos.x, p.y - vp->Pos.y, q.x - vp->Pos.x, q.y - vp->Pos.y));
		if(hov)
			dl->AddRectFilled(p, q, kind == 2 ? IM_COL32(196, 43, 28, (int)(255 * chrome_alpha))
				: IM_COL32(255, 255, 255, (int)((held ? 26 : 16) * chrome_alpha)));
		ImVec2 c(p.x + bw * 0.5f, p.y + bh * 0.5f);
		float g = 5.0f * s;
		ImU32 col = glyph_col(hov);
		switch(kind)
		{
			case 0:
				dl->AddLine(ImVec2(c.x - g, c.y), ImVec2(c.x + g, c.y), col, lw);
				break;
			case 1:
				if(maximized || fullscreen)
				{
					dl->AddRect(ImVec2(c.x - g, c.y - g + 2 * s), ImVec2(c.x + g - 2 * s, c.y + g), col, 1.0f * s, 0, lw);
					dl->AddLine(ImVec2(c.x - g + 2 * s, c.y - g), ImVec2(c.x + g, c.y - g), col, lw);
					dl->AddLine(ImVec2(c.x + g, c.y - g), ImVec2(c.x + g, c.y + g - 2 * s), col, lw);
				}
				else
					dl->AddRect(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), col, 1.0f * s, 0, lw);
				break;
			case 2:
				dl->AddLine(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), col, lw);
				dl->AddLine(ImVec2(c.x - g, c.y + g), ImVec2(c.x + g, c.y - g), col, lw);
				break;
			case 3: // back to the full window: a box with an arrow out of it
				dl->AddRect(ImVec2(c.x - g, c.y - g), ImVec2(c.x + g, c.y + g), col, 1.0f * s, 0, lw);
				dl->AddLine(ImVec2(c.x - 1 * s, c.y + 1 * s), ImVec2(c.x + g + 2 * s, c.y - g - 2 * s), col, lw);
				break;
		}
		if(hov)
			ImGui::SetTooltip("%s", kind == 0 ? "Minimize" : kind == 1 ? (fullscreen ? "Exit full screen" : maximized ? "Restore" : "Maximize")
				: kind == 2 ? "Close" : "Back to the full window (Ctrl+M)");
		if(!clicked)
			continue;
		switch(kind)
		{
			case 0: SDL_MinimizeWindow(window); break;
			case 1:
				if(fullscreen)
				{
					settings.fullscreen = false;
					SetFullscreen(false);
					MarkSettingsDirty();
				}
				else if(maximized)
					SDL_RestoreWindow(window);
				else
					SDL_MaximizeWindow(window);
				break;
			case 2: quit = true; break;
			case 3: ToggleMiniPlayer(); break;
		}
	}
	ImGui::End();
}

// Window size, position and maximized state are restored on the next start.
void App::SaveWindowPlacement()
{
	if(!hwnd || mini_mode)
		return;
	WINDOWPLACEMENT wp = { sizeof(wp) };
	if(!GetWindowPlacement(hwnd, &wp))
		return;
	RECT r = wp.rcNormalPosition;
	settings.win_x = r.left;
	settings.win_y = r.top;
	settings.win_w = r.right - r.left;
	settings.win_h = r.bottom - r.top;
	settings.win_max = wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED);
}

void App::RestoreWindowPlacement()
{
	RECT r = { settings.win_x, settings.win_y, settings.win_x + settings.win_w, settings.win_y + settings.win_h };
	bool valid = settings.win_w >= 640 && settings.win_h >= 360 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
	if(!valid)
	{
		// first start: 70% of the work area, centered
		HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
		MONITORINFO mi = { sizeof(mi) };
		GetMonitorInfoW(mon, &mi);
		int ww = (mi.rcWork.right - mi.rcWork.left) * 7 / 10, wh = ww * 9 / 16;
		r.left = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
		r.top = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
		r.right = r.left + ww;
		r.bottom = r.top + wh;
	}
	// set the normal position while still hidden, then show through SDL so it knows the state
	WINDOWPLACEMENT wp = { sizeof(wp) };
	wp.rcNormalPosition = r;
	wp.showCmd = SW_HIDE;
	SetWindowPlacement(hwnd, &wp);
	SDL_ShowWindow(window);
	if(valid && settings.win_max)
		SDL_MaximizeWindow(window);
}
