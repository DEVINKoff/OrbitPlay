// Orbit Play - PS5 Remote Play client for Windows 11
// Built on chiaki-ng's Remote Play protocol library.
// SPDX-License-Identifier: AGPL-3.0-only

#define SDL_MAIN_HANDLED
#include "app.h"
#include <SDL3/SDL_main.h>

#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

char g_crash_context[256] = "starting";

// ---------------------------------------------------------------------------
// Crash reports: a minidump plus a note of what was running, picked up on the next start.

static wchar_t g_crash_dir[MAX_PATH] = {};

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *info)
{
	static volatile LONG once = 0;
	if(InterlockedExchange(&once, 1))
		return EXCEPTION_EXECUTE_HANDLER;
	std::wstring dir = g_crash_dir;
	HANDLE f = CreateFileW((dir + L"\\crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(f != INVALID_HANDLE_VALUE)
	{
		MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), info, FALSE };
		MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
			(MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory), &mei, nullptr, nullptr);
		CloseHandle(f);
	}
	HANDLE m = CreateFileW((dir + L"\\crash.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(m != INVALID_HANDLE_VALUE)
	{
		char buf[400];
		int n = snprintf(buf, sizeof(buf), "code=0x%08lx %s\n", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0UL,
			g_crash_context);
		DWORD written = 0;
		WriteFile(m, buf, (DWORD)std::max(0, n), &written, nullptr);
		CloseHandle(m);
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

static void InstallCrashHandler()
{
	wchar_t appdata[MAX_PATH] = {};
	DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
	swprintf(g_crash_dir, MAX_PATH, L"%ls\\OrbitPlay", n ? appdata : L".");
	CreateDirectoryW(g_crash_dir, nullptr);
	SetUnhandledExceptionFilter(CrashFilter);
}

// ---------------------------------------------------------------------------

static bool Relaunch(const std::string &resume_mac)
{
	wchar_t exe[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --relaunch";
	if(!resume_mac.empty())
		cmd += L" --resume " + std::wstring(resume_mac.begin(), resume_mac.end());
	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {};
	if(!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
		return false;
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return true;
}

int main(int argc, char **argv)
{
	SDL_SetMainReady();
	InstallCrashHandler();

	bool relaunched = false;
	std::string resume_mac;
	for(int i = 1; i < argc; i++)
	{
		if(!strcmp(argv[i], "--relaunch"))
			relaunched = true;
		else if(!strcmp(argv[i], "--resume") && i + 1 < argc)
			resume_mac = argv[++i];
	}

	// One instance at a time: a second start brings the running window to the front.
	HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\OrbitPlay.SingleInstance");
	if(instance && GetLastError() == ERROR_ALREADY_EXISTS)
	{
		if(!relaunched)
		{
			if(HWND other = FindWindowW(nullptr, L"Orbit Play"))
			{
				ShowWindow(other, SW_RESTORE);
				SetForegroundWindow(other);
			}
			return 0;
		}
	}
	if(instance)
		WaitForSingleObject(instance, relaunched ? 15000 : 0); // after a restart: wait for the old process to exit

	// Per-monitor DPI awareness, 1 ms timer resolution for smooth frame pacing,
	// and keep the display awake while streaming.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	timeBeginPeriod(1);
	SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

	int rc = 0;
	bool relaunch = false;
	std::string relaunch_mac;
	{
		App app;
		if(app.Init(resume_mac))
			app.Run();
		else
		{
			MessageBoxA(nullptr, "Orbit Play could not start. See %APPDATA%\\OrbitPlay\\orbitplay.log for details.",
				"Orbit Play", MB_ICONERROR);
			rc = 1;
		}
		relaunch = app.RelaunchRequested();
		relaunch_mac = app.RelaunchResumeMac();
		app.Shutdown();
	}

	SetThreadExecutionState(ES_CONTINUOUS);
	timeEndPeriod(1);
	if(instance)
	{
		ReleaseMutex(instance);
		CloseHandle(instance);
	}
	if(relaunch && !relaunched)
		Relaunch(relaunch_mac); // only once: if the GPU keeps failing, don't loop
	return rc;
}

#ifdef _MSC_VER
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
