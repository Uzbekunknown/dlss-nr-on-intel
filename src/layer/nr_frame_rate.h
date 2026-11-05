/* The panel's frame counter: two rates and the network's time, from counts the host keeps.
 *
 * **Two rates, because under frame generation they differ.** NR runs on the frames the game
 * renders: each goes through the upscaler and NR once. A frame generator (XeSS's XeFG, say) puts
 * its own frames in after that, so the screen shows more frames than NR made - about twice as many
 * at x2. A single "fps" would be one of the two and read as the other, so the counter shows both:
 *
 *   the screen's  every present of the swap chain the panel is drawn on: in the Vulkan layer the
 *                 presents of its primary swapchain, in DirectX 12 the presents DXGI's hook sees,
 *                 which under XeFG are the generator's and include its frames;
 *   NR's          the frames the daemon answered and the host put into the picture, which is what
 *                 the proxy's "NR: N frames in 5 s" log line has always counted.
 *
 * And the network's time for one frame, when the daemon reports it: its FRAME reply carries the
 * network's microseconds in `detail` since 2026-11-05 (nr_gpu_wire.h). The frames on the CPU path
 * report none, and then the counter says nothing about it rather than a guess.
 *
 * **The host keeps counts, the panel keeps time.** A host only adds to four totals, from wherever
 * it learns of the event - a present, an answer - and the panel samples them once a present
 * (`nr_frame_rates_sample`). The rates are differences over the last second, read every half
 * second, so a number is steady enough to read and still follows a change within a second. At 27
 * frames a second a half-second window would move by two whole frames' worth from one reading to
 * the next; a second moves by one.
 *
 * Plain C with no platform in it, so the fixtures can drive it with a clock of their own.
 */
#ifndef NR_FRAME_RATE_H
#define NR_FRAME_RATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nr_menu_strings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a host counts, each a total since it started counting. */
typedef struct nr_frame_counts {
	uint64_t presented;       /* the game's presents, of the swap chain the panel is drawn on */
	uint64_t nr_frames;       /* frames NR answered and the host put into the picture */
	uint64_t network_us;      /* the network's time, summed over the answers that reported it */
	uint64_t network_frames;  /* how many answers reported it */
} nr_frame_counts;

#define NR_RATE_STEP 0.5      /* seconds from one reading to the next */
#define NR_RATE_SAMPLES 3     /* readings kept: the rates are over the last two steps, a second */
#define NR_RATE_GAP 2.0       /* no present for this long - a load, an alt-tab - and it starts over */

typedef struct nr_frame_rates {
	int ready;                /* a reading has been made, so the numbers below mean something */
	double screen_fps;
	double nr_fps;
	double network_ms;        /* below 0 when no answer in the span reported the network's time */
	/* The readings, oldest first, and when the host last gave a sample. */
	int kept;
	double at[NR_RATE_SAMPLES];
	nr_frame_counts seen[NR_RATE_SAMPLES];
	double last;
} nr_frame_rates;

static inline void nr_frame_rates_reset(nr_frame_rates *r)
{
	memset(r, 0, sizeof *r);
	r->network_ms = -1.0;
}

static inline int nr_frame_counts_behind(const nr_frame_counts *now, const nr_frame_counts *then)
{
	return now->presented < then->presented || now->nr_frames < then->nr_frames ||
	       now->network_us < then->network_us || now->network_frames < then->network_frames;
}

/* One present's sample: `now` in seconds on any monotonic clock, `counts` the host's totals.
 *
 * It starts over on the first sample, when a total went back (the host counted afresh: a new device
 * in the Vulkan layer, a panel made again), and after a pause of NR_RATE_GAP - a rate over a second
 * that held a three-second load is a number about the load. Until the next reading after a start
 * the panel shows nothing, which is half a second. */
static inline void nr_frame_rates_sample(nr_frame_rates *r, double now, const nr_frame_counts *counts)
{
	if (!r || !counts) return;
	if (r->kept <= 0 || now < r->last || now - r->last > NR_RATE_GAP ||
	    nr_frame_counts_behind(counts, &r->seen[r->kept - 1])) {
		nr_frame_rates_reset(r);
		r->kept = 1;
		r->at[0] = now;
		r->seen[0] = *counts;
		r->last = now;
		return;
	}
	r->last = now;
	if (now - r->at[r->kept - 1] < NR_RATE_STEP) return;
	if (r->kept == NR_RATE_SAMPLES) {
		for (int i = 1; i < NR_RATE_SAMPLES; ++i) {
			r->at[i - 1] = r->at[i];
			r->seen[i - 1] = r->seen[i];
		}
		r->kept--;
	}
	r->at[r->kept] = now;
	r->seen[r->kept] = *counts;
	r->kept++;
	const double span = now - r->at[0];
	if (span <= 0.0) return;
	const nr_frame_counts *then = &r->seen[0];
	r->screen_fps = (double)(counts->presented - then->presented) / span;
	r->nr_fps = (double)(counts->nr_frames - then->nr_frames) / span;
	const uint64_t reported = counts->network_frames - then->network_frames;
	r->network_ms = reported
		? (double)(counts->network_us - then->network_us) / (double)reported / 1000.0 : -1.0;
	r->ready = 1;
}

/* A rate or a time as the panel prints it: one decimal under a hundred, none above, with the
 * language's own decimal mark - a comma in Russian and Spanish, a point in English and Chinese. */
static inline void nr_rate_number(char *out, size_t size, double value, int language)
{
	if (!out || !size) return;
	if (value < 0.0) value = 0.0;
	snprintf(out, size, value < 99.95 ? "%.1f" : "%.0f", value);
	if (language == NR_LANG_RU || language == NR_LANG_ES)
		for (char *p = out; *p; ++p)
			if (*p == '.') *p = ',';
}

/* `shape` from the strings table with its `{}` replaced by `number`, appended to `out`. A shape with
 * no `{}` is appended as it is, so a translation is free to leave the number out. */
static inline void nr_rate_put(char *out, size_t size, const char *shape, const char *number)
{
	size_t used = strlen(out);
	if (!shape || used + 1 >= size) return;
	const char *brace = strstr(shape, "{}");
	if (brace)
		snprintf(out + used, size - used, "%.*s%s%s", (int)(brace - shape), shape,
		         number ? number : "", brace + 2);
	else
		snprintf(out + used, size - used, "%s", shape);
}

/* The counter's line in `language`: the screen's rate, NR's - or "NR off" while `nr_on` is 0 - and
 * the network's time when an answer in the span reported it. `nr_on` is the host's trigger as the
 * panel reads it: 0 off, 1 on, -1 when the installation has no trigger, which runs NR always.
 * `gap` goes between the parts. Writes "" and returns 0 before the first reading. */
static inline int nr_frame_rates_line(const nr_frame_rates *r, int language, int nr_on,
                                      const char *gap, char *out, size_t size)
{
	if (!out || !size) return 0;
	out[0] = '\0';
	if (!r || !r->ready) return 0;
	char number[32];
	nr_rate_number(number, sizeof number, r->screen_fps, language);
	nr_rate_put(out, size, nr_menu_text(language, NR_STR_FPS_SCREEN), number);
	nr_rate_put(out, size, gap ? gap : "  ", NULL);
	if (nr_on == 0) {
		nr_rate_put(out, size, nr_menu_text(language, NR_STR_FPS_NR_OFF), NULL);
		return 1;
	}
	nr_rate_number(number, sizeof number, r->nr_fps, language);
	nr_rate_put(out, size, nr_menu_text(language, NR_STR_FPS_NR), number);
	if (r->network_ms >= 0.0) {
		nr_rate_put(out, size, gap ? gap : "  ", NULL);
		nr_rate_number(number, sizeof number, r->network_ms, language);
		nr_rate_put(out, size, nr_menu_text(language, NR_STR_FPS_NETWORK), number);
	}
	return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* NR_FRAME_RATE_H */
