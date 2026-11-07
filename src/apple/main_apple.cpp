// Orbit Play for iPad - PS5 Remote Play
// SPDX-License-Identifier: AGPL-3.0-only
#include "../app.h"

#include <SDL3/SDL_main.h>

char g_crash_context[256] = "";

int main(int, char **)
{
	SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
	App app;
	int rc = 0;
	if(app.Init())
		app.Run();
	else
		rc = 1;
	app.Shutdown();
	return rc;
}
