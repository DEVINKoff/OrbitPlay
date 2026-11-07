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
// Background: dark slate with a soft light falling in from the top-left corner. Static.

void App::DrawBackground()
{
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	ImVec2 a = vp->Pos, b(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
	float W = vp->Size.x, H = vp->Size.y;
	dl->AddRectFilledMultiColor(a, b, IM_COL32(30, 31, 37, 255), IM_COL32(17, 19, 26, 255), IM_COL32(9, 10, 15, 255),
		IM_COL32(20, 21, 27, 255));
	// the light: many large, very faint discs make a smooth falloff
	ImVec2 c(a.x + W * 0.16f, a.y - H * 0.28f);
	for(int i = 28; i >= 1; i--)
		dl->AddCircleFilled(c, H * 0.045f * i, IM_COL32(205, 192, 180, 3), 96);
	// a slightly brighter core where the beam enters
	ImVec2 c2(a.x + W * 0.12f, a.y - H * 0.05f);
	for(int i = 8; i >= 1; i--)
		dl->AddCircleFilled(c2, H * 0.05f * i, IM_COL32(220, 205, 190, 3), 64);
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
// Main window (not streaming): title, section list on the left, page on the right

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
	float W = vp->Size.x, H = vp->Size.y;
	float mx = std::clamp(W * 0.05f, 40.0f * ui_scale, 96.0f * ui_scale);
	float top = 44.0f * ui_scale;
	ImDrawList *dl = ImGui::GetWindowDrawList();

	// title
	const char *title = screen == Screen::Consoles ? "Consoles" : screen == Screen::Log ? "Log" : "Settings";
	ImFont *tf = g_title ? g_title : ImGui::GetFont();
	dl->AddText(tf, tf->FontSize, ImVec2(vp->Pos.x + mx, vp->Pos.y + top), IM_COL32(240, 242, 246, 255), title);

	// top right: controllers + clock
	{
		ImFont *hf = g_heading ? g_heading : ImGui::GetFont();
		float rx = vp->Pos.x + W - mx, y = vp->Pos.y + top + (tf->FontSize - hf->FontSize) * 0.5f;
		std::string clock = ClockText();
		ImVec2 cs = hf->CalcTextSizeA(hf->FontSize, FLT_MAX, 0, clock.c_str());
		rx -= cs.x;
		dl->AddText(hf, hf->FontSize, ImVec2(rx, y), IM_COL32(225, 228, 235, 255), clock.c_str());
		rx -= 28 * ui_scale;
		float bh = 12.0f * ui_scale;
		for(auto &pi : gamepads.Info())
		{
			if(pi.battery < 0 && !pi.charging)
				continue;
			char pct[16];
			snprintf(pct, sizeof(pct), "%d%%", pi.battery);
			ImVec2 ps = ImGui::CalcTextSize(pct);
			rx -= ps.x;
			dl->AddText(ImVec2(rx, y + (cs.y - ps.y) * 0.5f), IM_COL32(190, 195, 205, 255), pct);
			rx -= 6 * ui_scale + bh * 2.1f;
			DrawBattery(dl, ImVec2(rx, y + (cs.y - bh) * 0.5f), bh, pi.battery, pi.charging);
			rx -= 22 * ui_scale;
		}
	}

	// gamepad: L1 / R1 move between sections
	{
		int idx = screen == Screen::Consoles ? 0 : screen == Screen::Log ? Cat_Count : settings_cat; // Game slot (0) = Consoles here
		int dir = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? -1 : 0;
		if(dir)
		{
			idx = std::clamp(idx + dir, 0, (int)Cat_Count);
			screen = idx == 0 ? Screen::Consoles : idx == Cat_Count ? Screen::Log : Screen::Settings;
			if(screen == Screen::Settings)
				settings_cat = idx;
			sounds.Play(UiSound::Move);
		}
	}

	float list_w = std::clamp(W * 0.22f, 240.0f * ui_scale, 360.0f * ui_scale);
	float body_top = top + tf->FontSize + 40 * ui_scale;
	float gap = std::clamp(W * 0.04f, 32.0f * ui_scale, 80.0f * ui_scale);

	// left column
	ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x + mx, vp->Pos.y + body_top));
	ImGui::BeginChild("##sections", ImVec2(list_w, H - body_top - 24 * ui_scale), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
	if(ListItem("Consoles", screen == Screen::Consoles))
		screen = Screen::Consoles;
	ImGui::Dummy(ImVec2(0, 10 * ui_scale));
	for(int c = Cat_Stream; c < Cat_Count; c++)
		if(ListItem(kCats[c].name, screen == Screen::Settings && settings_cat == c))
		{
			screen = Screen::Settings;
			settings_cat = c;
		}
	ImGui::Dummy(ImVec2(0, 10 * ui_scale));
	if(ListItem("Log", screen == Screen::Log))
		screen = Screen::Log;
	ImGui::EndChild();

	// right column
	float rx0 = vp->Pos.x + mx + list_w + gap;
	float rw = std::min(vp->Pos.x + W - mx - rx0, 1180.0f * ui_scale);
	ImGui::SetCursorScreenPos(ImVec2(rx0, vp->Pos.y + body_top));
	ImGui::BeginChild("##page", ImVec2(rw, H - body_top - 24 * ui_scale), ImGuiChildFlags_None, ImGuiWindowFlags_None);
	if(screen == Screen::Consoles)
		UiConsolesPage();
	else if(screen == Screen::Log)
		UiLog();
	else
		DrawCategory(settings_cat, false);
	ImGui::EndChild();
	ImGui::End();
}

// ---------------------------------------------------------------------------
// Consoles: one row per console, details under the name, actions on the right

void App::UiConsolesPage()
{
	std::vector<DiscoveredHost> found = discovery.Hosts();
	auto discovered = [&](const std::string &mac, DiscoveredHost *out) {
		for(auto &d : found)
			if(d.mac == mac)
			{
				*out = d;
				return true;
			}
		return false;
	};
	ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 18 * ui_scale);
	if(g_heading)
		ImGui::PushFont(g_heading);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.88f, 0.92f, 1.0f));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18 * ui_scale);
	ImGui::TextUnformatted(settings.hosts.empty()
		? "Pair your PS5 once, then play from here. Remote Play must be on: PS5 Settings > System > Remote Play."
		: "Select a console to play. Consoles in rest mode wake up automatically.");
	ImGui::PopStyleColor();
	if(g_heading)
		ImGui::PopFont();
	ImGui::PopTextWrapPos();
	ImGui::Dummy(ImVec2(0, 14 * ui_scale));

	float padx = 18 * ui_scale;
	bool busy = phase != Phase::Idle;
	float bh = ImGui::GetFrameHeight();
	auto right_buttons_x = [&](float total_w) { return ImGui::GetWindowContentRegionMax().x - padx - total_w; };
	auto sub_line = [&](const std::string &s) {
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padx);
		ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
		ImGui::TextUnformatted(s.c_str());
		ImGui::PopStyleColor();
	};

	RowsBegin();
	std::string forget;
	for(size_t i = 0; i < settings.hosts.size(); i++)
	{
		RegisteredHost &h = settings.hosts[i];
		DiscoveredHost d;
		bool has_d = discovered(h.mac, &d);
		ImGui::PushID((int)i);
		std::string name = h.nickname.empty() ? "PlayStation 5" : h.nickname;
		Row("");
		// name in the larger font, on the left
		ImGui::SameLine(18 * ui_scale);
		if(g_heading)
			ImGui::PushFont(g_heading);
		ImGui::TextUnformatted(name.c_str());
		if(g_heading)
			ImGui::PopFont();
		// actions on the right
		const char *play = has_d && !d.ready ? "Wake & play" : "Play";
		float pw = std::max(150 * ui_scale, ImGui::CalcTextSize(play).x + 40 * ui_scale), ow = 110 * ui_scale;
		ImGui::SameLine(right_buttons_x(pw + ow * 2 + 16 * ui_scale));
		ImGui::BeginDisabled(busy);
		if(home_focus_pending && i == 0)
		{
			ImGui::SetKeyboardFocusHere();
			home_focus_pending = false;
		}
		if(PrimaryButton(play, ImVec2(pw, bh)))
		{
			sounds.Play(UiSound::Select);
			BeginConnect(h.mac);
		}
		ImGui::SameLine(0, 8 * ui_scale);
		if(ImGui::Button("Wake", ImVec2(ow, bh)))
		{
			Discovery::Wakeup(has_d ? d.address : h.last_address, h.regist_key, h.ps5);
			ShowToast("Wake-up sent to " + name);
		}
		ImGui::SameLine(0, 8 * ui_scale);
		if(ImGui::Button("Forget", ImVec2(ow, bh)))
			ImGui::OpenPopup("##forget");
		ImGui::EndDisabled();
		if(ImGui::BeginPopup("##forget"))
		{
			ImGui::Text("Forget %s?", name.c_str());
			HelpText("You'll need to pair it again to play.");
			if(PrimaryButton("Forget", ImVec2(140 * ui_scale, 0)))
			{
				forget = h.mac;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if(ImGui::Button("Cancel", ImVec2(120 * ui_scale, 0)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		// status and details underneath
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padx);
		if(has_d)
			StatusPill(d.ready ? "Ready" : "Rest mode", d.ready ? kGood : kWarn);
		else
			StatusPill("Not found on the network", kTextDim);
		std::string details = has_d ? d.address : (h.last_address.empty() ? "" : "Last seen at " + h.last_address);
		std::string game = has_d && !d.running_app.empty() ? d.running_app : h.last_app;
		if(!game.empty())
		{
			details += (details.empty() ? "" : "   \xC2\xB7   ") + std::string(has_d && !d.running_app.empty() ? "Playing " : "Last played ") + game;
			auto it = settings.playtime.find(SanitizeKey(game));
			if(it != settings.playtime.end() && it->second >= 60)
				details += " (" + FormatDuration(it->second) + ")";
		}
		if(!details.empty())
		{
			ImGui::SameLine(0, 18 * ui_scale);
			ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
			ImGui::TextUnformatted(details.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::PopID();
	}
	if(!forget.empty())
	{
		settings.RemoveHost(forget);
		MarkSettingsDirty();
	}

	// consoles on the network that aren't paired yet
	bool any_new = false;
	for(auto &d : found)
	{
		if(settings.FindHost(d.mac))
			continue;
		if(!any_new)
		{
			SubHeader("Found on your network");
			any_new = true;
		}
		ImGui::PushID(d.mac.c_str());
		Row("");
		ImGui::SameLine(18 * ui_scale);
		if(g_heading)
			ImGui::PushFont(g_heading);
		ImGui::TextUnformatted(d.name.empty() ? "PlayStation" : d.name.c_str());
		if(g_heading)
			ImGui::PopFont();
		float pw = 150 * ui_scale;
		ImGui::SameLine(right_buttons_x(pw));
		ImGui::BeginDisabled(!d.ready || busy);
		if(PrimaryButton("Pair", ImVec2(pw, bh)))
		{
			sounds.Play(UiSound::Select);
			pair_open = true;
			pair_address = d.address;
			pair_target = chiaki_target_is_unknown((ChiakiTarget)d.target) ? (d.ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10) : d.target;
			memset(pin_buf, 0, sizeof(pin_buf));
		}
		ImGui::EndDisabled();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padx);
		StatusPill(d.ready ? "Not paired" : "Rest mode - turn it on to pair", d.ready ? kTextDim : kWarn);
		ImGui::SameLine(0, 18 * ui_scale);
		ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
		ImGui::Text("%s   \xC2\xB7   %s", d.address.c_str(), d.ps5 ? "PS5" : "PS4");
		ImGui::PopStyleColor();
		ImGui::PopID();
	}
	if(settings.hosts.empty() && !any_new)
	{
		Row("Searching your network...");
		RowHelp("If your PS5 doesn't show up, add it by its IP address below "
			"(PS5: Settings > Network > Connection Status > View Connection Status).");
	}

	// add by IP
	SubHeader("Add by IP address");
	Row("IP address");
	ImGui::SetNextItemWidth(std::max(160 * ui_scale, ImGui::GetContentRegionAvail().x - 120 * ui_scale - padx));
	bool enter = ImGui::InputTextWithHint("##ip", "192.168.1.50", manual_ip_buf, sizeof(manual_ip_buf), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::SameLine(0, 8 * ui_scale);
	if((ImGui::Button("Add", ImVec2(110 * ui_scale, 0)) || enter) && manual_ip_buf[0])
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
	RowHelp("For a console on another subnet, or when your network blocks discovery.");
	for(size_t i = 0; i < settings.manual_hosts.size(); i++)
	{
		ImGui::PushID((int)(1000 + i));
		Row(settings.manual_hosts[i].address.c_str());
		if(ImGui::Button("Remove"))
		{
			settings.manual_hosts.erase(settings.manual_hosts.begin() + i);
			MarkSettingsDirty();
			ImGui::PopID();
			break;
		}
		ImGui::PopID();
	}
	RowsEnd();

	// this PC, in small print at the end
	ImGui::Dummy(ImVec2(0, 24 * ui_scale));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padx);
	ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
	ImGui::Text("This PC: %s%s   \xC2\xB7   %d Hz %s display   \xC2\xB7   %s", renderer.GpuVendorName().c_str(),
		renderer.AiSuperResolutionAvailable() ? " (AI upscaling)" : "", renderer.DisplayHz(), renderer.DisplayHdr() ? "HDR" : "SDR",
		gamepads.Count() ? (std::to_string(gamepads.Count()) + " controller(s)").c_str() : "no controller");
	ImGui::PopStyleColor();
	(void)sub_line;
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
