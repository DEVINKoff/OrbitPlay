// SPDX-License-Identifier: AGPL-3.0-only
// PS5-style home screen: animated background, tile row, game hub, top bar, boot intro and the
// control-center bar shown while the stream menu is open.
#include "app.h"
#include "log.h"
#include "ui_style.h"

#include <chiaki/time.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

static uint64_t NowMs() { return chiaki_time_now_monotonic_ms(); }
static float Smooth01(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
static ImU32 Col(const ImVec4 &c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

// Each console gets its own color (stable, from its MAC), like game tiles on the PS5.
ImVec4 App::HostColor(const std::string &id)
{
	uint32_t h = 2166136261u;
	for(char c : id)
		h = (h ^ (uint8_t)c) * 16777619u;
	float hue = 0.52f + (h % 1000) / 1000.0f * 0.36f; // blues, indigos, purples, teals
	float r, g, b;
	ImGui::ColorConvertHSVtoRGB(hue - std::floor(hue), 0.78f, 0.85f, r, g, b);
	return ImVec4(r, g, b, 1.0f);
}

std::string App::FormatDuration(uint64_t seconds)
{
	char b[48];
	if(seconds < 60)
		snprintf(b, sizeof(b), "%llu s", (unsigned long long)seconds);
	else if(seconds < 3600)
		snprintf(b, sizeof(b), "%llu min", (unsigned long long)(seconds / 60));
	else
		snprintf(b, sizeof(b), "%llu h %llu min", (unsigned long long)(seconds / 3600), (unsigned long long)(seconds / 60 % 60));
	return b;
}

static std::string ClockText()
{
	wchar_t w[32] = {};
	if(!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, w, 32))
		return "";
	char u[64] = {};
	WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), nullptr, nullptr);
	return u;
}

// PlayStation face-button glyph: 0 cross, 1 circle, 2 square, 3 triangle
static void DrawPsGlyph(ImDrawList *dl, ImVec2 c, float r, int kind)
{
	dl->AddCircleFilled(c, r, IM_COL32(20, 24, 34, 230), 32);
	dl->AddCircle(c, r, IM_COL32(255, 255, 255, 170), 32, 1.4f);
	float s = r * 0.42f, t = std::max(1.5f, r * 0.13f);
	switch(kind)
	{
		case 0:
			dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), IM_COL32(124, 178, 232, 255), t);
			dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), IM_COL32(124, 178, 232, 255), t);
			break;
		case 1:
			dl->AddCircle(c, s * 1.05f, IM_COL32(255, 102, 102, 255), 24, t);
			break;
		case 2:
			dl->AddRect(ImVec2(c.x - s * 0.9f, c.y - s * 0.9f), ImVec2(c.x + s * 0.9f, c.y + s * 0.9f), IM_COL32(255, 105, 248, 255), 0, 0, t);
			break;
		default:
			dl->AddTriangle(ImVec2(c.x, c.y - s * 1.05f), ImVec2(c.x + s * 1.05f, c.y + s * 0.75f), ImVec2(c.x - s * 1.05f, c.y + s * 0.75f),
				IM_COL32(64, 226, 160, 255), t);
			break;
	}
}

static void DrawShoulder(ImDrawList *dl, ImVec2 p, float h, const char *text)
{
	ImVec2 ts = ImGui::CalcTextSize(text);
	float w = ts.x + h * 0.8f;
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(20, 24, 34, 230), h * 0.3f);
	dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 170), h * 0.3f, 0, 1.3f);
	dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), IM_COL32(255, 255, 255, 230), text);
}

// Controller battery: outline with a fill level, blue while charging
static float DrawBattery(ImDrawList *dl, ImVec2 p, float h, int pct, bool charging)
{
	float w = h * 1.9f, nub = h * 0.18f;
	dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 200), h * 0.22f, 0, 1.4f);
	dl->AddRectFilled(ImVec2(p.x + w + 1, p.y + h * 0.3f), ImVec2(p.x + w + 1 + nub, p.y + h * 0.7f), IM_COL32(255, 255, 255, 200), 1.0f);
	if(pct >= 0)
	{
		ImU32 c = charging ? IM_COL32(90, 170, 255, 255) : pct > 30 ? IM_COL32(110, 225, 140, 255) : pct > 15 ? IM_COL32(255, 190, 80, 255)
			: IM_COL32(255, 85, 85, 255);
		float inner = (w - 4) * std::clamp(pct, 4, 100) / 100.0f;
		dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + 2 + inner, p.y + h - 2), c, h * 0.15f);
	}
	return w + nub + 1;
}

// ---------------------------------------------------------------------------
// Background: a deep gradient tinted by the selected tile, slow light ribbons and drifting
// particles, like the PS5's animated home background.

void App::DrawPs5Background(const ImVec4 &tint_target)
{
	float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
	float k = 1.0f - std::exp(-dt * 3.0f); // ~0.3 s crossfade
	bg_tint.x += (tint_target.x - bg_tint.x) * k;
	bg_tint.y += (tint_target.y - bg_tint.y) * k;
	bg_tint.z += (tint_target.z - bg_tint.z) * k;
	const ImVec4 &c = bg_tint;

	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImVec2 a = vp->Pos, b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
	float W = vp->Size.x, H = vp->Size.y;
	auto mix = [&](float m, float base) { return (int)std::clamp((base + m) * 255.0f, 0.0f, 255.0f); };
	ImU32 tl = IM_COL32(mix(c.x * 0.30f, 0.02f), mix(c.y * 0.30f, 0.025f), mix(c.z * 0.30f, 0.05f), 255);
	ImU32 tr = IM_COL32(mix(c.x * 0.18f, 0.015f), mix(c.y * 0.18f, 0.02f), mix(c.z * 0.18f, 0.04f), 255);
	ImU32 br = IM_COL32(3, 4, 9, 255), bl = IM_COL32(mix(c.x * 0.08f, 0.01f), mix(c.y * 0.08f, 0.012f), mix(c.z * 0.08f, 0.025f), 255);
	dl->AddRectFilledMultiColor(a, b, tl, tr, br, bl);

	// soft glow behind the tile row
	ImVec2 g(a.x + W * 0.32f, a.y + H * 0.18f);
	for(int i = 12; i >= 1; i--)
		dl->AddCircleFilled(g, H * 0.06f * i, IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), 3), 64);

	float t = (float)ImGui::GetTime();
	// light ribbons (one mesh without anti-aliased edges, so the strips join without seams)
	const int steps = 64;
	ImDrawListFlags saved_flags = dl->Flags;
	for(int r = 0; r < 3; r++)
	{
		float base = H * (0.60f + 0.08f * r), amp = H * (0.05f + 0.02f * r);
		float f1 = 2.2f + r * 0.7f, f2 = 4.7f - r * 0.9f, sp1 = 0.10f + r * 0.035f, sp2 = 0.07f + r * 0.02f;
		float thick = H * (0.10f - 0.02f * r);
		ImVec2 prev_top, prev_bot;
		ImVec2 line[steps + 1];
		for(int i = 0; i <= steps; i++)
		{
			float u = (float)i / steps;
			float y = base + std::sin(u * f1 * 3.14159f + t * sp1 * 6.28f + r * 1.7f) * amp
				+ std::sin(u * f2 * 3.14159f - t * sp2 * 6.28f + r) * amp * 0.45f;
			ImVec2 top(a.x + u * W, a.y + y), bot(a.x + u * W, a.y + y + thick * (0.6f + 0.4f * std::sin(u * 5.0f + t * 0.3f + r)));
			line[i] = top;
			if(i > 0)
			{
				dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
				ImU32 ct = IM_COL32((int)(c.x * 200 + 55), (int)(c.y * 200 + 55), (int)(c.z * 200 + 55), 9 - r * 2);
				dl->AddQuadFilled(prev_top, top, bot, prev_bot, ct);
				dl->Flags = saved_flags;
			}
			prev_top = top;
			prev_bot = bot;
		}
		dl->AddPolyline(line, steps + 1, IM_COL32((int)(c.x * 120 + 135), (int)(c.y * 120 + 135), (int)(c.z * 120 + 135), 34 - r * 8), 0,
			1.2f * ui_scale);
	}
	// particles
	for(int i = 0; i < 46; i++)
	{
		uint32_t h = (uint32_t)i * 2654435761u;
		float sx = (h & 0xffff) / 65535.0f, sy = ((h >> 16) & 0xffff) / 65535.0f;
		float vx = 0.004f + 0.01f * ((i * 37) % 11) / 11.0f, vy = 0.006f + 0.012f * ((i * 53) % 7) / 7.0f;
		float x = std::fmod(sx + t * vx, 1.0f) * W, y = (1.0f - std::fmod(sy + t * vy, 1.0f)) * H;
		float tw = 0.5f + 0.5f * std::sin(t * (0.8f + (i % 5) * 0.3f) + i);
		float rr = (1.0f + (i % 3) * 0.7f) * ui_scale;
		dl->AddCircleFilled(ImVec2(a.x + x, a.y + y), rr, IM_COL32(200, 225, 255, (int)(18 + 50 * tw)), 8);
	}
	// vignette at the bottom
	dl->AddRectFilledMultiColor(ImVec2(a.x, a.y + H * 0.72f), b, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 150),
		IM_COL32(0, 0, 0, 150));
}

// ---------------------------------------------------------------------------
// Top bar: tabs on the left (Games / Settings / Log), controllers and clock on the right

void App::UiTopBar()
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	float x = vp->Pos.x + 72 * ui_scale, y = vp->Pos.y + 30 * ui_scale;

	// logo
	float R = 13.0f * ui_scale;
	ImVec2 c(x + R, y + R + 2 * ui_scale);
	dl->AddCircleFilled(c, R * 0.62f, IM_COL32(255, 255, 255, 235), 32);
	dl->AddEllipse(c, ImVec2(R * 1.3f, R * 0.42f), IM_COL32(255, 255, 255, 200), -0.35f, 48, 1.8f * ui_scale);
	x += R * 2 + 30 * ui_scale;

	struct Tab { const char *name; Screen s; };
	const Tab tabs[] = { { "Games", Screen::Consoles }, { "Settings", Screen::Settings }, { "Log", Screen::Log } };
	ImFont *f = g_heading ? g_heading : ImGui::GetFont();
	for(const Tab &t : tabs)
	{
		bool active = screen == t.s;
		ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, t.name);
		ImGui::SetCursorScreenPos(ImVec2(x - 8 * ui_scale, y - 4 * ui_scale));
		ImGui::PushID(t.name);
		bool clicked = ImGui::InvisibleButton("##tab", ImVec2(ts.x + 16 * ui_scale, ts.y + 8 * ui_scale), ImGuiButtonFlags_None);
		bool hov = ImGui::IsItemHovered();
		ImGui::PopID();
		ImU32 col = active ? IM_COL32(255, 255, 255, 255) : hov ? IM_COL32(220, 225, 235, 255) : IM_COL32(150, 158, 175, 255);
		dl->AddText(f, f->FontSize, ImVec2(x, y), col, t.name);
		if(active)
			dl->AddRectFilled(ImVec2(x, y + ts.y + 6 * ui_scale), ImVec2(x + ts.x, y + ts.y + 9 * ui_scale), IM_COL32(255, 255, 255, 230), 2.0f);
		if(clicked && !active)
		{
			screen = t.s;
			sounds.Play(UiSound::Move);
		}
		x += ts.x + 40 * ui_scale;
	}
	// L1 / R1 switch tabs (PS5 style)
	if(ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
	{
		int i = (int)screen + (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : 2);
		screen = (Screen)(i % 3);
		sounds.Play(UiSound::Move);
	}

	// right side: controllers with battery, then the clock
	float rx = vp->Pos.x + vp->Size.x - 64 * ui_scale;
	std::string clock = ClockText();
	ImVec2 cs = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, clock.c_str());
	rx -= cs.x;
	dl->AddText(f, f->FontSize, ImVec2(rx, y), IM_COL32(255, 255, 255, 240), clock.c_str());
	rx -= 34 * ui_scale;
	auto pads = gamepads.Info();
	float bh = 13.0f * ui_scale;
	for(int i = (int)std::min<size_t>(pads.size(), 4) - 1; i >= 0; i--)
	{
		float bw = bh * 1.9f + bh * 0.18f + 1;
		if(pads[i].battery >= 0 || pads[i].charging)
		{
			rx -= bw;
			DrawBattery(dl, ImVec2(rx, y + (cs.y - bh) * 0.5f), bh, pads[i].battery, pads[i].charging);
			rx -= 8 * ui_scale;
		}
		if(g_icons)
		{
			ImVec2 is = ImGui::CalcTextSize(ICON_GAME);
			rx -= is.x;
			dl->AddText(ImVec2(rx, y + (cs.y - is.y) * 0.5f - 3 * ui_scale), IM_COL32(255, 255, 255, 220), ICON_GAME);
		}
		rx -= 22 * ui_scale;
	}
	if(settings.kbm_enabled && g_icons)
	{
		ImVec2 is = ImGui::CalcTextSize(ICON_KEYBOARD);
		rx -= is.x;
		dl->AddText(ImVec2(rx, y + (cs.y - is.y) * 0.5f - 3 * ui_scale), IM_COL32(255, 255, 255, 150), ICON_KEYBOARD);
	}
}

// ---------------------------------------------------------------------------
// Home: a row of tiles (paired consoles, new consoles on the network, "add by IP"), the selected
// one enlarged with its name beside it, and a hub underneath with the Play button.

void App::UiHome()
{
	struct Tile
	{
		int kind; // 0 paired, 1 found (not paired), 2 add
		std::string id, title;
		RegisteredHost *host = nullptr;
		DiscoveredHost d;
		bool has_d = false;
	};
	std::vector<DiscoveredHost> found = discovery.Hosts();
	std::vector<Tile> tiles;
	for(auto &h : settings.hosts)
	{
		Tile t;
		t.kind = 0;
		t.id = h.mac;
		t.title = h.nickname.empty() ? "PlayStation 5" : h.nickname;
		t.host = &h;
		for(auto &d : found)
			if(d.mac == h.mac)
			{
				t.d = d;
				t.has_d = true;
			}
		tiles.push_back(t);
	}
	for(auto &d : found)
	{
		if(settings.FindHost(d.mac))
			continue;
		Tile t;
		t.kind = 1;
		t.id = "new:" + d.mac;
		t.title = d.name.empty() ? "PlayStation" : d.name;
		t.d = d;
		t.has_d = true;
		tiles.push_back(t);
	}
	{
		Tile t;
		t.kind = 2;
		t.id = "add";
		t.title = "Add a console";
		tiles.push_back(t);
	}
	home_sel = std::clamp(home_sel, 0, (int)tiles.size() - 1);
	Tile &sel = tiles[home_sel];
	ImVec4 tint = sel.kind == 2 ? ImVec4(0.0f, 0.35f, 0.85f, 1) : HostColor(sel.id);
	DrawPs5Background(tint);

	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
	float k = 1.0f - std::exp(-dt * 14.0f);
	const float base = 124 * ui_scale, big = 176 * ui_scale, gap = 18 * ui_scale;
	const float x0 = vp->Pos.x + 72 * ui_scale, y0 = vp->Pos.y + 104 * ui_scale;
	ImFont *hf = g_heading ? g_heading : ImGui::GetFont();

	// keep the selected tile in view (one tile peeks out on the left)
	float sel_left = 0, cursor = 0;
	for(int i = 0; i < (int)tiles.size(); i++)
	{
		float &an = tile_anim[tiles[i].id];
		an += ((i == home_sel ? 1.0f : 0.0f) - an) * k;
		if(i == home_sel)
			sel_left = cursor;
		float sz = base + (big - base) * an;
		ImVec2 ts = hf->CalcTextSizeA(hf->FontSize, FLT_MAX, 0, tiles[i].title.c_str());
		cursor += sz + gap + (ts.x + 36 * ui_scale) * an;
	}
	float want_scroll = std::max(0.0f, sel_left - (base + gap));
	if(cursor - want_scroll < vp->Size.x - x0 - 72 * ui_scale)
		want_scroll = std::max(0.0f, std::min(want_scroll, cursor - (vp->Size.x - x0 - 72 * ui_scale)));
	home_scroll += (want_scroll - home_scroll) * k;

	float x = x0 - home_scroll;
	int activated = -1;
	for(int i = 0; i < (int)tiles.size(); i++)
	{
		Tile &t = tiles[i];
		float an = tile_anim[t.id];
		float sz = base + (big - base) * an;
		ImVec2 p(x, y0), q(x + sz, y0 + sz);
		float r = 16.0f * ui_scale;
		ImGui::SetCursorScreenPos(p);
		ImGui::PushID(t.id.c_str());
		bool was_sel = i == home_sel;
		if(i == home_sel && home_focus_pending)
		{
			ImGui::SetKeyboardFocusHere();
			home_focus_pending = false;
		}
		bool clicked = ImGui::InvisibleButton("##tile", ImVec2(sz, sz));
		bool focused = ImGui::IsItemFocused();
		bool dbl = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
		ImGui::PopID();
		if((focused && !was_sel) || (clicked && !was_sel))
		{
			home_sel = i;
			sounds.Play(UiSound::Move);
		}
		else if((clicked && was_sel) || dbl)
			activated = i;

		// tile art
		ImVec4 hc = t.kind == 2 ? ImVec4(0.25f, 0.3f, 0.4f, 1) : HostColor(t.id);
		if(t.kind == 1)
			hc = ImVec4(hc.x * 0.55f + 0.2f, hc.y * 0.55f + 0.2f, hc.z * 0.55f + 0.25f, 1);
		if(an > 0.02f)
			for(int g = 1; g <= 4; g++)
				dl->AddRect(ImVec2(p.x - g * 2.5f, p.y - g * 2.5f), ImVec2(q.x + g * 2.5f, q.y + g * 2.5f),
					IM_COL32(255, 255, 255, (int)(26 * an / g)), r + g * 2.5f, 0, 2.5f);
		if(t.kind == 2)
			dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 20), r);
		else
		{
			// rounded tile with a diagonal color gradient, plus a soft light from the top
			int v0 = dl->VtxBuffer.Size;
			dl->AddRectFilled(p, q, IM_COL32_WHITE, r);
			ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p, q, Col(hc),
				IM_COL32((int)(hc.x * 40), (int)(hc.y * 40), (int)(hc.z * 60 + 14), 255));
		}
		ImFont *gf = g_hero ? g_hero : (g_title ? g_title : ImGui::GetFont());
		const char *glyph = t.kind == 2 ? (g_icons ? ICON_ADD : "+") : (g_icons ? ICON_GAME : "PS");
		float gs = gf->FontSize * (sz / big) * (t.kind == 2 ? 0.8f : 1.0f);
		ImVec2 gsz = gf->CalcTextSizeA(gs, FLT_MAX, 0, glyph);
		dl->AddText(gf, gs, ImVec2(p.x + (sz - gsz.x) * 0.5f, p.y + (sz - gsz.y) * 0.5f - (g_icons ? gs * 0.06f : 0)), IM_COL32(255, 255, 255, 235),
			glyph);
		if(t.kind != 2)
		{
			// status dot
			ImU32 dc = !t.has_d ? IM_COL32(150, 150, 160, 255) : t.d.ready ? IM_COL32(90, 230, 130, 255) : IM_COL32(255, 180, 70, 255);
			dl->AddCircleFilled(ImVec2(p.x + 16 * ui_scale, q.y - 16 * ui_scale), 5.0f * ui_scale, dc, 16);
			if(t.kind == 1)
			{
				const char *nb = "NEW";
				ImVec2 ns = ImGui::CalcTextSize(nb);
				ImVec2 np(q.x - ns.x - 22 * ui_scale, p.y + 10 * ui_scale);
				dl->AddRectFilled(np, ImVec2(np.x + ns.x + 12 * ui_scale, np.y + ns.y + 4 * ui_scale), kAccentU, 6 * ui_scale);
				dl->AddText(ImVec2(np.x + 6 * ui_scale, np.y + 2 * ui_scale), IM_COL32(255, 255, 255, 255), nb);
			}
		}
		if(an > 0.6f || i == home_sel)
			dl->AddRect(p, q, IM_COL32(255, 255, 255, (int)(255 * an)), r, 0, 3.0f * ui_scale);
		else
			dl->AddRect(p, q, IM_COL32(255, 255, 255, 30), r, 0, 1.0f);
		x += sz + gap;
		// name beside the selected tile
		ImVec2 ts = hf->CalcTextSizeA(hf->FontSize, FLT_MAX, 0, t.title.c_str());
		float reserve = (ts.x + 36 * ui_scale) * an;
		if(an > 0.05f)
		{
			dl->PushClipRect(ImVec2(x, y0), ImVec2(x + reserve, y0 + big), true);
			dl->AddText(hf, hf->FontSize, ImVec2(x + 6 * ui_scale, y0 + sz * 0.5f - ts.y * 0.5f), IM_COL32(255, 255, 255, (int)(255 * an)),
				t.title.c_str());
			dl->PopClipRect();
		}
		x += reserve;
	}

	// ---- hub of the selected tile
	float hy = y0 + big + 46 * ui_scale;
	ImGui::SetCursorScreenPos(ImVec2(x0, hy));
	ImGui::BeginGroup();
	if(g_hero)
		ImGui::PushFont(g_hero);
	ImGui::TextUnformatted(sel.title.c_str());
	if(g_hero)
		ImGui::PopFont();

	bool busy = phase != Phase::Idle;
	float bh = 60 * ui_scale;
	auto white_button = [&](const char *label, float w) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.96f, 0.97f, 1.0f, 1));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 1));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.82f, 0.85f, 0.9f, 1));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.06f, 0.1f, 1));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
		if(g_heading)
			ImGui::PushFont(g_heading);
		bool r = ImGui::Button(label, ImVec2(w, bh));
		if(g_heading)
			ImGui::PopFont();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(4);
		return r;
	};
	auto round_button = [&](const char *icon, const char *fallback, const char *tip) {
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.12f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.24f));
		bool r = ImGui::Button(g_icons ? icon : fallback, ImVec2(bh, bh));
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
		if(ImGui::IsItemHovered() || ImGui::IsItemFocused())
			ImGui::SetTooltip("%s", tip);
		return r;
	};

	if(sel.kind == 0)
	{
		RegisteredHost &h = *sel.host;
		std::string sub;
		if(sel.has_d && !sel.d.running_app.empty())
			sub = "Playing " + sel.d.running_app;
		else if(!h.last_app.empty())
			sub = "Last played: " + h.last_app;
		const std::string &game = sel.has_d && !sel.d.running_app.empty() ? sel.d.running_app : h.last_app;
		if(!game.empty())
		{
			auto it = settings.playtime.find(SanitizeKey(game));
			if(it != settings.playtime.end() && it->second >= 60)
				sub += "   \xC2\xB7   " + FormatDuration(it->second) + " played here";
		}
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.86f, 0.94f, 1));
		if(g_heading)
			ImGui::PushFont(g_heading);
		ImGui::TextUnformatted(sub.empty() ? (sel.has_d ? sel.d.address.c_str() : "Not seen on the network yet") : sub.c_str());
		if(g_heading)
			ImGui::PopFont();
		ImGui::PopStyleColor();
		if(sel.has_d)
			StatusPill(sel.d.ready ? "Ready" : "Rest mode", sel.d.ready ? kGood : kWarn);
		else
			StatusPill(h.last_address.empty() ? "Not found" : ("Not found  -  last at " + h.last_address).c_str(), kTextDim);
		ImGui::Dummy(ImVec2(0, 10 * ui_scale));
		ImGui::BeginDisabled(busy);
		if(white_button(sel.has_d && !sel.d.ready ? "Wake & Play" : "Play", 300 * ui_scale) || activated == home_sel)
		{
			sounds.Play(UiSound::Select);
			BeginConnect(h.mac);
		}
		ImGui::SameLine(0, 14 * ui_scale);
		if(round_button(ICON_POWER, "W", "Wake from rest mode"))
		{
			Discovery::Wakeup(sel.has_d ? sel.d.address : h.last_address, h.regist_key, h.ps5);
			ShowToast("Wake-up sent to " + sel.title);
			sounds.Play(UiSound::Toggle);
		}
		ImGui::SameLine(0, 14 * ui_scale);
		if(round_button(ICON_DELETE, "X", "Forget this console"))
			ImGui::OpenPopup("##forget");
		ImGui::EndDisabled();
		if(ImGui::BeginPopup("##forget"))
		{
			ImGui::Text("Forget %s?", sel.title.c_str());
			HelpText("You'll need to pair it again to play.");
			if(PrimaryButton("Forget", ImVec2(140 * ui_scale, 0)))
			{
				forget_mac = h.mac;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if(ImGui::Button("Cancel", ImVec2(120 * ui_scale, 0)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
	else if(sel.kind == 1)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.86f, 0.94f, 1));
		ImGui::Text("%s  on  %s", sel.d.ps5 ? "PS5" : "PS4", sel.d.address.c_str());
		ImGui::PopStyleColor();
		StatusPill(sel.d.ready ? "Found on your network - not paired" : "Rest mode - turn it on to pair", sel.d.ready ? kAccentHi : kWarn);
		ImGui::Dummy(ImVec2(0, 10 * ui_scale));
		ImGui::BeginDisabled(!sel.d.ready || busy);
		if(white_button("Pair", 300 * ui_scale) || (activated == home_sel && sel.d.ready && !busy))
		{
			sounds.Play(UiSound::Select);
			pair_open = true;
			pair_address = sel.d.address;
			pair_target = chiaki_target_is_unknown((ChiakiTarget)sel.d.target) ? (sel.d.ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10)
				: sel.d.target;
			memset(pin_buf, 0, sizeof(pin_buf));
		}
		ImGui::EndDisabled();
	}
	else
	{
		HelpText(settings.hosts.empty() && found.empty()
			? "Searching your network... If your PS5 doesn't appear, enter its IP address (PS5: Settings > Network > Connection Status)."
			: "For a PS5 on another subnet, or when your network blocks discovery.");
		ImGui::Dummy(ImVec2(0, 6 * ui_scale));
		ImGui::SetNextItemWidth(300 * ui_scale);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18 * ui_scale, (bh - ImGui::GetFontSize()) * 0.5f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bh * 0.5f);
		if(activated == home_sel)
			ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputTextWithHint("##ip", "192.168.1.50", manual_ip_buf, sizeof(manual_ip_buf), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::PopStyleVar(2);
		ImGui::SameLine(0, 14 * ui_scale);
		if((white_button("Add", 160 * ui_scale) || enter) && manual_ip_buf[0])
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
			sounds.Play(UiSound::Select);
			ShowToast("Looking for a console at " + ip + "...");
		}
		if(!settings.manual_hosts.empty())
		{
			ImGui::Dummy(ImVec2(0, 4 * ui_scale));
			ImGui::TextDisabled("Probed by IP:");
			for(size_t i = 0; i < settings.manual_hosts.size(); i++)
			{
				ImGui::PushID((int)(1000 + i));
				ImGui::SameLine();
				char lbl[96];
				snprintf(lbl, sizeof(lbl), "%s  %s", settings.manual_hosts[i].address.c_str(), Ico(ICON_CANCEL));
				if(ImGui::SmallButton(lbl))
				{
					settings.manual_hosts.erase(settings.manual_hosts.begin() + i);
					MarkSettingsDirty();
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
		}
	}
	ImGui::EndGroup();
	float hub_bottom = ImGui::GetItemRectMax().y;

	if(!forget_mac.empty())
	{
		settings.RemoveHost(forget_mac);
		forget_mac.clear();
		home_sel = 0;
		MarkSettingsDirty();
		sounds.Play(UiSound::Back);
	}

	// ---- info cards along the bottom
	float ch = 150 * ui_scale, cw = 372 * ui_scale, cg = 18 * ui_scale;
	float cy = std::max(hub_bottom + 36 * ui_scale, vp->Pos.y + vp->Size.y - ch - 64 * ui_scale);
	float cx = x0;
	auto card = [&](const char *icon, const char *title, const std::vector<std::string> &lines) {
		if(cx + cw > vp->Pos.x + vp->Size.x - 40 * ui_scale)
			return;
		ImVec2 p(cx, cy), q(cx + cw, cy + ch);
		float r = 16 * ui_scale;
		dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 14), r);
		dl->AddRect(p, q, IM_COL32(255, 255, 255, 26), r, 0, 1.0f);
		float tx = p.x + 20 * ui_scale, ty = p.y + 16 * ui_scale;
		std::string head = std::string(Ico(icon)) + (g_icons ? "  " : "") + title;
		dl->AddText(hf, hf->FontSize, ImVec2(tx, ty), IM_COL32(255, 255, 255, 240), head.c_str());
		ty += hf->FontSize + 10 * ui_scale;
		for(const auto &l : lines)
		{
			dl->PushClipRect(p, ImVec2(q.x - 12 * ui_scale, q.y), true);
			dl->AddText(ImVec2(tx, ty), IM_COL32(190, 198, 214, 255), l.c_str());
			dl->PopClipRect();
			ty += ImGui::GetTextLineHeightWithSpacing();
		}
		cx += cw + cg;
	};
	{
		const char *ups[] = { "Off", "Enhanced", "AI super resolution" };
		const char *fgs[] = { "Off", "Predictive", "Interpolated" };
		char l1[96], l2[96], l3[96];
		snprintf(l1, sizeof(l1), "%dp  \xC2\xB7  %d fps  \xC2\xB7  %s%s", settings.resolution, settings.fps,
			settings.codec == VideoCodec::H265 ? "HEVC" : "H.264", settings.hdr && renderer.DisplayHdr() ? "  \xC2\xB7  HDR" : "");
		if(settings.upscale_target > 0)
			snprintf(l2, sizeof(l2), "Upscaling: %s to %dp", ups[std::clamp(settings.upscale_mode, 0, 2)], settings.upscale_target);
		else
			snprintf(l2, sizeof(l2), "Upscaling: %s", ups[std::clamp(settings.upscale_mode, 0, 2)]);
		snprintf(l3, sizeof(l3), "Frame generation: %s", fgs[std::clamp(settings.frame_gen, 0, 2)]);
		card(ICON_VIDEO, "Stream", { l1, l2, l3 });
	}
	{
		char l1[96], l2[96];
		snprintf(l1, sizeof(l1), "%s%s", renderer.GpuVendorName().c_str(), renderer.AiSuperResolutionAvailable() ? "  \xC2\xB7  AI upscaling" : "");
		snprintf(l2, sizeof(l2), "Display %d Hz  \xC2\xB7  %s", renderer.DisplayHz(), renderer.DisplayHdr() ? "HDR" : "SDR");
		card(ICON_DISPLAY, "This PC", { l1, l2, settings.kbm_enabled ? "Keyboard & mouse: on" : "Keyboard & mouse: off" });
	}
	{
		std::vector<std::string> lines;
		for(auto &pi : gamepads.Info())
		{
			if(lines.size() == 3)
				break;
			std::string l = pi.name;
			if(pi.battery >= 0)
				l += "  \xC2\xB7  " + std::to_string(pi.battery) + "%" + (pi.charging ? " charging" : "");
			else if(pi.wired)
				l += "  \xC2\xB7  USB";
			lines.push_back(l);
		}
		if(lines.empty())
			lines.push_back("No controller - plug one in or use keyboard & mouse");
		card(ICON_GAME, "Controllers", lines);
	}

	// ---- button hints, bottom right
	{
		float gh = 13.0f * ui_scale, hx = vp->Pos.x + vp->Size.x - 64 * ui_scale, hy2 = vp->Pos.y + vp->Size.y - 40 * ui_scale;
		auto hint = [&](int glyph, const char *shoulder, const char *text) {
			ImVec2 ts = ImGui::CalcTextSize(text);
			hx -= ts.x;
			dl->AddText(ImVec2(hx, hy2 - ts.y * 0.5f), IM_COL32(220, 225, 235, 230), text);
			hx -= 10 * ui_scale;
			if(shoulder)
			{
				ImVec2 ss = ImGui::CalcTextSize(shoulder);
				hx -= ss.x + gh * 1.6f;
				DrawShoulder(dl, ImVec2(hx, hy2 - gh), gh * 2, shoulder);
			}
			else
			{
				hx -= gh;
				DrawPsGlyph(dl, ImVec2(hx, hy2), gh, glyph);
				hx -= gh;
			}
			hx -= 26 * ui_scale;
		};
		hint(0, nullptr, sel.kind == 0 ? "Play" : sel.kind == 1 ? "Pair" : "Enter IP");
		hint(0, "L1 R1", "Switch tab");
	}
}

// ---------------------------------------------------------------------------
// Boot intro: the logo forms out of light, then the home screen fades in. Any key skips it.

void App::UiBootIntro()
{
	if(!boot_start_ms)
		return;
	float t = (NowMs() - boot_start_ms) / 1000.0f;
	const float fade_start = 1.9f, end = 2.6f;
	ImGuiIO &io = ImGui::GetIO();
	bool skip = ImGui::IsMouseClicked(0) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape)
		|| ImGui::IsKeyPressed(ImGuiKey_Space) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown);
	if(skip && t < fade_start)
	{
		boot_start_ms = NowMs() - (uint64_t)(fade_start * 1000);
		t = fade_start;
	}
	if(t >= end)
	{
		boot_start_ms = 0;
		return;
	}
	(void)io;
	float alpha = 1.0f - Smooth01((t - fade_start) / (end - fade_start));
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImVec2 a = vp->Pos, b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), c = vp->GetCenter();
	auto A = [&](float v) { return (int)std::clamp(v * alpha * 255.0f, 0.0f, 255.0f); };
	dl->AddRectFilled(a, b, IM_COL32(2, 3, 8, A(1.0f)));
	float R = vp->Size.y * 0.075f;
	c.y -= R * 0.6f;
	// glow
	float g = Smooth01(t / 1.2f);
	for(int i = 14; i >= 1; i--)
		dl->AddCircleFilled(c, R * 0.35f * i * (0.6f + 0.4f * g), IM_COL32(0, 100, 255, A(0.012f * g)), 64);
	// ring drawn progressively: the half behind the planet first, then the planet, then the front half
	float ring = Smooth01((t - 0.25f) / 0.9f);
	auto draw_ring = [&](bool front) {
		if(ring <= 0)
			return;
		const int n = 120;
		ImVec2 pts[n + 1];
		int cnt = 0;
		float rot = -0.35f, start = 3.14159f * 0.9f;
		auto at = [&](float u) {
			float ex = std::cos(u) * R * 1.32f, ey = std::sin(u) * R * 0.42f;
			return ImVec2(c.x + ex * std::cos(rot) - ey * std::sin(rot), c.y + ex * std::sin(rot) + ey * std::cos(rot));
		};
		ImU32 rc = IM_COL32(150, 200, 255, A(0.95f));
		for(int i = 1; i <= n; i++)
		{
			float u0 = start + (float)(i - 1) / n * 6.28318f * ring, u1 = start + (float)i / n * 6.28318f * ring;
			bool edge_front = std::sin((u0 + u1) * 0.5f) > 0.0f; // which side this piece of the ring is on
			if(edge_front == front)
			{
				if(!cnt)
					pts[cnt++] = at(u0);
				pts[cnt++] = at(u1);
			}
			else if(cnt)
			{
				dl->AddPolyline(pts, cnt, rc, 0, R * 0.07f);
				cnt = 0;
			}
		}
		if(cnt > 1)
			dl->AddPolyline(pts, cnt, rc, 0, R * 0.07f);
	};
	draw_ring(false);
	dl->AddCircleFilled(c, R * 0.62f * Smooth01(t / 0.7f), IM_COL32(255, 255, 255, A(0.95f)), 64);
	draw_ring(true);
	// title
	float ta = Smooth01((t - 0.7f) / 0.6f);
	if(ta > 0 && g_hero)
	{
		const char *title = "Orbit Play";
		float spacing = (1.0f - ta) * 14.0f * ui_scale; // letters settle together
		float w = 0;
		for(const char *ch = title; *ch; ch++)
			w += g_hero->CalcTextSizeA(g_hero->FontSize, FLT_MAX, 0, ch, ch + 1).x + spacing;
		float x = c.x - w * 0.5f, y = c.y + R * 1.25f;
		for(const char *ch = title; *ch; ch++)
		{
			dl->AddText(g_hero, g_hero->FontSize, ImVec2(x, y), IM_COL32(255, 255, 255, A(ta)), ch, ch + 1);
			x += g_hero->CalcTextSizeA(g_hero->FontSize, FLT_MAX, 0, ch, ch + 1).x + spacing;
		}
		const char *sub = "PS5 Remote Play";
		ImVec2 ss = ImGui::CalcTextSize(sub);
		dl->AddText(ImVec2(c.x - ss.x * 0.5f, y + g_hero->FontSize + 10 * ui_scale), IM_COL32(170, 185, 210, A(ta * 0.9f)), sub);
	}
}

// ---------------------------------------------------------------------------
// Control-center bar along the bottom while the stream menu is open (PS5 style)

void App::UiControlCenterBar()
{
	if(!session)
		return;
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	float h = 92 * ui_scale;
	ImVec2 a(vp->Pos.x, vp->Pos.y + vp->Size.y - h), b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
	dl->AddRectFilledMultiColor(ImVec2(a.x, a.y - 60 * ui_scale), ImVec2(b.x, a.y), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
		IM_COL32(4, 6, 14, 200), IM_COL32(4, 6, 14, 200));
	dl->AddRectFilled(a, b, IM_COL32(8, 11, 22, 235));
	dl->AddLine(a, ImVec2(b.x, a.y), IM_COL32(255, 255, 255, 30), 1.0f);
	ImFont *hf = g_heading ? g_heading : ImGui::GetFont();
	float x = a.x + 48 * ui_scale, cy = a.y + h * 0.5f;

	// game + session time
	std::string game = current_game.empty() ? (active_host.nickname.empty() ? "PlayStation 5" : active_host.nickname) : current_game;
	dl->AddText(hf, hf->FontSize, ImVec2(x, cy - hf->FontSize + 2 * ui_scale), IM_COL32(255, 255, 255, 245), game.c_str());
	std::string sub = "Session " + FormatDuration((NowMs() - stream_start_ms) / 1000);
	if(!current_game.empty())
	{
		auto it = settings.playtime.find(SanitizeKey(current_game));
		uint64_t total = (it != settings.playtime.end() ? it->second : 0) + (NowMs() - game_since_ms) / 1000;
		sub += "   \xC2\xB7   " + FormatDuration(total) + " total";
	}
	dl->AddText(ImVec2(x, cy + 4 * ui_scale), IM_COL32(170, 180, 200, 255), sub.c_str());

	// right: network, controllers, clock
	float rx = b.x - 48 * ui_scale;
	std::string clock = ClockText();
	ImVec2 cs = hf->CalcTextSizeA(hf->FontSize, FLT_MAX, 0, clock.c_str());
	rx -= cs.x;
	dl->AddText(hf, hf->FontSize, ImVec2(rx, cy - cs.y * 0.5f), IM_COL32(255, 255, 255, 240), clock.c_str());
	rx -= 40 * ui_scale;
	float bh = 13.0f * ui_scale;
	auto pads = gamepads.Info();
	for(int i = (int)std::min<size_t>(pads.size(), 4) - 1; i >= 0; i--)
	{
		if(pads[i].battery >= 0 || pads[i].charging)
		{
			char pct[16];
			snprintf(pct, sizeof(pct), "%d%%", pads[i].battery);
			ImVec2 ps = ImGui::CalcTextSize(pct);
			rx -= ps.x;
			dl->AddText(ImVec2(rx, cy - ps.y * 0.5f), IM_COL32(220, 225, 235, 255), pct);
			rx -= 6 * ui_scale + bh * 1.9f + bh * 0.18f + 1;
			DrawBattery(dl, ImVec2(rx, cy - bh * 0.5f), bh, pads[i].battery, pads[i].charging);
			rx -= 6 * ui_scale;
		}
		if(g_icons)
		{
			ImVec2 is = ImGui::CalcTextSize(ICON_GAME);
			rx -= is.x;
			dl->AddText(ImVec2(rx, cy - is.y * 0.5f - 3 * ui_scale), IM_COL32(255, 255, 255, 220), ICON_GAME);
		}
		rx -= 28 * ui_scale;
	}
	StreamStats s = session->Stats();
	char net[96];
	snprintf(net, sizeof(net), "%s%s%.0f ms  \xC2\xB7  %.0f Mbps", Ico(ICON_WIFI), g_icons ? "  " : "", s.rtt_ms, s.mbps);
	ImVec2 ns = ImGui::CalcTextSize(net);
	rx -= ns.x;
	ImU32 nc = s.rtt_ms < 12 ? IM_COL32(150, 235, 170, 255) : s.rtt_ms < 30 ? IM_COL32(255, 205, 110, 255) : IM_COL32(255, 120, 120, 255);
	dl->AddText(ImVec2(rx, cy - ns.y * 0.5f), nc, net);

	// center hint
	const char *hint = "Esc / F1  back to the game";
	ImVec2 hs = ImGui::CalcTextSize(hint);
	float hx = vp->GetCenter().x - hs.x * 0.5f;
	if(hx > x + 300 * ui_scale && hx + hs.x < rx - 30 * ui_scale)
	{
		DrawPsGlyph(dl, ImVec2(hx - 22 * ui_scale, cy), 12 * ui_scale, 1);
		dl->AddText(ImVec2(hx, cy - hs.y * 0.5f), IM_COL32(170, 180, 200, 255), hint);
	}
}

// ---------------------------------------------------------------------------
// Settings pieces

void App::UiInterfaceSection()
{
	bool changed = false;
	Row("Sounds");
	if(ImGui::Checkbox("Interface sounds", &settings.ui_sounds))
	{
		sounds.SetEnabled(settings.ui_sounds);
		sounds.Play(UiSound::Toggle);
		changed = true;
	}
	if(settings.ui_sounds)
	{
		Row("Sound volume");
		if(ImGui::SliderFloat("##uisv", &settings.ui_sound_volume, 0.05f, 1.0f, "%.2f"))
		{
			sounds.SetVolume(settings.ui_sound_volume);
			changed = true;
		}
		if(ImGui::IsItemDeactivatedAfterEdit())
			sounds.Play(UiSound::Select);
	}
	Row("Start-up");
	changed |= ImGui::Checkbox("Play the intro animation", &settings.boot_animation);
	Row("Controllers");
	changed |= ImGui::Checkbox("Warn when a controller's battery is low", &settings.low_battery_warning);
	Row("Game profiles");
	changed |= ImGui::Checkbox("Load a game's saved settings when it starts", &settings.game_profiles);
	RowHelp("Save a profile from the stream menu's Game tab while playing. Upscaling, detail, frame generation, "
		"picture and mouse settings are stored per game.");
	if(!settings.profiles.empty())
	{
		Row("Saved profiles");
		ImGui::BeginGroup();
		std::string remove;
		for(auto &p : settings.profiles)
		{
			ImGui::PushID(p.game.c_str());
			if(ImGui::SmallButton(g_icons ? ICON_DELETE : "x"))
				remove = p.game;
			ImGui::SameLine();
			ImGui::TextUnformatted(p.game.c_str());
			ImGui::PopID();
		}
		ImGui::EndGroup();
		if(!remove.empty())
		{
			settings.RemoveProfile(remove);
			changed = true;
		}
	}
	if(changed)
		MarkSettingsDirty();
}

// Stream menu "Game" tab: what's running, play time, the game's profile
void App::UiGameSection()
{
	if(current_game.empty())
	{
		HelpText("The console hasn't reported a running game yet. It shows up here a few seconds after a game starts.");
		Row("Session");
		ImGui::TextUnformatted(FormatDuration((NowMs() - stream_start_ms) / 1000).c_str());
		return;
	}
	if(g_heading)
		ImGui::PushFont(g_heading);
	ImGui::TextUnformatted(current_game.c_str());
	if(g_heading)
		ImGui::PopFont();
	auto it = settings.playtime.find(SanitizeKey(current_game));
	uint64_t total = (it != settings.playtime.end() ? it->second : 0) + (NowMs() - game_since_ms) / 1000;
	Row("This session");
	ImGui::TextUnformatted(FormatDuration((NowMs() - stream_start_ms) / 1000).c_str());
	Row("Played with Orbit Play");
	ImGui::TextUnformatted(FormatDuration(total).c_str());

	Row("Profile");
	GameProfile *p = settings.FindProfile(current_game);
	ImGui::BeginGroup();
	if(PrimaryButton(p ? "Update profile with current settings" : "Save current settings for this game"))
	{
		GameProfile np = settings.CaptureProfile(current_game);
		if(p)
			*p = np;
		else
			settings.profiles.push_back(np);
		MarkSettingsDirty();
		ShowToast("Saved settings for " + current_game);
		sounds.Play(UiSound::Select);
	}
	if(p)
	{
		ImGui::SameLine();
		if(ImGui::Button("Delete profile"))
		{
			settings.RemoveProfile(current_game);
			MarkSettingsDirty();
			ShowToast("Profile removed");
			sounds.Play(UiSound::Back);
		}
	}
	ImGui::EndGroup();
	RowHelp(p ? "This game has its own settings: they load automatically every time it starts."
		: "Stores the upscaling, detail, frame generation, picture and mouse settings you use now, and loads them every time this game starts.");
	Row("Auto-load");
	if(ImGui::Checkbox("Load profiles automatically", &settings.game_profiles))
		MarkSettingsDirty();
}
