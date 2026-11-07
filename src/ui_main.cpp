// SPDX-License-Identifier: AGPL-3.0-only
// Main screens. Layout follows console settings menus: a large light title, a list of sections on
// the left, and full-width rows on the right with the value or switch at the far right.
#include "app.h"
#include "log.h"
#include "ui_style.h"

#include <chiaki/time.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>

static uint64_t NowMs() { return chiaki_time_now_monotonic_ms(); }
static float Smooth01(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }

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

// Controller battery: outline with a fill level, blue while charging
static float DrawBattery(ImDrawList *dl, ImVec2 p, float h, int pct, bool charging)
{
	float w = h * 1.9f, nub = h * 0.18f;
	dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 190), h * 0.18f, 0, 1.3f);
	dl->AddRectFilled(ImVec2(p.x + w + 1, p.y + h * 0.3f), ImVec2(p.x + w + 1 + nub, p.y + h * 0.7f), IM_COL32(255, 255, 255, 190), 1.0f);
	if(pct >= 0)
	{
		ImU32 c = charging ? IM_COL32(120, 170, 250, 255) : pct > 30 ? IM_COL32(225, 228, 235, 255) : pct > 15 ? IM_COL32(245, 185, 90, 255)
			: IM_COL32(240, 90, 90, 255);
		float inner = (w - 4) * std::clamp(pct, 4, 100) / 100.0f;
		dl->AddRectFilled(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + 2 + inner, p.y + h - 2), c, 1.0f);
	}
	return w + nub + 1;
}

// ---------------------------------------------------------------------------
// Background: dark slate with a soft light falling in from the top-left. Computed once per
// window size at full resolution in floating point, then dithered to 8 bits, so the falloff is
// smooth with no visible steps.

void App::ReleaseBackground()
{
	if(bg_srv)
		bg_srv->Release();
	bg_srv = nullptr;
	bg_w = bg_h = 0;
}

void App::DrawBackground()
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	int w = std::max(16, (int)vp->Size.x), h = std::max(16, (int)vp->Size.y);
	if(!bg_srv || w != bg_w || h != bg_h)
	{
		ReleaseBackground();
		std::vector<uint32_t> px((size_t)w * h);
		const float lx = -0.10f * h + 0.10f * w, ly = -0.42f * h;   // where the light comes from
		const float bx = 0.30f * w - lx, by = 1.0f * h - ly;           // beam direction
		const float blen = std::sqrt(bx * bx + by * by);
		uint32_t seed = 0x9e3779b9u;
		auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xffffff) / 16777216.0f; };
		for(int y = 0; y < h; y++)
		{
			float v = (float)y / h;
			for(int x = 0; x < w; x++)
			{
				float u = (float)x / w;
				// base: charcoal at the top, near-black at the bottom, slightly cooler to the right
				float r = 0.090f - 0.055f * v - 0.012f * u;
				float g = 0.094f - 0.056f * v - 0.008f * u;
				float b = 0.112f - 0.060f * v + 0.004f * u;
				// light: soft round glow plus a wide beam, warm white
				float dx = x - lx, dy = y - ly;
				float d = std::sqrt(dx * dx + dy * dy) / h;
				float glow = std::exp(-d * d * 2.0f) * 0.15f;
				float along = (dx * bx + dy * by) / blen / h;
				float across = std::fabs(dx * by - dy * bx) / blen / h;
				float width = 0.10f + along * 0.42f;
				float beam = along > 0 ? std::exp(-(across * across) / (width * width)) * std::exp(-along * 1.15f) * 0.075f : 0.0f;
				float l = glow + beam;
				r += l * 1.00f;
				g += l * 0.94f;
				b += l * 0.88f;
				// gentle vignette at the bottom corners
				float vig = 1.0f - 0.25f * std::pow(std::max(0.0f, v - 0.55f) / 0.45f, 2.0f) * (0.6f + 0.4f * std::fabs(u - 0.5f) * 2.0f);
				r *= vig;
				g *= vig;
				b *= vig;
				// to sRGB-ish 8 bits with triangular dither (no banding)
				auto q = [&](float c) {
					float dither = (rnd() + rnd() - 1.0f);
					return (uint32_t)std::clamp(std::pow(std::max(c, 0.0f), 1.0f / 1.08f) * 255.0f + dither + 0.5f, 0.0f, 255.0f);
				};
				px[(size_t)y * w + x] = q(r) | (q(g) << 8) | (q(b) << 16) | 0xff000000u;
			}
		}
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = (UINT)w;
		td.Height = (UINT)h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA sd = { px.data(), (UINT)(w * 4), 0 };
		ID3D11Texture2D *tex = nullptr;
		if(SUCCEEDED(renderer.Device()->CreateTexture2D(&td, &sd, &tex)))
		{
			renderer.Device()->CreateShaderResourceView(tex, nullptr, &bg_srv);
			tex->Release();
		}
		bg_w = w;
		bg_h = h;
	}
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImVec2 a = vp->Pos, b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
	if(bg_srv)
		dl->AddImage((ImTextureID)(intptr_t)bg_srv, a, b);
	else
		dl->AddRectFilledMultiColor(a, b, IM_COL32(24, 25, 30, 255), IM_COL32(20, 22, 28, 255), IM_COL32(9, 10, 14, 255), IM_COL32(10, 11, 15, 255));
}

// ---------------------------------------------------------------------------
// Rows: label on the left, control on the right, a hairline under each row. The focused row
// (mouse or controller) gets a thin light outline.

void App::RowsBegin()
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	row_splitter.Split(dl, 2);
	row_splitter.SetCurrentChannel(dl, 1);
	rows_active = true;
	row_open = false;
}

void App::RowsEnd()
{
	if(!rows_active)
		return;
	CloseRow();
	row_splitter.Merge(ImGui::GetWindowDrawList());
	rows_active = false;
}

void App::CloseRow()
{
	if(!row_open)
		return;
	row_open = false;
	ImGuiStyle &st = ImGui::GetStyle();
	float pad = 12.0f * ui_scale, padx = 18.0f * ui_scale;
	float y1 = ImGui::GetCursorScreenPos().y - st.ItemSpacing.y + pad;
	ImVec2 a(row_x0, row_y0), b(row_x1, y1);

	ImGuiContext &g = *GImGui;
	ImGuiWindow *w = ImGui::GetCurrentWindow();
	bool focused = false;
	if(g.NavWindow == w && g.NavId && g.NavCursorVisible)
	{
		ImRect r = ImGui::WindowRectRelToAbs(w, w->NavRectRel[0]);
		focused = r.Min.y >= a.y - 1 && r.Max.y <= b.y + 1;
	}
	bool hovered = !focused && ImGui::IsMouseHoveringRect(a, b, false)
		&& ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

	ImDrawList *dl = ImGui::GetWindowDrawList();
	row_splitter.SetCurrentChannel(dl, 0);
	float r = 4.0f * ui_scale;
	if(focused)
	{
		dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 14), r);
		dl->AddRect(a, b, IM_COL32(235, 238, 245, 200), r, 0, 1.6f * ui_scale);
	}
	else if(hovered)
		dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 9), r);
	else
		dl->AddLine(ImVec2(a.x + padx, b.y), ImVec2(b.x - padx, b.y), IM_COL32(255, 255, 255, 22), 1.0f);
	row_splitter.SetCurrentChannel(dl, 1);

	ImGui::SetCursorScreenPos(ImVec2(row_x0, y1 - st.ItemSpacing.y));
	ImGui::Dummy(ImVec2(0, 0));
}

void App::Row(const char *label)
{
	CloseRow();
	float pad = 12.0f * ui_scale, padx = 18.0f * ui_scale;
	ImVec2 c = ImGui::GetCursorScreenPos();
	if(rows_active)
	{
		row_open = true;
		row_x0 = c.x;
		row_x1 = c.x + ImGui::GetContentRegionAvail().x;
		row_y0 = c.y;
		ImGui::SetCursorScreenPos(ImVec2(c.x + padx, c.y + pad));
	}
	// label in the larger font, centered on the control's line, clipped before the control column
	ImFont *f = g_heading ? g_heading : ImGui::GetFont();
	ImVec2 lp = ImGui::GetCursorScreenPos();
	float fh = ImGui::GetFrameHeight();
	float clip_r = ImGui::GetWindowPos().x - ImGui::GetScrollX() + row_label_w - 14.0f * ui_scale;
	if(*label)
	{
		ImDrawList *dl = ImGui::GetWindowDrawList();
		dl->PushClipRect(ImVec2(lp.x, lp.y - 4), ImVec2(std::max(lp.x, clip_r), lp.y + fh + 4), true);
		dl->AddText(f, f->FontSize, ImVec2(lp.x, lp.y + (fh - f->FontSize) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), label);
		dl->PopClipRect();
	}
	ImGui::Dummy(ImVec2(1, fh));
	ImGui::SameLine(row_label_w);
	ImGui::SetNextItemWidth(row_item_w);
}

void App::RowHelp(const char *text)
{
	float padx = 18.0f * ui_scale;
	ImGui::SetCursorPosX(row_label_w);
	float right = rows_active ? row_x1 - padx : ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (right - ImGui::GetCursorScreenPos().x));
	ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
	ImGui::TextUnformatted(text);
	ImGui::PopStyleColor();
	ImGui::PopTextWrapPos();
}

// Group heading inside a page ("KEYBOARD & MOUSE")
void App::SubHeader(const char *text)
{
	CloseRow();
	ImGui::Dummy(ImVec2(0, 14 * ui_scale));
	std::string up = text;
	for(char &ch : up)
		ch = (char)toupper((unsigned char)ch);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18 * ui_scale);
	ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
	ImGui::TextUnformatted(up.c_str());
	ImGui::PopStyleColor();
	ImGui::Dummy(ImVec2(0, 2 * ui_scale));
}

void App::SetRowLayout(bool compact)
{
	// controls live in a column on the right, like values on a console settings screen
	float avail = ImGui::GetContentRegionAvail().x;
	float ctrl = std::clamp(avail * 0.45f, 300.0f * ui_scale, (compact ? 440.0f : 500.0f) * ui_scale);
	row_label_w = ImGui::GetCursorPosX() + std::max(180.0f * ui_scale, avail - ctrl - 18.0f * ui_scale);
	row_item_w = -18.0f * ui_scale;
}

// Left-column entry: plain text; the selected one sits in an outlined box
bool App::ListItem(const char *label, bool active)
{
	ImVec2 size(ImGui::GetContentRegionAvail().x, 52.0f * ui_scale);
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::PushID(label);
	bool clicked = ImGui::InvisibleButton("##li", size);
	ImGui::PopID();
	bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 q(p.x + size.x, p.y + size.y);
	float r = 4.0f * ui_scale;
	if(active || foc)
	{
		dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, active ? 16 : 10), r);
		dl->AddRect(p, q, IM_COL32(235, 238, 245, active ? 215 : 150), r, 0, 1.6f * ui_scale);
	}
	else if(hov)
		dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 9), r);
	ImFont *f = g_heading ? g_heading : ImGui::GetFont();
	ImU32 col = active || hov || foc ? IM_COL32(242, 244, 248, 255) : IM_COL32(196, 200, 210, 255);
	dl->AddText(f, f->FontSize, ImVec2(p.x + 22 * ui_scale, p.y + (size.y - f->FontSize) * 0.5f - 1), col, label);
	if(clicked && !active)
		sounds.Play(UiSound::Move);
	return clicked;
}

// ---------------------------------------------------------------------------
// Settings sections, shared by the Settings page and the in-game menu

struct CatInfo { const char *name; const char *desc; };
static const CatInfo kCats[App::Cat_Count] = {
	{ "Game", "What's running on the console, how long you've played it, and settings saved just for this game." },
	{ "Stream", "What the console sends: resolution, frame rate, codec, bitrate and HDR. Changes apply after reconnecting." },
	{ "Image", "Upscaling and detail. AI upscaling uses NVIDIA RTX Video Super Resolution or Intel VSR when your GPU has it." },
	{ "Motion", "Frame generation turns the console's 60 fps into your display's refresh rate." },
	{ "Picture", "Picture mode, HDR brightness, aspect ratio and ambient light in the side bars." },
	{ "Controls", "Keyboard and mouse, mouse feel, key bindings and controller features." },
	{ "Capture", "Recording, instant replay and screenshots." },
	{ "Interface", "Sounds, the start-up animation, battery warnings and per-game profiles." },
	{ "System", "Display, audio, connection behavior and hotkeys." },
	{ "Account", "The PSN account used to pair consoles." },
};

const char *App::CategoryName(int cat) { return kCats[std::clamp(cat, 0, Cat_Count - 1)].name; }

void App::DrawCategory(int cat, bool compact)
{
	SetRowLayout(compact);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 18 * ui_scale);
	if(g_heading)
		ImGui::PushFont(g_heading);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.88f, 0.92f, 1.0f));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18 * ui_scale);
	ImGui::TextUnformatted(kCats[cat].desc);
	ImGui::PopStyleColor();
	if(g_heading)
		ImGui::PopFont();
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, 14 * ui_scale));
	RowsBegin();
	switch(cat)
	{
		case Cat_Game: UiGameSection(); break;
		case Cat_Stream: UiStreamSection(); break;
		case Cat_Image: UiUpscaleSection(); break;
		case Cat_Motion: UiFrameGenSection(); break;
		case Cat_Picture: UiPictureSection(); break;
		case Cat_Controls: UiControlsSection(); break;
		case Cat_Capture: UiCaptureSection(); break;
		case Cat_Interface: UiInterfaceSection(); break;
		case Cat_System: UiSystemSection(); break;
		case Cat_Account:
		{
			Row("PSN Account ID");
			if(ImGui::InputText("##acc2", account_buf, sizeof(account_buf)))
			{
				settings.psn_account_id = account_buf;
				MarkSettingsDirty();
			}
			RowHelp("Filled in automatically when you pair a console and sign in with PSN.");
			Row("Get it from PSN");
			if(ImGui::Button("Sign in in the browser"))
				PsnLogin::OpenLoginPage();
			break;
		}
	}
	RowsEnd();
	ImGui::Dummy(ImVec2(0, 30 * ui_scale));
}

// ---------------------------------------------------------------------------
// Main window (not streaming)

static void TopRightStatus(App *app, ImDrawList *dl, float right, float y, float line_h, float s,
	const std::vector<Gamepads::PadInfo> &pads, const std::string &clock)
{
	ImFont *hf = g_heading ? g_heading : ImGui::GetFont();
	float rx = right;
	ImVec2 cs = hf->CalcTextSizeA(hf->FontSize, FLT_MAX, 0, clock.c_str());
	rx -= cs.x;
	dl->AddText(hf, hf->FontSize, ImVec2(rx, y + (line_h - cs.y) * 0.5f), IM_COL32(225, 228, 235, 255), clock.c_str());
	rx -= 26 * s;
	float bh = 12.0f * s;
	for(auto &pi : pads)
	{
		if(pi.battery < 0 && !pi.charging)
			continue;
		char pct[16];
		snprintf(pct, sizeof(pct), "%d%%", pi.battery);
		ImVec2 ps = ImGui::CalcTextSize(pct);
		rx -= ps.x;
		dl->AddText(ImVec2(rx, y + (line_h - ps.y) * 0.5f), IM_COL32(190, 195, 205, 255), pct);
		rx -= 6 * s + bh * 2.1f;
		DrawBattery(dl, ImVec2(rx, y + (line_h - bh) * 0.5f), bh, pi.battery, pi.charging);
		rx -= 22 * s;
	}
	(void)app;
}

void App::UiMain()
{
	DrawBackground();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->Pos);
	ImGui::SetNextWindowSize(vp->Size);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
		| ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground);
	ImGui::PopStyleVar();
	if(screen == Screen::Consoles)
		UiHome();
	else
		UiSettingsScreen();
	ImGui::End();
}

void App::UiSettingsScreen()
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	float W = vp->Size.x, H = vp->Size.y;
	float mx = std::clamp(W * 0.05f, 40.0f * ui_scale, 96.0f * ui_scale);
	float top = 44.0f * ui_scale;
	ImDrawList *dl = ImGui::GetWindowDrawList();

	ImFont *tf = g_title ? g_title : ImGui::GetFont();
	dl->AddText(tf, tf->FontSize, ImVec2(vp->Pos.x + mx, vp->Pos.y + top), IM_COL32(240, 242, 246, 255),
		screen == Screen::Log ? "Log" : "Settings");
	TopRightStatus(this, dl, vp->Pos.x + W - mx, vp->Pos.y + top, tf->FontSize, ui_scale, gamepads.Info(), ClockText());

	// Esc / Circle: back to the home screen (unless closing a dropdown or typing)
	bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
	if(!popup && !ImGui::GetIO().WantTextInput && kbm.CapturingAction() < 0
		&& (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
	{
		screen = Screen::Consoles;
		sounds.Play(UiSound::Back);
	}
	// L1 / R1 move between sections
	{
		int idx = screen == Screen::Log ? Cat_Count : settings_cat;
		int dir = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? -1 : 0;
		if(dir)
		{
			idx = std::clamp(idx + dir, (int)Cat_Stream, (int)Cat_Count);
			screen = idx == Cat_Count ? Screen::Log : Screen::Settings;
			if(screen == Screen::Settings)
				settings_cat = idx;
			sounds.Play(UiSound::Move);
		}
	}

	float list_w = std::clamp(W * 0.22f, 240.0f * ui_scale, 360.0f * ui_scale);
	float body_top = top + tf->FontSize + 40 * ui_scale;
	float gap = std::clamp(W * 0.04f, 32.0f * ui_scale, 80.0f * ui_scale);

	ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x + mx, vp->Pos.y + body_top));
	ImGui::BeginChild("##sections", ImVec2(list_w, H - body_top - 24 * ui_scale), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
	for(int c = Cat_Stream; c < Cat_Count; c++)
		if(ListItem(kCats[c].name, screen == Screen::Settings && settings_cat == c))
		{
			screen = Screen::Settings;
			settings_cat = c;
		}
	ImGui::Dummy(ImVec2(0, 10 * ui_scale));
	if(ListItem("Log", screen == Screen::Log))
		screen = Screen::Log;
	ImGui::Dummy(ImVec2(0, 10 * ui_scale));
	if(ListItem("Back to home", false))
	{
		screen = Screen::Consoles;
		sounds.Play(UiSound::Back);
	}
	ImGui::EndChild();

	float rx0 = vp->Pos.x + mx + list_w + gap;
	float rw = std::min(vp->Pos.x + W - mx - rx0, 1180.0f * ui_scale);
	ImGui::SetCursorScreenPos(ImVec2(rx0, vp->Pos.y + body_top));
	ImGui::BeginChild("##page", ImVec2(rw, H - body_top - 24 * ui_scale), ImGuiChildFlags_None, ImGuiWindowFlags_None);
	if(screen == Screen::Log)
		UiLog();
	else
		DrawCategory(settings_cat, false);
	ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Home: the selected console large on the left with its actions, a strip to switch consoles,
// and what the stream will look like along the bottom.

void App::UiHome()
{
	ImGuiViewport *vp = ImGui::GetMainViewport();
	float W = vp->Size.x, H = vp->Size.y, s = ui_scale;
	float mx = std::clamp(W * 0.06f, 48.0f * s, 128.0f * s);
	float top = 40.0f * s;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImFont *hf = g_heading ? g_heading : ImGui::GetFont();
	ImFont *df = g_display ? g_display : (g_title ? g_title : ImGui::GetFont());

	// ---- top bar: wordmark, then Settings, controllers and clock on the right
	{
		ImVec2 p(vp->Pos.x + mx, vp->Pos.y + top);
		float R = 9.0f * s;
		ImVec2 c(p.x + R, p.y + hf->FontSize * 0.5f + 1);
		dl->AddCircleFilled(c, R * 0.62f, IM_COL32(236, 238, 242, 255), 24);
		dl->AddEllipse(c, ImVec2(R * 1.35f, R * 0.42f), IM_COL32(236, 238, 242, 220), -0.35f, 40, 1.5f * s);
		dl->AddText(hf, hf->FontSize, ImVec2(p.x + R * 2 + 14 * s, p.y), IM_COL32(236, 238, 242, 255), "Orbit Play");

		const char *sl = g_icons ? ICON_SETTINGS "  Settings" : "Settings";
		float bw = ImGui::CalcTextSize(sl).x + 32 * s;
		float right = vp->Pos.x + W - mx;
		ImGui::SetCursorScreenPos(ImVec2(right - bw, p.y + (hf->FontSize - ImGui::GetFrameHeight()) * 0.5f));
		if(ImGui::Button(sl, ImVec2(bw, 0)))
		{
			screen = Screen::Settings;
			sounds.Play(UiSound::Select);
		}
		TopRightStatus(this, dl, right - bw - 28 * s, p.y, hf->FontSize, s, gamepads.Info(), ClockText());
	}
	// Options on the pad opens Settings
	if(ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
	{
		screen = Screen::Settings;
		sounds.Play(UiSound::Select);
	}

	// ---- the list of things you can select
	struct Entry { int kind; std::string title; RegisteredHost *host = nullptr; DiscoveredHost d; bool has_d = false; };
	std::vector<DiscoveredHost> found = discovery.Hosts();
	std::vector<Entry> entries;
	for(auto &h : settings.hosts)
	{
		Entry e;
		e.kind = 0;
		e.title = h.nickname.empty() ? "PlayStation 5" : h.nickname;
		e.host = &h;
		for(auto &d : found)
			if(d.mac == h.mac)
			{
				e.d = d;
				e.has_d = true;
			}
		entries.push_back(e);
	}
	for(auto &d : found)
		if(!settings.FindHost(d.mac))
		{
			Entry e;
			e.kind = 1;
			e.title = d.name.empty() ? "PlayStation" : d.name;
			e.d = d;
			e.has_d = true;
			entries.push_back(e);
		}
	{
		Entry e;
		e.kind = 2;
		e.title = "Add a console";
		entries.push_back(e);
	}
	home_sel = std::clamp(home_sel, 0, (int)entries.size() - 1);
	int dir = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? -1 : 0;
	if(dir && entries.size() > 1)
	{
		home_sel = (home_sel + dir + (int)entries.size()) % (int)entries.size();
		home_focus_pending = true;
		sounds.Play(UiSound::Move);
	}
	Entry &e = entries[home_sel];
	bool busy = phase != Phase::Idle;

	// ---- hero: the selected console
	float hy = vp->Pos.y + std::max(150.0f * s, H * 0.25f);
	float x0 = vp->Pos.x + mx;
	{
		std::string kicker = e.kind == 0 ? "PLAYSTATION 5" : e.kind == 1 ? "FOUND ON YOUR NETWORK" : "NEW CONSOLE";
		if(e.kind == 0 && !e.host->ps5)
			kicker = "PLAYSTATION 4";
		dl->AddText(ImVec2(x0 + 2 * s, hy), IM_COL32(150, 156, 168, 255), kicker.c_str());
		hy += ImGui::GetFontSize() + 8 * s;
		dl->PushClipRect(ImVec2(x0, hy), ImVec2(vp->Pos.x + W - mx, hy + df->FontSize + 10 * s), true);
		dl->AddText(df, df->FontSize, ImVec2(x0 - 3 * s, hy), IM_COL32(244, 245, 248, 255), e.title.c_str());
		dl->PopClipRect();
		hy += df->FontSize + 18 * s;
	}
	ImGui::SetCursorScreenPos(ImVec2(x0, hy));
	ImGui::BeginGroup();
	float bh = 54.0f * s;
	auto big_button = [&](const char *label, float w, bool primary) {
		if(hf)
			ImGui::PushFont(hf);
		bool r = primary ? PrimaryButton(label, ImVec2(w, bh)) : ImGui::Button(label, ImVec2(w, bh));
		if(hf)
			ImGui::PopFont();
		return r;
	};
	if(e.kind == 0)
	{
		RegisteredHost &h = *e.host;
		if(e.has_d)
			StatusPill(e.d.ready ? "Ready" : "Rest mode", e.d.ready ? kGood : kWarn);
		else
			StatusPill("Not found on the network", kTextDim);
		ImGui::SameLine(0, 16 * s);
		ImGui::TextDisabled("%s", e.has_d ? e.d.address.c_str() : (h.last_address.empty() ? "" : ("last seen at " + h.last_address).c_str()));
		std::string game = e.has_d && !e.d.running_app.empty() ? e.d.running_app : h.last_app;
		if(!game.empty())
		{
			std::string line = (e.has_d && !e.d.running_app.empty() ? "Playing " : "Last played ") + game;
			auto it = settings.playtime.find(SanitizeKey(game));
			if(it != settings.playtime.end() && it->second >= 60)
				line += "   \xC2\xB7   " + FormatDuration(it->second) + " played";
			if(hf)
				ImGui::PushFont(hf);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.82f, 0.87f, 1.0f));
			ImGui::TextUnformatted(line.c_str());
			ImGui::PopStyleColor();
			if(hf)
				ImGui::PopFont();
		}
		ImGui::Dummy(ImVec2(0, 14 * s));
		ImGui::BeginDisabled(busy);
		if(home_focus_pending)
		{
			ImGui::SetKeyboardFocusHere();
			home_focus_pending = false;
		}
		if(big_button(e.has_d && !e.d.ready ? "Wake and play" : "Play", 260 * s, true))
		{
			sounds.Play(UiSound::Select);
			BeginConnect(h.mac);
		}
		ImGui::SameLine(0, 10 * s);
		if(big_button("Wake", 130 * s, false))
		{
			Discovery::Wakeup(e.has_d ? e.d.address : h.last_address, h.regist_key, h.ps5);
			ShowToast("Wake-up sent to " + e.title);
		}
		ImGui::SameLine(0, 10 * s);
		if(big_button("Forget", 130 * s, false))
			ImGui::OpenPopup("##forget");
		ImGui::EndDisabled();
		if(ImGui::BeginPopup("##forget"))
		{
			ImGui::Text("Forget %s?", e.title.c_str());
			HelpText("You'll need to pair it again to play.");
			if(PrimaryButton("Forget", ImVec2(140 * s, 0)))
			{
				settings.RemoveHost(h.mac);
				MarkSettingsDirty();
				home_sel = 0;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if(ImGui::Button("Cancel", ImVec2(120 * s, 0)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
	else if(e.kind == 1)
	{
		StatusPill(e.d.ready ? "Not paired yet" : "Rest mode - turn it on to pair", e.d.ready ? kTextDim : kWarn);
		ImGui::SameLine(0, 16 * s);
		ImGui::TextDisabled("%s   \xC2\xB7   %s", e.d.address.c_str(), e.d.ps5 ? "PS5" : "PS4");
		ImGui::Dummy(ImVec2(0, 14 * s));
		ImGui::BeginDisabled(!e.d.ready || busy);
		if(home_focus_pending)
		{
			ImGui::SetKeyboardFocusHere();
			home_focus_pending = false;
		}
		if(big_button("Pair", 260 * s, true))
		{
			sounds.Play(UiSound::Select);
			pair_open = true;
			pair_address = e.d.address;
			pair_target = chiaki_target_is_unknown((ChiakiTarget)e.d.target) ? (e.d.ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10)
				: e.d.target;
			memset(pin_buf, 0, sizeof(pin_buf));
		}
		ImGui::EndDisabled();
	}
	else
	{
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(W - mx * 2, 760 * s));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.82f, 0.87f, 1.0f));
		if(hf)
			ImGui::PushFont(hf);
		ImGui::TextUnformatted(settings.hosts.empty() && found.empty()
			? "Looking for consoles on your network. If yours doesn't appear, enter its IP address "
			  "(PS5: Settings > Network > Connection Status)."
			: "Enter the IP address of a console on another subnet, or when your network blocks discovery.");
		if(hf)
			ImGui::PopFont();
		ImGui::PopStyleColor();
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, 14 * s));
		ImGui::SetNextItemWidth(320 * s);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16 * s, (bh - ImGui::GetFontSize()) * 0.5f));
		if(home_focus_pending)
		{
			ImGui::SetKeyboardFocusHere();
			home_focus_pending = false;
		}
		bool enter = ImGui::InputTextWithHint("##ip", "192.168.1.50", manual_ip_buf, sizeof(manual_ip_buf), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::PopStyleVar();
		ImGui::SameLine(0, 10 * s);
		if((big_button("Add", 130 * s, true) || enter) && manual_ip_buf[0])
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
			ShowToast("Looking for a console at " + ip + "...");
		}
		for(size_t i = 0; i < settings.manual_hosts.size(); i++)
		{
			ImGui::PushID((int)(1000 + i));
			if(i == 0)
				ImGui::TextDisabled("Checked by IP:");
			ImGui::SameLine();
			if(ImGui::SmallButton((settings.manual_hosts[i].address + "  x").c_str()))
			{
				settings.manual_hosts.erase(settings.manual_hosts.begin() + i);
				MarkSettingsDirty();
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
	}
	ImGui::EndGroup();

	// ---- console switcher
	float strip_y = vp->Pos.y + H - 132 * s;
	float sw_y = std::max(ImGui::GetItemRectMax().y + 40 * s, strip_y - 110 * s);
	{
		ImGui::SetCursorScreenPos(ImVec2(x0, sw_y));
		dl->AddText(ImVec2(x0 + 2 * s, sw_y), IM_COL32(150, 156, 168, 255), entries.size() > 1 ? "CONSOLES   (L1 / R1)" : "CONSOLES");
		float x = x0;
		float y = sw_y + ImGui::GetFontSize() + 12 * s;
		for(int i = 0; i < (int)entries.size(); i++)
		{
			const Entry &en = entries[i];
			std::string label = en.title + (en.kind == 1 ? "  (new)" : "");
			if(en.kind == 2)
				label = "+  Add a console";
			ImVec2 ts = hf->CalcTextSizeA(hf->FontSize, FLT_MAX, 0, label.c_str());
			ImVec2 p(x, y), q(x + ts.x + 44 * s, y + ts.y + 20 * s);
			ImGui::SetCursorScreenPos(p);
			ImGui::PushID(i);
			bool clicked = ImGui::InvisibleButton("##sw", ImVec2(q.x - p.x, q.y - p.y));
			bool hov = ImGui::IsItemHovered(), foc = ImGui::IsItemFocused() && GImGui->NavCursorVisible;
			ImGui::PopID();
			bool sel = i == home_sel;
			float r = 4.0f * s;
			if(sel)
			{
				dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 16), r);
				dl->AddRect(p, q, IM_COL32(235, 238, 245, 215), r, 0, 1.6f * s);
			}
			else if(hov || foc)
				dl->AddRectFilled(p, q, IM_COL32(255, 255, 255, 10), r);
			// status dot
			if(en.kind != 2)
			{
				ImU32 dc = !en.has_d ? IM_COL32(120, 124, 134, 255) : en.d.ready ? IM_COL32(110, 214, 143, 255) : IM_COL32(245, 184, 92, 255);
				dl->AddCircleFilled(ImVec2(p.x + 15 * s, p.y + (q.y - p.y) * 0.5f), 3.0f * s, dc, 12);
			}
			dl->AddText(hf, hf->FontSize, ImVec2(p.x + 28 * s, p.y + 10 * s), sel || hov ? IM_COL32(242, 244, 248, 255) : IM_COL32(190, 195, 205, 255),
				label.c_str());
			if(clicked && !sel)
			{
				home_sel = i;
				home_focus_pending = true;
				sounds.Play(UiSound::Move);
			}
			x = q.x + 12 * s;
		}
	}

	// ---- what the stream will look like (click to change)
	{
		dl->AddLine(ImVec2(x0, strip_y), ImVec2(vp->Pos.x + W - mx, strip_y), IM_COL32(255, 255, 255, 26), 1.0f);
		const char *ups[] = { "Off", "Enhanced", "AI super resolution" };
		const char *fgs[] = { "Off", "Predictive", "Interpolated" };
		char v1[96], v2[96], v3[96], v4[96];
		snprintf(v1, sizeof(v1), "%dp  \xC2\xB7  %d fps  \xC2\xB7  %s%s", settings.resolution, settings.fps,
			settings.codec == VideoCodec::H265 ? "HEVC" : "H.264", settings.hdr && renderer.DisplayHdr() ? "  \xC2\xB7  HDR" : "");
		if(settings.upscale_target > 0)
			snprintf(v2, sizeof(v2), "%s  to  %dp", ups[std::clamp(settings.upscale_mode, 0, 2)], settings.upscale_target);
		else
			snprintf(v2, sizeof(v2), "%s%s", ups[std::clamp(settings.upscale_mode, 0, 2)], settings.upscale_mode ? " to display size" : "");
		snprintf(v3, sizeof(v3), "%s", fgs[std::clamp(settings.frame_gen, 0, 2)]);
		snprintf(v4, sizeof(v4), "%s  \xC2\xB7  %d Hz", renderer.GpuVendorName().c_str(), renderer.DisplayHz());
		struct Col { const char *k; const char *v; int cat; };
		Col cols[] = { { "STREAM", v1, Cat_Stream }, { "UPSCALING", v2, Cat_Image }, { "FRAME GENERATION", v3, Cat_Motion },
			{ "THIS PC", v4, Cat_System } };
		float cw = (W - mx * 2) / 4.0f;
		for(int i = 0; i < 4; i++)
		{
			ImVec2 p(x0 + cw * i, strip_y + 18 * s);
			ImGui::SetCursorScreenPos(ImVec2(p.x - 10 * s, p.y - 8 * s));
			ImGui::PushID(100 + i);
			bool clicked = ImGui::InvisibleButton("##spec", ImVec2(cw - 12 * s, 76 * s));
			bool hov = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible);
			ImGui::PopID();
			if(hov)
				dl->AddRectFilled(ImVec2(p.x - 10 * s, p.y - 8 * s), ImVec2(p.x + cw - 22 * s, p.y + 68 * s), IM_COL32(255, 255, 255, 9), 4 * s);
			dl->AddText(p, IM_COL32(150, 156, 168, 255), cols[i].k);
			dl->PushClipRect(p, ImVec2(p.x + cw - 24 * s, p.y + 80 * s), true);
			dl->AddText(hf, hf->FontSize, ImVec2(p.x, p.y + ImGui::GetFontSize() + 8 * s), IM_COL32(232, 235, 240, 255), cols[i].v);
			dl->PopClipRect();
			if(clicked)
			{
				screen = Screen::Settings;
				settings_cat = cols[i].cat;
				sounds.Play(UiSound::Select);
			}
		}
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
	dl->AddRectFilled(a, b, IM_COL32(10, 11, 15, A(1.0f)));
	float R = vp->Size.y * 0.075f;
	c.y -= R * 0.6f;
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
		ImU32 rc = IM_COL32(236, 238, 242, A(0.95f));
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
	dl->AddRectFilled(a, b, IM_COL32(16, 18, 24, 242));
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
		dl->AddText(ImVec2(hx, cy - hs.y * 0.5f), IM_COL32(170, 180, 200, 255), hint);
	}
}

// ---------------------------------------------------------------------------
// Settings pieces

void App::UiInterfaceSection()
{
	bool changed = false;
	Row("Sounds");
	if(Toggle("Interface sounds", &settings.ui_sounds))
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
	changed |= Toggle("Play the intro animation", &settings.boot_animation);
	Row("Controllers");
	changed |= Toggle("Warn when a controller's battery is low", &settings.low_battery_warning);
	Row("Game profiles");
	changed |= Toggle("Load a game's saved settings when it starts", &settings.game_profiles);
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
	Row("Now playing");
	ImGui::TextUnformatted(current_game.c_str());
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
	if(Toggle("Load profiles automatically", &settings.game_profiles))
		MarkSettingsDirty();
}
