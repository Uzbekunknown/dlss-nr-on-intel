/* The render-scale floor. See nr_menu_floor.h for why this is its own file. */

#include "nr_menu_floor.h"

/* ---------------------------------------------------------------- the render scale's floor
 *
 * **What the knob's minimum should be, and why it is not a constant.**
 *
 * `render_scale` multiplies the picture, and the network then runs on a field at least
 * `min_extent` a side, aligned to the graph's own reductions. Below the point where the picture
 * clears that floor, the field stops shrinking: at 1280x720 every scale from 0.25 down produced
 * the same 320x320 field, so five settings bought nothing and the log named the requested value
 * while the network ran a larger one.
 *
 * The point depends on the frame - 1280x720 bottoms out at 0.30, 1920x1080 at 0.20, 640x360 at
 * 0.55 - so it is computed here rather than written down.
 *
 * **This is a second implementation of a rule that lives in `src/ref/nr_frame.py`.** The panel
 * cannot call Python: it runs in the game's process. `test_menu_pacing` holds the two together by
 * checking this against values generated from the Python side, so a change to one that the other
 * does not follow fails the fixtures rather than the user seeing a slider that lies.
 */

/* The graph's own minimum. `min_extent` is the knob that moves the vendor's field minimum, and it
 * arrives as `minimum`; the graph's 128 is the hard lower bound `network_geometry` applies. */
#define PANEL_GRAPH_MIN_EXTENT 128

/* Two to the number of times the graph reduces an axis of `extent` pixels - the same six halvings,
 * each rounded up to 4, that `_field_alignment` walks. */
static uint32_t panel_field_alignment(uint32_t extent)
{
	uint32_t size = extent;
	int reductions = 0;
	for (int level = 0; level < 6; ++level) {
		uint32_t half = (((size + 1) / 2) + 3) / 4 * 4;
		if (half < size) ++reductions;
		if (level == 0 && half % 8 != 0) ++reductions;
		size = half;
	}
	return (uint32_t)1 << reductions;
}

/* The field's area for a frame, which is what the network actually pays for: every frame on the
 * same field costs the same, so "cheaper" means a smaller field, not a smaller picture. */
static uint32_t panel_field_area(uint32_t width, uint32_t height, uint32_t minimum)
{
	uint32_t floor_size = minimum > PANEL_GRAPH_MIN_EXTENT ? minimum : PANEL_GRAPH_MIN_EXTENT;
	uint32_t aw = panel_field_alignment(width), ah = panel_field_alignment(height);
	uint32_t fw = (width + aw - 1) / aw * aw;
	uint32_t fh = (height + ah - 1) / ah * ah;
	if (fw < floor_size) fw = floor_size;
	if (fh < floor_size) fh = floor_size;
	/* The vendor's extra alignment when both sides are four alignments; it costs an area step and
	 * `network_geometry` applies it, so the area has to follow or the floor lands a step low. */
	if (fw % (4 * aw) == 0 && fh % (4 * ah) == 0) fw += aw;
	fw = (fw + 63) / 64 * 64;
	fh = (fh + 63) / 64 * 64;
	return fw * fh;
}

/* The field `render_extent` picks for a frame at `scale` - **the cheapest one at or above the
 * scale's own, not the one the scale calculates**.
 *
 * That distinction is the whole of this function's second half. `render_extent` walks upward from
 * the scaled picture because the vendor's field is not monotonic in the picture's size: 800x450 at
 * 0.35 is 280x158, whose field is 448x320, while a larger 320x180 lands on 320x320 - cheaper. A
 * floor computed from `width * scale` alone would say 0.35 buys something on that window, and it
 * does not.
 *
 * The walk runs the full width rather than stopping early, which is what the Python side does: it
 * takes every candidate at or below the running cheapest and the last of those wins, so the search
 * must reach the widest frame that is still no more expensive - stopping at the first price rise
 * would miss a cheaper size further along, because the alignment makes the price non-monotonic. */
static uint32_t panel_extent_area(uint32_t width, uint32_t height, float scale, uint32_t minimum)
{
	uint32_t w = (uint32_t)((float)width * scale + 0.5f);
	uint32_t h = (uint32_t)((float)height * scale + 0.5f);
	if (w < 64) w = 64;
	if (h < 64) h = 64;
	uint32_t cheapest = panel_field_area(w, h, minimum);
	/* One pixel at a time, as the reference does: the field's alignment makes a jump of a few
	 * pixels change the price, so stepping coarser would miss the cheaper sizes. Bounded by the
	 * frame's width, and the caller only asks once per present. */
	for (uint32_t candidate = w + 1; candidate <= width; ++candidate) {
		uint32_t ch = (uint32_t)((float)height * (float)candidate / (float)width + 0.5f);
		if (ch < 64) ch = 64;
		uint32_t price = panel_field_area(candidate, ch, minimum);
		if (price <= cheapest) cheapest = price;
	}
	return cheapest;
}

/* The lowest `render_scale`, on the knob's own step, that still runs on the cheapest network field.
 *
 * **The last step on that field, not the first step off it.** `render_extent` already maps every
 * scale at or below a point to the widest frame on the cheapest field, so at 1280x720 the scales
 * 0.05 through 0.25 all give 320x180 on 320x320 and render the same frame at the same cost. 0.25
 * is the last of them and the lowest worth offering; returning the step after it would drop the
 * cheapest field from the knob entirely and hand the user a floor that costs 1.2x the work
 * (384x320) for nothing. */
float nr_panel_lowest_scale(uint32_t width, uint32_t height, uint32_t minimum, float step)
{
	if (width == 0 || height == 0 || step <= 0.0f) return 0.0f;
	/* **Cached, because the walk is per pixel.** The answer depends only on these four numbers, and
	 * the row builder asks every present while the size changes on a resize and almost never
	 * otherwise. A hit is four comparisons; a miss is the same few thousand as before.
	 *
	 * **Thread-local, not a plain static.** The panel runs under `device_data.present_lock`, and
	 * that lock is per DEVICE - so two Vulkan devices presenting at once in one process reach this
	 * function concurrently, and a shared cache would be a data race on five words. `_Thread_local`
	 * keeps the hit for the thread that fills it (the panel always asks from the same thread) and
	 * makes the miss case merely repeat work rather than read a half-written answer. `__declspec`
	 * on MSVC, which has no C11 `_Thread_local` under /TC, and the standard spelling elsewhere. */
#if defined(_MSC_VER)
	__declspec(thread) static uint32_t cached_width, cached_height, cached_minimum;
	__declspec(thread) static float cached_step, cached_answer;
#else
	static _Thread_local uint32_t cached_width, cached_height, cached_minimum;
	static _Thread_local float cached_step, cached_answer;
#endif
	if (width == cached_width && height == cached_height
	    && minimum == cached_minimum && step == cached_step)
		return cached_answer;
	uint32_t cheapest = 0;
	float last_on_cheapest = 0.0f;
	for (int i = 1; (float)i * step <= 1.0f + 1e-6f; ++i) {
		float scale = (float)i * step;
		if (scale > 1.0f) scale = 1.0f;
		uint32_t area = panel_extent_area(width, height, scale, minimum);
		if (cheapest == 0 || area < cheapest) {
			cheapest = area;
			last_on_cheapest = scale;
		} else if (area == cheapest) {
			last_on_cheapest = scale;
		}
	}
	cached_width = width;
	cached_height = height;
	cached_minimum = minimum;
	cached_step = step;
	cached_answer = last_on_cheapest > 0.0f ? last_on_cheapest : 1.0f;
	return cached_answer;
}

/* The published name for the field's area, so a fixture can check what the floor is for rather
 * than only the numbers that come out of it. See nr_menu_floor.h. */
uint32_t nr_panel_scale_area(uint32_t width, uint32_t height, float scale, uint32_t minimum)
{
	if (width == 0 || height == 0) return 0;
	if (scale < 0.0f) scale = 0.0f;
	if (scale > 1.0f) scale = 1.0f;
	return panel_extent_area(width, height, scale, minimum);
}