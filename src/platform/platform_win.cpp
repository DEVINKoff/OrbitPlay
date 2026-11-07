// SPDX-License-Identifier: AGPL-3.0-only
#include "platform.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>

namespace platform
{
static std::wstring Widen(const std::string &s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if(n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
	return w;
}

static std::string Narrow(const std::wstring &w)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if(n > 0)
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
	return s;
}

std::string DataDir()
{
	wchar_t appdata[MAX_PATH] = {};
	DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
	std::wstring dir = (n ? std::wstring(appdata) : std::wstring(L".")) + L"\\OrbitPlay";
	CreateDirectoryW(dir.c_str(), nullptr);
	return Narrow(dir);
}

std::string CaptureDir(bool video)
{
	wchar_t profile[MAX_PATH] = {};
	DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
	std::wstring base = (n ? std::wstring(profile) : std::wstring(L".")) + (video ? L"\\Videos" : L"\\Pictures");
	CreateDirectoryW(base.c_str(), nullptr);
	std::wstring dir = base + L"\\Orbit Play";
	CreateDirectoryW(dir.c_str(), nullptr);
	return Narrow(dir);
}

std::string TimestampName(const char *prefix, const char *ext)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char b[128];
	snprintf(b, sizeof(b), "%s_%04d%02d%02d_%02d%02d%02d.%s", prefix, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ext);
	return b;
}

std::string JoinPath(const std::string &dir, const std::string &name) { return dir + "\\" + name; }

std::string ClockText()
{
	wchar_t w[32] = {};
	if(!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, nullptr, nullptr, w, 32))
		return "";
	return Narrow(w);
}

void OpenFolder(const std::string &path)
{
	ShellExecuteW(nullptr, L"open", Widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool FileExists(const std::string &path)
{
	return GetFileAttributesW(Widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

void RemoveFile(const std::string &path) { DeleteFileW(Widen(path).c_str()); }

// IBM Plex Sans is compiled into the exe as resources 101..104 (res/orbitplay.rc)
bool FontData(int weight, const void **data, int *size)
{
	HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(100 + weight), (LPCWSTR)RT_RCDATA);
	HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
	*data = g ? LockResource(g) : nullptr;
	*size = r ? (int)SizeofResource(nullptr, r) : 0;
	return *data && *size > 0;
}

std::string IconFontPath()
{
	const char *f11 = "C:\\Windows\\Fonts\\SegoeIcons.ttf", *f10 = "C:\\Windows\\Fonts\\segmdl2.ttf";
	if(GetFileAttributesA(f11) != INVALID_FILE_ATTRIBUTES)
		return f11;
	if(GetFileAttributesA(f10) != INVALID_FILE_ATTRIBUTES)
		return f10;
	return "";
}

std::vector<std::string> LocalSubnetHosts() { return {}; } // broadcasts work on Windows

const char *Name() { return "Windows"; }
}
