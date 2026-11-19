// SPDX-License-Identifier: AGPL-3.0-only
// Packed 4:2:2 capture formats to NV12 (4:2:0), the format the hardware video decoder produces,
// so capture card frames take exactly the stream's path through the renderer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// YUY2: Y0 U Y1 V.  UYVY: U Y0 V Y1.  Chroma of two lines is averaged (rounded) into one.
inline void Packed422ToNv12(const uint8_t *src, ptrdiff_t src_pitch, int width, int height, bool uyvy,
	uint8_t *dst_y, ptrdiff_t y_pitch, uint8_t *dst_uv, ptrdiff_t uv_pitch)
{
	const int yo = uyvy ? 1 : 0, uo = uyvy ? 0 : 1, vo = uyvy ? 2 : 3;
	const int pairs = width / 2;
	for(int y = 0; y + 1 < height; y += 2)
	{
		const uint8_t *a = src + (ptrdiff_t)y * src_pitch;
		const uint8_t *b = a + src_pitch;
		uint8_t *ya = dst_y + (ptrdiff_t)y * y_pitch;
		uint8_t *yb = ya + y_pitch;
		uint8_t *uv = dst_uv + (ptrdiff_t)(y / 2) * uv_pitch;
		for(int i = 0; i < pairs; i++)
		{
			const uint8_t *pa = a + i * 4, *pb = b + i * 4;
			ya[i * 2] = pa[yo];
			ya[i * 2 + 1] = pa[yo + 2];
			yb[i * 2] = pb[yo];
			yb[i * 2 + 1] = pb[yo + 2];
			uv[i * 2] = (uint8_t)((pa[uo] + pb[uo] + 1) >> 1);
			uv[i * 2 + 1] = (uint8_t)((pa[vo] + pb[vo] + 1) >> 1);
		}
	}
}

inline void Nv12Copy(const uint8_t *src_y, ptrdiff_t src_pitch, const uint8_t *src_uv, int width, int height,
	uint8_t *dst_y, ptrdiff_t y_pitch, uint8_t *dst_uv, ptrdiff_t uv_pitch)
{
	for(int y = 0; y < height; y++)
		memcpy(dst_y + (ptrdiff_t)y * y_pitch, src_y + (ptrdiff_t)y * src_pitch, (size_t)width);
	for(int y = 0; y < height / 2; y++)
		memcpy(dst_uv + (ptrdiff_t)y * uv_pitch, src_uv + (ptrdiff_t)y * src_pitch, (size_t)width);
}
