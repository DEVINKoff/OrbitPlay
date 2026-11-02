// Orbit Play - PS5 Remote Play client for Windows 11
// Built on chiaki-ng's Remote Play protocol library.
// SPDX-License-Identifier: AGPL-3.0-only

#define SDL_MAIN_HANDLED
#include "app.h"
#include <SDL3/SDL_main.h>

#include <windows.h>
#include <timeapi.h>

int main(int, char **)
{
	SDL_SetMainReady();
	// Per-monitor DPI awareness, 1 ms timer resolution for smooth frame pacing,
	// and keep the display awake while streaming.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	timeBeginPeriod(1);
	SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

	int rc = 0;
	{
		App app;
		if(app.Init())
			app.Run();
		else
		{
			MessageBoxA(nullptr, "Orbit Play could not start. See %APPDATA%\\OrbitPlay\\orbitplay.log for details.",
				"Orbit Play", MB_ICONERROR);
			rc = 1;
		}
		app.Shutdown();
	}

	SetThreadExecutionState(ES_CONTINUOUS);
	timeEndPeriod(1);
	return rc;
}

#ifdef _MSC_VER
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(0, nullptr); }
#endif
