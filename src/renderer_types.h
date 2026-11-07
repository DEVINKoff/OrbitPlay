// SPDX-License-Identifier: AGPL-3.0-only
// Types shared by the Direct3D 11 (Windows) and Metal (iPad) renderers.
#pragma once

#include <cstdint>

enum class UpscaleMode : int { Off = 0, Enhanced = 1, AI = 2 };
enum class AspectMode : int { Fit = 0, Fill = 1, Stretch = 2 };

// How each decoded frame is processed (applied in SubmitFrame).
struct ProcessOptions
{
	UpscaleMode upscale = UpscaleMode::Off;
	int target_height = 0;          // 0 = match the video area on screen
	float artifact_reduction = 0.4f;
	float sharpness = 0.5f;         // enhanced upscaler
	float ai_sharpness = 0.35f;     // extra sharpening after AI super resolution (0..2)
	float texture_boost = 0.0f;     // fine texture enhancement, 0..1
	float clarity = 0.0f;           // large-radius local contrast, 0..1
	float overshoot = 0.0f;         // allow sharpening past local min/max (0 = no halos), 0..1
	float deband = 0.3f;            // gradient banding removal, 0 = off
	float paper_white = 203.0f;     // used to put SDR and HDR on one scale for ambient light
	bool ai_hdr = false;            // NVIDIA RTX Video HDR for SDR streams on an HDR display
	bool frame_gen = false;
	bool ambient = false;
	AspectMode aspect = AspectMode::Fit;
};

// How the picture is shown (applied in Present).
struct PresentOptions
{
	int sync_interval = 1;          // 0 = present immediately (tearing allowed if supported)
	float paper_white = 203.0f;
	float hdr_boost = 0.0f;         // SDR -> HDR highlight expansion (shader), 0..1
	float ambient = 0.0f;           // ambient light intensity, 0 = off
	AspectMode aspect = AspectMode::Fit;
	int ambient_style = 0;          // 0 glow, 1 extend
	float brightness = 0.0f, contrast = 1.0f, saturation = 1.0f, vibrance = 0.0f;
	bool compare = false;           // split screen: original on the left, processed on the right
	float compare_split = 0.5f;     // divider position, fraction of the video width
};

enum class VideoView { None, Latest, Interpolated };

struct SubmitResult
{
	uint64_t serial = 0;     // id of the stored frame, 0 = not stored
	bool duplicate = false;  // identical to the previous frame (30 fps game in a 60 fps stream, static scene)
	bool scene_cut = false;  // most of the picture changed: don't interpolate across it
};

