// SPDX-License-Identifier: AGPL-3.0-only
// NVIDIA hardware optical flow (the NVOFA unit in RTX 20/30/40/50 GPUs - the same hardware DLSS
// Frame Generation uses) for frame generation's motion estimation. Uses CUDA's Direct3D 11
// interop; CUDA and the optical flow library are part of the NVIDIA driver and loaded at runtime.
#pragma once

#include <d3d11.h>

#include <map>
#include <string>

class HwOpticalFlow
{
public:
	~HwOpticalFlow();

	// width/height: size of the grayscale (R8_UNORM) input textures. Fails quietly on other GPUs.
	bool Init(ID3D11Device *device, int width, int height);
	void Shutdown();
	bool Ready() const { return ready; }
	int Width() const { return width; }
	int Height() const { return height; }
	// Flow output grid (one vector per 4x4 input pixels)
	int GridWidth() const { return (width + 3) / 4; }
	int GridHeight() const { return (height + 3) / 4; }

	// Optical flow of `cur` against `prev` (both R8_UNORM, width x height) into `flow`
	// (R16G16_SINT, GridWidth x GridHeight, S10.5 pixels: for each block of `cur`, where it is in `prev`).
	bool Run(ID3D11Texture2D *prev, ID3D11Texture2D *cur, ID3D11Texture2D *flow);

	const std::string &Status() const { return status; }

private:
	struct Impl;
	Impl *d = nullptr;
	bool ready = false;
	int width = 0, height = 0;
	std::string status = "Not started";
};
