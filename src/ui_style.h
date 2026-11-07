// SPDX-License-Identifier: AGPL-3.0-only
// Shared look: fonts, colors and small widgets used by every screen.
#pragma once

#include "icons.h"
#include <imgui.h>

inline bool g_icons = false;
inline ImFont *g_heading = nullptr, *g_title = nullptr, *g_hero = nullptr;
inline const char *Ico(const char *icon) { return g_icons ? icon : ""; }

// Colors
inline const ImVec4 kAccent(0.00f, 0.44f, 0.86f, 1.0f);       // PlayStation blue
inline const ImVec4 kAccentHi(0.20f, 0.58f, 1.00f, 1.0f);
inline const ImU32 kAccentU = IM_COL32(0, 112, 219, 255);
inline const ImVec4 kTextDim(0.62f, 0.66f, 0.74f, 1.0f);
inline const ImVec4 kGood(0.35f, 0.88f, 0.55f, 1.0f);
inline const ImVec4 kWarn(1.00f, 0.74f, 0.30f, 1.0f);

inline void SectionTitle(const char *text)
{
	ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
	if(g_heading)
		ImGui::PushFont(g_heading);
	ImGui::TextUnformatted(text);
	if(g_heading)
		ImGui::PopFont();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = ImGui::GetContentRegionAvail().x;
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + 42.0f * ImGui::GetFontSize() / 19.0f, p.y + 3), kAccentU, 2.0f);
	dl->AddRectFilled(ImVec2(p.x, p.y + 1), ImVec2(p.x + w, p.y + 2), IM_COL32(255, 255, 255, 18));
	ImGui::Dummy(ImVec2(0, 8));
}

inline bool PrimaryButton(const char *label, const ImVec2 &size = ImVec2(0, 0))
{
	ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHi);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.36f, 0.72f, 1.0f));
	bool r = ImGui::Button(label, size);
	ImGui::PopStyleColor(3);
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

// Small rounded status pill: colored dot + text
inline void StatusPill(const char *text, const ImVec4 &col)
{
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImVec2 ts = ImGui::CalcTextSize(text);
	float h = ts.y + 8, r = 4.5f * ImGui::GetFontSize() / 19.0f;
	float w = ts.x + r * 2 + 26;
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, 0.14f)), h * 0.5f);
	dl->AddCircleFilled(ImVec2(p.x + 11 + r, p.y + h * 0.5f), r, ImGui::GetColorU32(col));
	dl->AddText(ImVec2(p.x + 17 + r * 2, p.y + 4), ImGui::GetColorU32(col), text);
	ImGui::Dummy(ImVec2(w, h));
}

