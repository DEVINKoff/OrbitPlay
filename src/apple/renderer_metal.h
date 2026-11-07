// SPDX-License-Identifier: AGPL-3.0-only
// Metal renderer for iPadOS: VideoToolbox frames in (zero copy), MetalFX upscaling, Dear ImGui on
// top. Same public interface as the Direct3D 11 renderer, so the app code is shared.
#pragma once

#include "../renderer_types.h"

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <string>

struct AVFrame;
struct SDL_Window;

class Renderer
{
public:
	Renderer();
	~Renderer();

	bool Init(SDL_Window *window);
	void Shutdown();
	void Resize(int width, int height);

	// VideoToolbox needs no device from the renderer
	void *DecoderDevice() const { return nullptr; }
	void *DecoderContext() const { return nullptr; }

	bool ImGuiInit();
	void ImGuiShutdown();
	void ImGuiNewFrame();
	void ImGuiRender() {}          // drawn inside Present, on top of the video
	void ImGuiInvalidate() {}
	ImTextureID CreateImage(int w, int h, const uint32_t *rgba);
	void DestroyImage(ImTextureID id);

	void RefreshDisplayInfo();
	bool DisplayHdr() const { return false; }   // HDR streams are tone-mapped for now
	float DisplayPeakNits() const { return 500.0f; }
	int DisplayHz() const;
	bool OutputHdr() const { return false; }

	SubmitResult SubmitFrame(AVFrame *frame, const ProcessOptions &options);
	int PollFrameDiff(uint64_t, bool *duplicate, bool *cut) { *duplicate = *cut = false; return -1; }
	void DiscardFrame(uint64_t) {}
	void ResetVideo();
	bool HasVideo() const;
	bool VideoHdr() const;
	int VideoWidth() const;
	int VideoHeight() const;
	int OutputVideoWidth() const;
	int OutputVideoHeight() const;
	const std::string &UpscalerStatus() const;
	bool AiSuperResolutionAvailable() const;
	bool AiHdrAvailable() const { return false; }
	std::string GpuVendorName() const;

	// Frame generation is not available on iPad yet
	bool BuildInterpolated(uint64_t, uint64_t, float) { return false; }
	bool HasFrame(uint64_t serial) const;

	void BeginOverlay() {}
	void Present(VideoView view, const PresentOptions &options, uint64_t serial = 0);
	bool DeviceLost() const { return false; }

	// Where the video is drawn, in interface units (points)
	void GetVideoRect(AspectMode aspect, float rect[4]) const;
	bool SaveScreenshot(const std::string &path_utf8, const PresentOptions &options);

private:
	struct Impl;
	std::unique_ptr<Impl> d;
};
