// SPDX-License-Identifier: AGPL-3.0-only
// iPad pieces of the App: the main loop, and stand-ins for the Windows-only window code.
#include "../app.h"
#include "../log.h"

extern "C" {
#include <libavutil/frame.h>
}

// One iteration per display refresh: Present waits for the next drawable (60 or 120 Hz).
void App::RunApple()
{
	while(!quit)
	{
		UpdateMouseCapture();
		PumpEvents();
		if(in_background)
		{
			SDL_Delay(50); // no drawing while in the background (iPadOS doesn't allow it)
			continue;
		}
		Tick();
		HandleSessionEvents();
		SendInput();
		bool new_frame = false;
		if(phase == Phase::Streaming && session)
		{
			if(AVFrame *frame = session->PullFrame())
			{
				new_frame = renderer.SubmitFrame(frame, MakeProcessOptions()).serial != 0;
				av_frame_free(&frame);
			}
		}
		PresentFrame(new_frame);
	}
}

// The window is always full screen on iPad: no title bar, no placement, no mini player
void App::InstallChrome() {}
void App::DrawWindowControls() { caption_h = 0.0f; }
void App::SaveWindowPlacement() {}
void App::RestoreWindowPlacement() {}
void App::RememberWindowRect() {}
float App::MiniAspect() const { return 16.0f / 9.0f; }
bool App::WindowFullscreen() const { return true; }
