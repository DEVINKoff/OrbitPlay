// SPDX-License-Identifier: AGPL-3.0-only
// Shared look: fonts, colors and small widgets used by every screen.
#pragma once

#include "icons.h"
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

inline bool g_icons = false;
inline ImFont *g_heading = nullptr, *g_title = nullptr, *g_hero = nullptr, *g_display = nullptr;
inline const char *Ico(const char *icon) { return g_icons ? icon : ""; }

// Colors: flat, neutral surfaces; white is the selection color; one muted accent for controls
inline const ImVec4 kAccent(0.36f, 0.56f, 0.98f, 1.0f);
inline const ImVec4 kAccentHi(0.50f, 0.67f, 1.00f, 1.0f);
inline const ImU32 kAccentU = IM_COL32(92, 143, 250, 255);
inline const ImVec4 kTextDim(0.56f, 0.59f, 0.65f, 1.0f);
inline const ImVec4 kGood(0.42f, 0.84f, 0.56f, 1.0f);
inline const ImVec4 kWarn(0.96f, 0.72f, 0.36f, 1.0f);
inline const ImU32 kBgU = IM_COL32(12, 14, 19, 255);
inline const ImU32 kNavU = IM_COL32(16, 19, 26, 255);
inline const ImU32 kSurfaceU = IM_COL32(255, 255, 255, 10);
inline const ImU32 kSurfaceHiU = IM_COL32(255, 255, 255, 20);
inline const ImU32 kSelectU = IM_COL32(236, 238, 242, 255);
inline const ImU32 kOnSelectU = IM_COL32(12, 14, 19, 255);

inline void SectionTitle(const char *text)
{
	ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
	if(g_heading)
		ImGui::PushFont(g_heading);
	ImGui::TextUnformatted(text);
	if(g_heading)
		ImGui::PopFont();
	ImGui::Dummy(ImVec2(0, 2));
}

// The main action: light button with dark text
inline bool PrimaryButton(const char *label, const ImVec2 &size = ImVec2(0, 0))
{
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.92f, 0.93f, 0.95f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.78f, 0.80f, 0.84f, 1.0f));
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.055f, 0.075f, 1.0f));
	bool r = ImGui::Button(label, size);
	ImGui::PopStyleColor(4);
	return r;
}

inline void HelpText(const char *text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
	ImGui::PushTextWrapPos(0.0f);
	ImGui::TextUnformatted(text);
	ImGui::PopTextWrapPos();
	ImGui::PopStyleColor();
}

// Status: a small dot and colored text
inline void StatusPill(const char *text, const ImVec4 &col)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 ts = ImGui::CalcTextSize(text);
	float r = 3.5f * ImGui::GetFontSize() / 18.0f;
	dl->AddCircleFilled(ImVec2(p.x + r, p.y + ts.y * 0.5f + 1), r, ImGui::GetColorU32(col));
	dl->AddText(ImVec2(p.x + r * 2 + 8, p.y), ImGui::GetColorU32(col), text);
	ImGui::Dummy(ImVec2(ts.x + r * 2 + 8, ts.y));
}

// On/off switch (instead of a checkbox). The label (text before "##") sits at the cursor, the
// switch at the right edge of the available width; the whole width is clickable.
inline bool Toggle(const char *label, bool *v)
{
	ImGuiStorage *st = ImGui::GetStateStorage();
	ImGuiID id = ImGui::GetID(label);
	float fs = ImGui::GetFontSize();
	float h = fs * 1.15f, w = h * 1.9f, pad = fs;
	ImVec2 p = ImGui::GetCursorScreenPos();
	float avail = std::max(w + pad, ImGui::GetContentRegionAvail().x - pad);
	float fh = ImGui::GetFrameHeight();
	// the description wraps in the space left of the switch
	const char *end = ImGui::FindRenderedTextEnd(label);
	float wrap = std::max(10.0f, avail - w - pad);
	ImVec2 ts = end != label ? ImGui::CalcTextSize(label, end, false, wrap) : ImVec2(0, 0);
	float bh = std::max(fh, ts.y + (fh - fs));
	bool clicked = ImGui::InvisibleButton(label, ImVec2(avail, bh));
	if(clicked)
		*v = !*v;
	bool hov = ImGui::IsItemHovered();
	float *anim = st->GetFloatRef(id, *v ? 1.0f : 0.0f);
	float target = *v ? 1.0f : 0.0f;
	*anim += (target - *anim) * std::min(1.0f, ImGui::GetIO().DeltaTime * 16.0f);
	float t = *anim;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	if(end != label)
		dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x, p.y + (fh - fs) * 0.5f), ImGui::GetColorU32(kTextDim), label, end, wrap);
	ImVec2 a(p.x + avail - w, p.y + (fh - h) * 0.5f), b(a.x + w, a.y + h);
	auto mixc = [](int a0, int a1, float k) { return (int)(a0 + (a1 - a0) * k); };
	int alpha = (int)(255 * ImGui::GetStyle().Alpha);
	dl->AddRectFilled(a, b, IM_COL32(mixc(62, 150, t), mixc(65, 154, t), mixc(74, 164, t), alpha), h * 0.5f);
	if(hov)
		dl->AddRect(a, b, IM_COL32(255, 255, 255, 60), h * 0.5f, 0, 1.0f);
	float r = h * 0.5f - 3.0f;
	ImVec2 c(a.x + h * 0.5f + (w - h) * t, a.y + h * 0.5f);
	dl->AddCircleFilled(c, r, IM_COL32(mixc(160, 250, t), mixc(163, 251, t), mixc(172, 253, t), alpha), 24);
	return clicked;
}
