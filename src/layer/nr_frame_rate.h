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

/* **The two graphs, after OptiScaler's.** A rate is one number for the last second; a graph is the
 * shape of that second, which is what tells a steady 40 from a 60 that stalls every fourth frame.
 * Two series, because they answer different questions and a player blaming NR needs to know which
 * one moved:
 *
 *   frame   the milliseconds between presents - what the screen actually did, frame generation
 *           included, so a generator's frames are the short ones;
 *   net     the network's milliseconds for the frames NR answered, which is the cost NR adds.
 *
 * One sample a present, in a ring: the panel draws its whole width from these and never keeps
 * anything else, so a lost frame costs one point rather than the graph.
 *
 * 120 samples is two seconds at 60 and four at 30 - long enough to see a hitch travel, short
 * enough that the right edge is now. The ring can hold the value up to `NR_GRAPH_CEIL_MS`, which
 * the drawing side scales against; a frame slower than that is drawn at the ceiling rather than
 * making every other point invisible. */
#define NR_GRAPH_SAMPLES 120
#define NR_GRAPH_CEIL_MS 100.0

typedef struct nr_frame_graph {
	/* Milliseconds between presents, oldest first in the ring. A slot is `-1` until it is used, so
	 * a graph starts empty rather than at zero - a zero would draw as a perfect frame. */
	float frame_ms[NR_GRAPH_SAMPLES];
	float net_ms[NR_GRAPH_SAMPLES];
	int count;      /* how many slots hold a sample, up to NR_GRAPH_SAMPLES */
	int next;       /* where the next sample goes */
} nr_frame_graph;

static inline void nr_frame_graph_reset(nr_frame_graph *g)
{
	if (!g) return;
	memset(g, 0, sizeof *g);
	for (int i = 0; i < NR_GRAPH_SAMPLES; ++i) {
		g->frame_ms[i] = -1.0f;
		g->net_ms[i] = -1.0f;
	}
}

/* One present: the milliseconds since the last one, and the network's time for it when the answer
 * carried one (`net_ms` below 0 appends no net point, which is every frame on the CPU path). */
static inline void nr_frame_graph_add(nr_frame_graph *g, float frame_ms, float net_ms)
{
	if (!g) return;
	if (frame_ms < 0.0f) frame_ms = 0.0f;
	if (frame_ms > (float)NR_GRAPH_CEIL_MS) frame_ms = (float)NR_GRAPH_CEIL_MS;
	g->frame_ms[g->next] = frame_ms;
	if (net_ms < 0.0f || net_ms > (float)NR_GRAPH_CEIL_MS) net_ms = -1.0f;
	g->net_ms[g->next] = net_ms;
	g->next = (g->next + 1) % NR_GRAPH_SAMPLES;
	if (g->count < NR_GRAPH_SAMPLES) g->count++;
}

/* The ring in drawing order, oldest first, as ImGui's `PlotLines` wants it: one contiguous run of
 * `float`. Returns how many points were written, which is the ring's `count`; a caller with a
 * scratch array of `NR_GRAPH_SAMPLES` floats and no gap. A sample the ring never used is left out
 * rather than drawn as zero. */
static inline int nr_frame_graph_frame(const nr_frame_graph *g, float *out, int capacity)
{
	if (!g || !out || capacity <= 0) return 0;
	const int n = g->count < capacity ? g->count : capacity;
	const int start = g->count < NR_GRAPH_SAMPLES ? 0 : g->next;
	for (int i = 0; i < n; ++i) out[i] = g->frame_ms[(start + i) % NR_GRAPH_SAMPLES];
	return n;
}

static inline int nr_frame_graph_net(const nr_frame_graph *g, float *out, int capacity)
{
	if (!g || !out || capacity <= 0) return 0;
	const int n = g->count < capacity ? g->count : capacity;
	const int start = g->count < NR_GRAPH_SAMPLES ? 0 : g->next;
	for (int i = 0; i < n; ++i) out[i] = g->net_ms[(start + i) % NR_GRAPH_SAMPLES];
	return n;
}

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
	/* The last second's shape, one point a present. See `nr_frame_graph`. */
	nr_frame_graph graph;
	/* The present before this one, so a frame's own milliseconds are a difference rather than a
	 * rate over half a second: the graph wants the frame, the counter wants the second. */
	double previous;
} nr_frame_rates;

static inline void nr_frame_rates_reset(nr_frame_rates *r)
{
	if (!r) return;
	memset(r, 0, sizeof *r);
	r->network_ms = -1.0;
	nr_frame_graph_reset(&r->graph);
	r->previous = -1.0;
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
		r->previous = now;
		return;
	}
	/* **One graph point a present, before the half-second gate below.** The gate belongs to the
	 * rates, which average over a second; the graph is the shape inside that second and gets every
	 * frame. `previous` is what makes it a frame's own time rather than a rate: the difference
	 * between two presents is that frame, whatever else the last second did.
	 *
	 * A sample whose total did not move is a repeat of a held frame, not a new one - the panel runs
	 * every present and so does this, but only a present that the screen counted is a frame. The
	 * network's time is taken from the mean the rates would report, because the host's counters
	 * carry a sum, not a per-frame figure. */
	const int presented_now = counts->presented > r->seen[r->kept - 1].presented;
	if (presented_now) {
		const double frame_ms = (r->previous > 0.0 && now > r->previous)
			? (now - r->previous) * 1000.0 : -1.0;
		const uint64_t net_frames_now = counts->network_frames - r->seen[r->kept - 1].network_frames;
		const double net_ms = net_frames_now
			? (double)(counts->network_us - r->seen[r->kept - 1].network_us)
			  / (double)net_frames_now / 1000.0 : -1.0;
		if (frame_ms >= 0.0) nr_frame_graph_add(&r->graph, (float)frame_ms, (float)net_ms);
		r->previous = now;
	} else {
		r->previous = now;
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
