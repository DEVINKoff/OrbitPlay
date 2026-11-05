// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <algorithm>
#include <cstdint>

// Frame generation timeline.
//
// Frames arrive from the network with jitter, and 30 fps games repeat every frame in the
// 60 fps stream. Instead of inserting a fixed number of frames after each arrival (which
// judders on 144/165 Hz displays and does nothing for 30 fps games), every display refresh
// asks "what moment of the game should be on screen now?" and builds exactly that picture.
//
// - Each stored (non-duplicate) frame gets a smoothed timestamp: a predicted cadence corrected
//   gently toward the real arrival time, so network jitter doesn't reach the screen.
// - The display runs one source frame (plus a small adaptive margin) behind the newest frame,
//   so there's always a "next" frame to interpolate toward.
// - Predictive mode instead runs only a few ms behind real time and extrapolates the newest
//   frame forward along its motion (t > 1): no added buffering at all.
class FrameTimeline
{
public:
	struct Pick
	{
		bool valid = false;
		uint64_t a = 0, b = 0;   // show between these frames
		float t = 0.0f;          // 0 = a, 1 = b, > 1 = predicted past b
	};

	void Reset()
	{
		count = 0;
		period_ms = 16.667;
		margin_ms = 2.0;
		pmargin_ms = 4.0;
		last_arrival = -1.0;
	}

	// arrival_ms: when the frame finished decoding. static_frame: it was a repeat we kept anyway.
	void Add(uint64_t serial, double arrival_ms, bool scene_cut, bool static_frame)
	{
		if(last_arrival >= 0.0 && !static_frame)
		{
			double iv = arrival_ms - last_arrival;
			if(iv > 6.0 && iv < 80.0)
				period_ms += (iv - period_ms) * 0.12;
		}
		double t;
		if(count == 0 || arrival_ms - last_arrival > 150.0)
			t = arrival_ms; // first frame or after a stall: resync
		else
		{
			double pred = e[count - 1].t + period_ms;
			t = pred + 0.08 * (arrival_ms - pred);
			t = std::min(std::max(t, arrival_ms - 1.5 * period_ms), arrival_ms);
			t = std::max(t, e[count - 1].t + 2.0);
		}
		last_arrival = arrival_ms;
		if(count == 3)
		{
			e[0] = e[1];
			e[1] = e[2];
			count = 2;
		}
		e[count++] = { serial, t, scene_cut };
		margin_ms = std::max(2.0, margin_ms - 0.15);
		pmargin_ms = std::max(4.0, pmargin_ms - 0.15);
	}

	Pick Get(double now_ms, bool predictive)
	{
		Pick p;
		if(count == 0)
			return p;
		p.valid = true;
		if(predictive && count >= 2)
		{
			const Entry &a = e[count - 2], &b = e[count - 1];
			double T = now_ms - pmargin_ms;
			if(T >= b.t)
			{
				double u = 1.0 + (T - b.t) / std::max(b.t - a.t, 1e-3);
				if(u > 2.0)
				{
					// next frame is very late: stop predicting further, add a little buffer
					pmargin_ms = std::min(8.0, pmargin_ms + 0.25);
					u = 2.0;
				}
				p.a = a.serial;
				p.b = b.serial;
				p.t = b.scene_cut ? 1.0f : (float)u;
				return p;
			}
			return Interpolate(T);
		}
		return Interpolate(now_ms - (period_ms + margin_ms));
	}

	Pick Interpolate(double T)
	{
		Pick p;
		p.valid = true;
		const Entry &newest = e[count - 1];
		if(T >= newest.t)
		{
			// ran out of frames: hold the newest and give ourselves more buffer
			if(T > newest.t + 0.5)
				margin_ms = std::min(8.0, margin_ms + 0.25);
			p.a = p.b = newest.serial;
			p.t = 1.0f;
			return p;
		}
		for(int i = count - 1; i >= 1; i--)
		{
			const Entry &a = e[i - 1], &b = e[i];
			if(T >= a.t)
			{
				p.a = a.serial;
				p.b = b.serial;
				p.t = b.scene_cut ? 0.0f : (float)std::clamp((T - a.t) / std::max(b.t - a.t, 1e-3), 0.0, 1.0);
				return p;
			}
		}
		p.a = p.b = e[0].serial;
		p.t = 0.0f;
		return p;
	}

	double SourceFps() const { return 1000.0 / period_ms; }
	double BufferMs(bool predictive) const { return predictive ? pmargin_ms : period_ms + margin_ms; }

private:
	struct Entry { uint64_t serial; double t; bool scene_cut; };
	Entry e[3] = {};
	int count = 0;
	double period_ms = 16.667;
	double margin_ms = 2.0;
	double pmargin_ms = 4.0;
	double last_arrival = -1.0;
};
