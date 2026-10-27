/* nr_xess.c — a libxess.dll in front of Intel's, so that NR works on the game's frame before
 * its interface is drawn, in the game's own D3D12: no DXVK or VKD3D.
 *
 * Where. With mode=after (the default) NR runs on XeSS's output: the scene upscaled, before
 * the game's post-processing and interface. With mode=before it runs on the colour XeSS is
 * about to upscale, where NVIDIA's own pipeline runs the network (notes/phase37), at a
 * fraction of the cost. There, though, XeSS's temporal accumulation takes NR's detail, redrawn
 * a little differently each frame at each jitter, for change it should not trust or for noise
 * it should average: halos and trails round moving edges, worse the further XeSS upscales
 * (Hogwarts, 2026-10-28).
 *
 * How, with mode=after or before. The game calls xessD3D12Execute with a command list it
 * is still recording, and nothing in a D3D12 list can wait for another API. So the list is
 * cut there (nr_xess_split.cpp, after DLSSNR-AMD), after XeSS's work or before it: the first
 * half copies the frame into a buffer the daemon shares (ENCODE in nr_xess.hlsl) and is
 * closed; when the game executes the list, its queue signals a shared fence, the daemon runs
 * the network on that frame on the GPU and signals the next value (protocol 3,
 * src/layer/nr_gpu_wire.h), and the queue waits for it before the continuation, where COMPOSE
 * adds NR's change to the frame: to XeSS's output, or to the colour XeSS then upscales. NR's
 * answer lands on its own frame; nothing is carried along motion.
 *
 * How, with mode=carry, as it was before the cut. The frame does not exist yet when the call
 * is made. Ahead of XeSS's own work on that list this file records a copy of the colour into
 * a buffer the CPU can read, and a marker the GPU writes once the copy is done. A worker
 * thread waits for the marker, sends the frame to the daemon over the layer's own pipe
 * protocol and keeps NR's *change*, its answer minus what it was given. Every later
 * execute adds the newest change to the colour in a compute pass and hands XeSS that
 * instead. The change is a few frames old when it lands, so it is read where the game's
 * own motion vectors say each surface was on the frame NR was given: a tracker per frame
 * in flight follows them back, one frame at a time (nr_xess.hlsl).
 *
 * The scene colour is linear HDR, and the network was trained on a display-referred
 * sRGB picture. The frame NR sees is src/ref/nr_display.py's proxy of it — a soft knee on
 * luminance above 0.75 of `white` — and the change goes back through the same ratio, in
 * nr_xess.hlsl.
 *
 * Every other export jumps straight to the original, renamed libxess_nr_orig.dll beside
 * this one (gen_xess_exports.py). Settings are read from nr-xess.txt beside it, and what
 * happens is written to nr-xess.log there.
 */
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../layer/nr_transport.h"
#include "../layer/nr_gpu_d3d12.h"
#include "../layer/nr_gpu_rpc.h"
#include "nr_xess_split.h"
/* nr_xess.hlsl, compiled by fxc once for each pass, each with the root signature */
#include "nr_xess_clear.h"
#include "nr_xess_claim.h"
#include "nr_xess_track.h"
#include "nr_xess_match.h"
#include "nr_xess_reduce.h"
#include "nr_xess_apply.h"
#include "nr_xess_encode.h"
#include "nr_xess_motion.h"
#include "nr_xess_compose.h"
#ifndef NR_XESS_TEST
#include "nr_xess_exports.h"
#endif

/* ---- The XeSS SDK's types, as far as this file reads them (xess.h, xess_d3d12.h) ---- */

typedef struct _xess_context_handle_t *xess_context_handle_t;
typedef int xess_result_t;
#define XESS_RESULT_ERROR_CANT_LOAD_LIBRARY (-11)
typedef struct { uint32_t x, y; } xess_2d_t;

typedef struct {
	xess_2d_t outputResolution;
	int qualitySetting;
	uint32_t initFlags;
	uint32_t creationNodeMask;
	uint32_t visibleNodeMask;
	ID3D12Heap *pTempBufferHeap;
	uint64_t bufferHeapOffset;
	ID3D12Heap *pTempTextureHeap;
	uint64_t textureHeapOffset;
	ID3D12PipelineLibrary *pPipelineLibrary;
} xess_d3d12_init_params_t;

typedef struct {
	ID3D12Resource *pColorTexture;
	ID3D12Resource *pVelocityTexture;
	ID3D12Resource *pDepthTexture;
	ID3D12Resource *pExposureScaleTexture;
	ID3D12Resource *pResponsivePixelMaskTexture;
	ID3D12Resource *pOutputTexture;
	float jitterOffsetX;
	float jitterOffsetY;
	float exposureScale;
	uint32_t resetHistory;
	uint32_t inputWidth;
	uint32_t inputHeight;
	xess_2d_t inputColorBase;
	xess_2d_t inputMotionVectorBase;
	xess_2d_t inputDepthBase;
	xess_2d_t inputResponsiveMaskBase;
	xess_2d_t reserved0;
	xess_2d_t outputColorBase;
	ID3D12DescriptorHeap *pDescriptorHeap;
	uint32_t descriptorHeapOffset;
} xess_d3d12_execute_params_t;

_Static_assert(sizeof(xess_d3d12_execute_params_t) == 136,
	       "xess_d3d12_execute_params_t is 136 bytes in every XeSS SDK since 1.0");

typedef xess_result_t (*execute_fn)(xess_context_handle_t, ID3D12GraphicsCommandList *,
				    const xess_d3d12_execute_params_t *);
typedef xess_result_t (*init_fn)(xess_context_handle_t, const xess_d3d12_init_params_t *);
typedef xess_result_t (*scale_fn)(xess_context_handle_t, float, float);
#define XESS_INIT_FLAG_HIGH_RES_MV 1u

static execute_fn real_execute;
static init_fn real_init;
static scale_fn real_velocity_scale, real_jitter_scale;

/* What the game told XeSS about its inputs; one context at a time is all a game uses. */
static struct {
	float velocity[2];   /* xessSetVelocityScale: a motion texel's value to pixels */
	float jitter[2];     /* xessSetJitterScale */
	uint32_t flags;      /* xessD3D12Init */
	xess_2d_t output;
	int quality;
} xess = { { 1, 1 }, { 1, 1 }, 0, { 0, 0 }, 0 };

/* ---- The log and the settings, both beside this DLL ---- */

static wchar_t here[MAX_PATH];          /* this DLL's directory, with its backslash */
static wchar_t log_path[MAX_PATH + 16];

static void say(const char *fmt, ...)
{
	char line[1024];
	SYSTEMTIME t;
	GetLocalTime(&t);
	int n = snprintf(line, sizeof line, "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute,
			 t.wSecond, t.wMilliseconds);
	va_list args;
	va_start(args, fmt);
	vsnprintf(line + n, sizeof line - (size_t)n, fmt, args);
	va_end(args);
	if (!log_path[0]) return;
	HANDLE file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
				  NULL, OPEN_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) return;
	DWORD written;
	WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
	CloseHandle(file);
}

enum { CARRY, BEFORE, AFTER };   /* `mode=`, below */

static struct {
	char pipe[256];
	float white;      /* the scene value shown as display white */
	float strength;   /* how much of NR's change the colour takes, 0-1 */
	int enabled;      /* 0 hands every call straight to XeSS */
	int dump;         /* write one frame in and out beside the log, for a look */
	int warp;         /* 0 adds the change where it was captured, ignoring motion */
	int motion_history; /* 0 sends the daemon no motion, and its history stands still */
	/* How far what NR was given may lie outside the range of the pixel's neighbourhood
	 * now, in nr_xess.hlsl's `likeness`, before its change is refused there: partly from the
	 * first, wholly from the second. */
	float match_from, match_to;
	/* 1 makes the game wait for NR: each frame takes the answer to the frame before it, and
	 * the game runs at NR's rate. 0 lets the game run on, the change carried along motion. */
	int sync;
	/* `record=N` records the next N frames XeSS is handed into `record_dir`, for
	 * replay_nr_xess; each time the file is saved with a nonzero N. */
	int record;
	char record_dir[MAX_PATH];
	/* `mode=after` (AFTER, the default): the list cut after XeSS, NR's answer on XeSS's output
	 * of the same frame, before the game's post-processing and interface. `mode=before`
	 * (BEFORE; `cut` says the same): cut before it, NR on the colour XeSS upscales. `mode=carry`
	 * (CARRY): the change to an earlier frame carried along motion. Read once, as the pipe is. */
	int cut;
} config = { "\\\\.\\pipe\\nr-xess", 1.0f, 1.0f, 1, 0, 1, 1, 0.02f, 0.06f, 0, 0, "", AFTER };

static volatile LONG record_asked;      /* frames to record, set by the settings, taken by the hook */

static FILETIME settings_time;

/* key=value lines; re-read by the worker when the file changes, so strength and white
 * can be set while the game runs. The pipe is read once. */
static void read_settings(int first)
{
	wchar_t path[MAX_PATH + 16];
	_snwprintf(path, MAX_PATH + 16, L"%lsnr-xess.txt", here);
	WIN32_FILE_ATTRIBUTE_DATA info;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) return;
	if (!first && CompareFileTime(&info.ftLastWriteTime, &settings_time) == 0) return;
	settings_time = info.ftLastWriteTime;
	FILE *file = _wfopen(path, L"r");
	if (!file) return;
	char line[512];
	while (fgets(line, sizeof line, file)) {
		char *equals = strchr(line, '=');
		if (!equals || line[0] == '#') continue;
		*equals = '\0';
		char *value = equals + 1;
		value[strcspn(value, "\r\n")] = '\0';
		if (!strcmp(line, "pipe") && first)
			snprintf(config.pipe, sizeof config.pipe, "%s", value);
		else if (!strcmp(line, "mode") && first)
			config.cut = !strcmp(value, "carry") ? CARRY : !strcmp(value, "after") ? AFTER : BEFORE;
		else if (!strcmp(line, "white"))
			config.white = (float)atof(value);
		else if (!strcmp(line, "strength"))
			config.strength = (float)atof(value);
		else if (!strcmp(line, "enabled"))
			config.enabled = atoi(value);
		else if (!strcmp(line, "dump"))
			config.dump = atoi(value);
		else if (!strcmp(line, "warp"))
			config.warp = atoi(value);
		else if (!strcmp(line, "motion_history"))
			config.motion_history = atoi(value);
		else if (!strcmp(line, "match_from"))
			config.match_from = (float)atof(value);
		else if (!strcmp(line, "match_to"))
			config.match_to = (float)atof(value);
		else if (!strcmp(line, "sync"))
			config.sync = atoi(value);
		else if (!strcmp(line, "record"))
			config.record = atoi(value);
		else if (!strcmp(line, "record_dir"))
			snprintf(config.record_dir, sizeof config.record_dir, "%s", value);
	}
	if (config.record > 0 && config.record_dir[0]) InterlockedExchange(&record_asked, config.record);
	fclose(file);
	if (!(config.white > 0)) config.white = 1;
	if (!(config.strength >= 0)) config.strength = 0;
	if (!(config.match_from >= 0)) config.match_from = 0;
	if (!(config.match_to > config.match_from)) config.match_to = config.match_from + 0.01f;
	say("settings: mode %s, enabled %d, white %g, strength %g, warp %d, motion history %d, "
	    "match %g-%g, sync %d, pipe %s\n",
	    config.cut == AFTER ? "after" : config.cut == BEFORE ? "before" : "carry", config.enabled, config.white,
	    config.strength, config.warp, config.motion_history, config.match_from, config.match_to,
	    config.sync, config.pipe);
}

/* ---- Pixels on the CPU: the proxy NR is given, and its change ---- */

static float from_half(uint16_t h)
{
	uint32_t sign = (uint32_t)(h & 0x8000) << 16, exponent = (h >> 10) & 0x1F;
	uint32_t mantissa = h & 0x3FF, bits;
	if (exponent == 0 && mantissa == 0) {
		bits = sign;
	} else if (exponent == 0) {
		exponent = 127 - 15 + 1;
		while (!(mantissa & 0x400)) {
			mantissa <<= 1;
			exponent--;
		}
		bits = sign | exponent << 23 | (mantissa & 0x3FF) << 13;
	} else if (exponent == 31) {
		bits = sign | 0x7F800000 | mantissa << 13;
	} else {
		bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
	}
	float value;
	memcpy(&value, &bits, sizeof value);
	return value;
}

/* Round to nearest even, as the GPU's f32tof16 does. */
static uint16_t to_half(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	uint32_t sign = (bits >> 16) & 0x8000, mantissa = bits & 0x7FFFFF;
	int exponent = (int)((bits >> 23) & 0xFF) - 127 + 15;
	if (((bits >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00 | (mantissa ? 0x200 : 0));
	if (exponent >= 31) return (uint16_t)(sign | 0x7C00);
	if (exponent <= 0) {
		if (exponent < -10) return (uint16_t)sign;
		mantissa |= 0x800000;
		uint32_t shift = (uint32_t)(14 - exponent), half = mantissa >> shift;
		uint32_t rest = mantissa & ((1u << shift) - 1), middle = 1u << (shift - 1);
		if (rest > middle || (rest == middle && (half & 1))) half++;
		return (uint16_t)(sign | half);
	}
	uint32_t half = (uint32_t)exponent << 10 | mantissa >> 13, rest = mantissa & 0x1FFF;
	if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) half++;
	return (uint16_t)(sign | half);
}

static float half_value[65536];
static float linear8[256];              /* an sRGB byte's linear value */
#define SRGB_STEPS 16384
static uint8_t srgb8[SRGB_STEPS + 1];   /* linear [0, 1] in 1/16384 steps -> an sRGB byte */
static uint16_t change_half[65536];     /* (answer << 8 | given) -> their linear difference */
static uint16_t likeness_half[256];     /* an sRGB byte -> nr_xess.hlsl's `likeness` of it */

static void build_tables(void)
{
	for (uint32_t i = 0; i < 65536; i++) half_value[i] = from_half((uint16_t)i);
	for (int i = 0; i < 256; i++) {
		double v = i / 255.0;
		linear8[i] = (float)(v < 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
	}
	for (int i = 0; i <= SRGB_STEPS; i++) {
		double v = (double)i / SRGB_STEPS;
		double s = v < 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055;
		srgb8[i] = (uint8_t)(s * 255 + 0.5);
	}
	for (uint32_t answer = 0; answer < 256; answer++)
		for (uint32_t given = 0; given < 256; given++)
			change_half[answer << 8 | given] = to_half(linear8[answer] - linear8[given]);
	for (int i = 0; i < 256; i++) likeness_half[i] = to_half(sqrtf(linear8[i]));
}

enum { KIND_RGBA16F, KIND_R11G11B10F, KIND_R10G10B10A2 };

static int kind_of(DXGI_FORMAT format, UINT *texel)
{
	switch (format) {
	case DXGI_FORMAT_R16G16B16A16_FLOAT: *texel = 8; return KIND_RGBA16F;
	case DXGI_FORMAT_R11G11B10_FLOAT: *texel = 4; return KIND_R11G11B10F;
	case DXGI_FORMAT_R10G10B10A2_UNORM: *texel = 4; return KIND_R10G10B10A2;
	default: return -1;
	}
}

static void load_texel(const uint8_t *at, int kind, float *rgb)
{
	if (kind == KIND_RGBA16F) {
		const uint16_t *h = (const uint16_t *)at;
		rgb[0] = half_value[h[0]];
		rgb[1] = half_value[h[1]];
		rgb[2] = half_value[h[2]];
		return;
	}
	uint32_t v;
	memcpy(&v, at, sizeof v);
	if (kind == KIND_R11G11B10F) {
		rgb[0] = half_value[(v & 0x7FF) << 4];
		rgb[1] = half_value[((v >> 11) & 0x7FF) << 4];
		rgb[2] = half_value[((v >> 22) & 0x3FF) << 5];
		return;
	}
	rgb[0] = (float)(v & 0x3FF) / 1023;
	rgb[1] = (float)((v >> 10) & 0x3FF) / 1023;
	rgb[2] = (float)((v >> 20) & 0x3FF) / 1023;
}

/* src/ref/nr_display.py `encode`: over white, the knee on luminance, then sRGB. */
static void encode_proxy(const float *rgb, float inverse_white, uint8_t *out)
{
	float r = rgb[0] > 0 ? rgb[0] * inverse_white : 0;
	float g = rgb[1] > 0 ? rgb[1] * inverse_white : 0;
	float b = rgb[2] > 0 ? rgb[2] * inverse_white : 0;
	float lit = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	if (lit > 0.75f) {
		float scale = (0.75f + 0.25f * (1 - expf(-(lit - 0.75f) / 0.25f))) / lit;
		r *= scale;
		g *= scale;
		b *= scale;
	}
	out[0] = srgb8[r >= 1 ? SRGB_STEPS : (int)(r * SRGB_STEPS + 0.5f)];
	out[1] = srgb8[g >= 1 ? SRGB_STEPS : (int)(g * SRGB_STEPS + 0.5f)];
	out[2] = srgb8[b >= 1 ? SRGB_STEPS : (int)(b * SRGB_STEPS + 0.5f)];
	out[3] = 255;
}

/* ---- The daemon ---- */

/* One frame each way, the layer's protocol (src/layer/nr_layer.c `process_frame`):
 * R8G8B8A8_UNORM holding sRGB-encoded values, as a swapchain would. With `motion` — for
 * each pixel, two floats: the offset to where its surface was on the frame sent before,
 * NaN where it was not on screen — the request is the daemon's MAGIC_MOTION, and its
 * history follows the game's own vectors instead of standing still. */
static int exchange_pipe(const uint8_t *frame, const float *motion, uint8_t *answer,
			 uint32_t width, uint32_t height)
{
	nr_link link;
	if (nr_link_connect(&link, config.pipe) != 0) return -1;
	uint32_t header[4] = { motion ? 0x324E524Eu : 0x304E524Eu, width, height, 37 };
	size_t bytes = (size_t)width * height * 4;
	int failed = nr_link_write(&link, header, sizeof header) != 0
		     || nr_link_write(&link, frame, bytes) != 0
		     || (motion && nr_link_write(&link, motion, bytes * 2) != 0)
		     || nr_link_read(&link, answer, bytes) != 0;
	nr_link_close(&link);
	return failed ? -1 : 0;
}

static int (*exchange)(const uint8_t *, const float *, uint8_t *, uint32_t, uint32_t) = exchange_pipe;

/* ---- The state shared by the hook and the worker ---- */

/* The newest change, the one before it while it fades out, the one the worker is writing,
 * and one the GPU may still be reading. */
#define CHANGES 4
/* One for each of those the hook reads, one for the frame NR is working on. */
#define TRACKERS 3
#define CELL 8          /* pixels a side of the cells an untrusted change is filled from */

/* nr_xess.hlsl's cbuffer, field for field. */
struct constants {
	uint32_t width, height, pitch, kind;
	float white, strength;
	uint32_t change_pitch, warp;
	float shift[2];
	uint32_t motion_pitch, reset;
	uint32_t motion_size[2];
	float motion_scale[2];
	float motion_texels[2];
	float jitter[2];
	float previous_jitter[2];
	uint32_t vectors, fading;
	float old_shift[2];
	float fade;
	uint32_t cell;
	uint32_t cells[2];
	float match_from, match_to;
};

_Static_assert(sizeof(struct constants) == 32 * 4, "the root signature takes 32 constants");

static struct {
	SRWLOCK lock;
	HANDLE wake;                 /* set when a capture has been recorded */
	HANDLE idle;                 /* set when the worker is done with one */
	HANDLE thread;
	int broken;                  /* something failed: every call goes straight to XeSS */
	ID3D12Device *device;
	ID3D12RootSignature *signature;
	ID3D12PipelineState *pipeline;
	ID3D12PipelineState *clear_pipeline, *claim_pipeline;
	ID3D12PipelineState *track_pipeline, *match_pipeline, *reduce_pipeline;
	ID3D12PipelineState *encode_pipeline, *motion_pipeline, *compose_pipeline;     /* mode=cut */
	ID3D12Resource *srgb_table, *linear_table;   /* `srgb8` for ENCODE and `linear8` for COMPOSE */
	ID3D12Resource *claims;      /* CLAIM's sixteen bytes a pixel, read by TRACK */
	ID3D12Resource *matched;     /* MATCH's seven floats a pixel */
	ID3D12Resource *sums;        /* ... summed over each cell by REDUCE, with the vectors */
	/* sized to the colour, and rebuilt when it changes */
	UINT width, height, texel;
	DXGI_FORMAT format;
	int kind;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT64 bytes;
	ID3D12Resource *readback;    /* the frame for NR, readable by the CPU */
	ID3D12Resource *staging;     /* the colour as a buffer, for the compute pass */
	ID3D12Resource *output;      /* the pass's result as a buffer ... */
	ID3D12Resource *mixed;       /* ... copied into the texture XeSS is given */
	D3D12_RESOURCE_STATES mixed_state;
	ID3D12Resource *marker;      /* the last execute whose work the GPU has finished */
	ID3D12Resource *change[CHANGES];
	uint64_t *change_map[CHANGES];
	uint32_t change_used[CHANGES];
	uint32_t sequence;           /* executes recorded */
	/* where each surface was on a captured frame: two buffers each, read one, write the other */
	struct {
		ID3D12Resource *buffer[2];
		int current;
		int live;
		uint32_t anchor;     /* the execute whose frame NR was given */
	} tracker[TRACKERS];
	/* the game's motion vectors, copied every frame: this frame's and the last one's */
	UINT motion_width, motion_height;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT motion_footprint;
	ID3D12Resource *motion[2];
	int motion_next;             /* the one this frame's copy goes to */
	int motion_previous;         /* the other holds the last frame's */
	float previous_jitter[2];
	/* between the hook and the worker, under the lock */
	int capture;                 /* 0 idle, 3 being recorded, 1 recorded, 2 the worker has it */
	uint32_t capture_sequence, capture_epoch;
	float capture_white, capture_jitter[2];
	int ready;                   /* the newest change, or -1 */
	int fading;                  /* the change before it while it gives way, or -1 */
	uint32_t change_anchor[CHANGES];      /* the execute each change was captured on */
	float change_jitter[CHANGES][2];      /* and that frame's jitter */
	/* the hook's own: which change it applied last, and how often a new one comes */
	int applied;
	uint32_t fade_start, arrived;
	float period;                /* executes between changes, smoothed */
	int rising;                  /* a cut came: the next change comes in from nothing ... */
	uint32_t rise_start;         /* ... from this execute */
	UINT cells_width, cells_height;
	/* The warp checked on the game's own frames: with a capture comes the tracker of the
	 * change in use, and the worker compares the frame NR had, moved by it, with this one. */
	ID3D12Resource *check_readback;
	int capture_check;
	uint32_t check_anchor;
	float check_shift[2];
	uint8_t *previous;           /* the last frame NR was given */
	float *offsets;              /* each pixel's offset to that frame, for the daemon */
	uint32_t previous_sequence;
	ULONGLONG check_said;
	uint32_t epoch;              /* a change from an older epoch is not used */
	/* the worker's */
	uint8_t *frame, *answer;
	size_t frame_bytes;
	uint32_t answered, failed;
	uint64_t window_start, window_frames;
	double window_ms;
} nr = { .ready = -1, .fading = -1, .applied = -1, .period = 4 };

static uint32_t completed(void)
{
	uint32_t *mapped = NULL, value = 0;
	D3D12_RANGE range = { 0, sizeof(uint32_t) }, none = { 0, 0 };
	if (SUCCEEDED(ID3D12Resource_Map(nr.marker, 0, &range, (void **)&mapped))) {
		value = *(volatile uint32_t *)mapped;
		ID3D12Resource_Unmap(nr.marker, 0, &none);
	}
	return value;
}

static int reached(uint32_t sequence)
{
	return (int32_t)(completed() - sequence) >= 0;
}

/* By the clock, not by counting sleeps: Sleep(1) is 15.6 ms under Windows' default timer
 * unless the game has asked for a finer one. */
static int wait_for(uint32_t sequence, DWORD limit_ms)
{
	ULONGLONG until = GetTickCount64() + limit_ms;
	while (!reached(sequence)) {
		if (GetTickCount64() >= until) return 0;
		Sleep(1);
	}
	return 1;
}

/* The frame the GPU copied, as the proxy NR is given. */
static void read_frame(float white)
{
	uint8_t *mapped = NULL;
	D3D12_RANGE range = { 0, (SIZE_T)nr.bytes }, none = { 0, 0 };
	if (FAILED(ID3D12Resource_Map(nr.readback, 0, &range, (void **)&mapped))) return;
	float inverse_white = 1 / white, rgb[3];
	for (UINT y = 0; y < nr.height; y++) {
		const uint8_t *row = mapped + nr.footprint.Offset + (size_t)y * nr.footprint.Footprint.RowPitch;
		uint8_t *out = nr.frame + (size_t)y * nr.width * 4;
		for (UINT x = 0; x < nr.width; x++) {
			load_texel(row + (size_t)x * nr.texel, nr.kind, rgb);
			encode_proxy(rgb, inverse_white, out + (size_t)x * 4);
		}
	}
	ID3D12Resource_Unmap(nr.readback, 0, &none);
}

static void dump_frames(void)
{
	const uint8_t *images[2] = { nr.frame, nr.answer };
	const wchar_t *names[2] = { L"nr-xess-in.ppm", L"nr-xess-out.ppm" };
	for (int i = 0; i < 2; i++) {
		wchar_t path[MAX_PATH + 32];
		_snwprintf(path, MAX_PATH + 32, L"%ls%ls", here, names[i]);
		FILE *file = _wfopen(path, L"wb");
		if (!file) continue;
		fprintf(file, "P6\n%u %u\n255\n", nr.width, nr.height);
		for (size_t p = 0; p < (size_t)nr.width * nr.height; p++)
			fwrite(images[i] + p * 4, 1, 3, file);
		fclose(file);
	}
	say("wrote nr-xess-in.ppm and nr-xess-out.ppm\n");
}

/* replay_nr_xess's --answers: each frame NR was given and its answer, by the execute it was
 * captured on, so NR's own picture can be told from what the proxy made of it. */
static const char *answers_dir;

static void write_answer(uint32_t sequence)
{
	const uint8_t *images[2] = { nr.frame, nr.answer };
	const char *names[2] = { "given", "answer" };
	for (int i = 0; i < 2; i++) {
		char path[MAX_PATH + 32];
		snprintf(path, sizeof path, "%s\\%s_%04u.ppm", answers_dir, names[i], sequence - 1);
		FILE *file = fopen(path, "wb");
		if (!file) continue;
		fprintf(file, "P6\n%u %u\n255\n", nr.width, nr.height);
		for (size_t p = 0; p < (size_t)nr.width * nr.height; p++)
			fwrite(images[i] + p * 4, 1, 3, file);
		fclose(file);
	}
}

/* NR's answer minus what it was given, into a free change buffer, made the newest. A
 * buffer is free when the hook reads it no more — neither the newest nor the one fading
 * out — and the GPU has finished every execute that read it.
 *
 * Beside each pixel's change goes what NR was given there as nr_xess.hlsl's `likeness`
 * measures it, the square root of each of the proxy's channels: MATCH compares it with the
 * frame it is changing, and does not trust the change where the two do not look alike. */
static void publish(uint32_t epoch, uint32_t sequence, const float *jitter)
{
	int slot = -1;
	for (ULONGLONG until = GetTickCount64() + 1000; slot < 0 && GetTickCount64() < until; ) {
		uint32_t done = completed();
		AcquireSRWLockExclusive(&nr.lock);
		for (int k = 0; k < CHANGES && slot < 0; k++)
			if (k != nr.ready && k != nr.fading && (int32_t)(done - nr.change_used[k]) >= 0)
				slot = k;
		ReleaseSRWLockExclusive(&nr.lock);
		if (slot < 0) Sleep(1);
	}
	if (slot < 0) return;
	uint64_t *out = nr.change_map[slot];
	const uint8_t *given = nr.frame, *answer = nr.answer;
	for (size_t p = 0, n = (size_t)nr.width * nr.height; p < n; p++, given += 4, answer += 4) {
		out[2 * p] = (uint64_t)change_half[answer[0] << 8 | given[0]]
			     | (uint64_t)change_half[answer[1] << 8 | given[1]] << 16
			     | (uint64_t)change_half[answer[2] << 8 | given[2]] << 32;
		out[2 * p + 1] = (uint64_t)likeness_half[given[0]] | (uint64_t)likeness_half[given[1]] << 16
				 | (uint64_t)likeness_half[given[2]] << 32;
	}
	AcquireSRWLockExclusive(&nr.lock);
	if (epoch == nr.epoch) {
		nr.ready = slot;
		nr.change_anchor[slot] = sequence;
		nr.change_jitter[slot][0] = jitter[0];
		nr.change_jitter[slot][1] = jitter[1];
	}
	ReleaseSRWLockExclusive(&nr.lock);
}

/* For each pixel of the frame NR is given, where its surface was on the one NR was given
 * before: the tracker of the change in use, which starts on that frame, plus the two
 * frames' jitter, as the apply pass reads it (nr_xess.hlsl). NaN where that was off screen,
 * and everywhere when that frame is not the one the daemon had last. The daemon's history
 * follows it (MAGIC_MOTION).
 *
 * Once a second, while things move, this also says whether the game's vectors are right:
 * the last frame NR was given, read where they send each pixel of this one, should look
 * more like this frame than it does unmoved. In the proxy's own sRGB levels. */
static void measure_motion(int known, const float *shift)
{
	int w = (int)nr.width, h = (int)nr.height;
	size_t pixels = (size_t)w * h;
	const float *offsets = NULL;
	D3D12_RANGE all = { 0, (SIZE_T)pixels * 8 }, none = { 0, 0 };
	if (!known || FAILED(ID3D12Resource_Map(nr.check_readback, 0, &all, (void **)&offsets))) {
		for (size_t i = 0; i < 2 * pixels; i++) nr.offsets[i] = NAN;
		return;
	}
	size_t entered = 0;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			size_t i = (size_t)y * w + x;
			float dx = offsets[2 * i] + shift[0], dy = offsets[2 * i + 1] + shift[1];
			int off = x + dx < -0.5f || y + dy < -0.5f || x + dx > w - 0.5f || y + dy > h - 0.5f;
			nr.offsets[2 * i] = off ? NAN : dx;
			nr.offsets[2 * i + 1] = off ? NAN : dy;
			entered += off;
		}
	ID3D12Resource_Unmap(nr.check_readback, 0, &none);

	ULONGLONG now = GetTickCount64();
	if (now - nr.check_said < 1000) return;
	double moved = 0, followed = 0, stayed = 0;
	size_t counted = 0;
	for (int y = 0; y < h; y += 2)
		for (int x = 0; x < w; x += 2) {
			const float *s = nr.offsets + 2 * ((size_t)y * w + x);
			float gx = x + s[0], gy = y + s[1];
			if (isnan(s[0]) || gx < 0 || gy < 0 || gx > w - 1 || gy > h - 1) continue;
			int ix = (int)gx, iy = (int)gy;
			int jx = ix + 1 < w ? ix + 1 : ix, jy = iy + 1 < h ? iy + 1 : iy;
			float fx = gx - ix, fy = gy - iy;
			const uint8_t *now_px = nr.frame + ((size_t)y * w + x) * 4;
			const uint8_t *a = nr.previous + ((size_t)iy * w + ix) * 4, *b = nr.previous + ((size_t)iy * w + jx) * 4;
			const uint8_t *c = nr.previous + ((size_t)jy * w + ix) * 4, *d = nr.previous + ((size_t)jy * w + jx) * 4;
			const uint8_t *same = nr.previous + ((size_t)y * w + x) * 4;
			for (int k = 0; k < 3; k++) {
				float there = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
				followed += fabsf(now_px[k] - there);
				stayed += abs((int)now_px[k] - (int)same[k]);
			}
			moved += sqrt((double)s[0] * s[0] + (double)s[1] * s[1]);
			counted++;
		}
	if (!counted || moved / counted < 0.5) return;       /* nothing moving to judge by */
	nr.check_said = now;
	say("warp check: moved %.1f px on average, %.0f%% come into view; difference from the frame "
	    "NR had %.2f following the vectors, %.2f staying put\n", moved / counted,
	    100.0 * entered / pixels, followed / (3.0 * counted), stayed / (3.0 * counted));
}

static DWORD WINAPI worker(LPVOID unused)
{
	(void)unused;
	for (;;) {
		WaitForSingleObject(nr.wake, 1000);
		read_settings(0);
		AcquireSRWLockExclusive(&nr.lock);
		int mine = nr.capture == 1;
		if (mine) nr.capture = 2;
		uint32_t sequence = nr.capture_sequence, epoch = nr.capture_epoch;
		float white = nr.capture_white;
		float jitter[2] = { nr.capture_jitter[0], nr.capture_jitter[1] };
		int check = nr.capture_check;
		uint32_t check_anchor = nr.check_anchor;
		float check_shift[2] = { nr.check_shift[0], nr.check_shift[1] };
		ReleaseSRWLockExclusive(&nr.lock);
		if (!mine) continue;

		LARGE_INTEGER t0, t1, f;
		QueryPerformanceCounter(&t0);
		int ok = wait_for(sequence, 2000);
		if (ok) {
			read_frame(white);
			measure_motion(check && nr.previous_sequence && check_anchor == nr.previous_sequence,
				       check_shift);
			ok = exchange(nr.frame, config.motion_history ? nr.offsets : NULL, nr.answer,
				      nr.width, nr.height) == 0;
			/* The next frame's motion leads to this one only if the daemon has it too. */
			nr.previous_sequence = ok ? sequence : 0;
			if (ok) memcpy(nr.previous, nr.frame, nr.frame_bytes);
			if (!ok && nr.failed++ % 20 == 0)
				say("no answer from the daemon at %s (%u so far); the colour goes to XeSS as it is\n",
				    config.pipe, nr.failed);
		}
		if (ok) {
			publish(epoch, sequence, jitter);
			if (answers_dir) write_answer(sequence);
			if (++nr.answered == 30 && config.dump) dump_frames();
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&f);
			nr.window_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart;
			nr.window_frames++;
			uint64_t now = GetTickCount64();
			if (!nr.window_start) nr.window_start = now;
			if (now - nr.window_start >= 5000) {
				say("NR: %llu frames in %.1f s, %.0f ms each from capture to change\n",
				    nr.window_frames, (now - nr.window_start) / 1000.0,
				    nr.window_ms / (double)nr.window_frames);
				nr.window_start = now;
				nr.window_frames = 0;
				nr.window_ms = 0;
			}
		}
		AcquireSRWLockExclusive(&nr.lock);
		nr.capture = 0;
		ReleaseSRWLockExclusive(&nr.lock);
		SetEvent(nr.idle);
	}
}

/* ---- GPU objects ---- */

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags,
				   D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES properties = { .Type = heap };
	D3D12_RESOURCE_DESC desc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = size, .Height = 1,
		.DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_UNKNOWN,
		.SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = flags,
	};
	ID3D12Resource *resource = NULL;
	if (FAILED(ID3D12Device_CreateCommittedResource(nr.device, &properties, D3D12_HEAP_FLAG_NONE,
			&desc, state, NULL, &IID_ID3D12Resource, (void **)&resource)))
		return NULL;
	return resource;
}

static void drop(ID3D12Resource **resource)
{
	if (*resource) ID3D12Resource_Release(*resource);
	*resource = NULL;
}

static void release_sized(void)
{
	drop(&nr.readback);
	drop(&nr.staging);
	drop(&nr.output);
	drop(&nr.mixed);
	for (int k = 0; k < CHANGES; k++) {
		drop(&nr.change[k]);
		nr.change_map[k] = NULL;
		nr.change_used[k] = 0;
	}
	for (int t = 0; t < TRACKERS; t++) {
		drop(&nr.tracker[t].buffer[0]);
		drop(&nr.tracker[t].buffer[1]);
		nr.tracker[t].live = 0;
	}
	drop(&nr.check_readback);
	drop(&nr.claims);
	drop(&nr.matched);
	drop(&nr.sums);
	free(nr.frame);
	free(nr.answer);
	free(nr.previous);
	free(nr.offsets);
	nr.frame = nr.answer = nr.previous = NULL;
	nr.offsets = NULL;
	nr.previous_sequence = 0;
	nr.width = nr.height = 0;
}

static void release_motion(void)
{
	drop(&nr.motion[0]);
	drop(&nr.motion[1]);
	nr.motion_width = nr.motion_height = 0;
	nr.motion_previous = 0;
}

static int build_device(ID3D12Device *device)
{
	ID3D12PipelineState **pipelines[9] = { &nr.clear_pipeline, &nr.claim_pipeline, &nr.track_pipeline,
						&nr.match_pipeline, &nr.reduce_pipeline, &nr.pipeline,
						&nr.encode_pipeline, &nr.motion_pipeline, &nr.compose_pipeline };
	for (int i = 0; i < 9; i++)
		if (*pipelines[i]) {
			ID3D12PipelineState_Release(*pipelines[i]);
			*pipelines[i] = NULL;
		}
	if (nr.signature) ID3D12RootSignature_Release(nr.signature);
	if (nr.marker) ID3D12Resource_Release(nr.marker);
	drop(&nr.srgb_table);
	drop(&nr.linear_table);
	if (nr.device) ID3D12Device_Release(nr.device);
	release_motion();
	nr.signature = NULL;
	nr.marker = NULL;
	nr.device = device;
	ID3D12Device_AddRef(device);
	/* Every pass carries the same root signature; any one's will do. */
	if (FAILED(ID3D12Device_CreateRootSignature(device, 0, nr_xess_apply, sizeof nr_xess_apply,
			&IID_ID3D12RootSignature, (void **)&nr.signature))) {
		say("the compute passes' root signature was refused\n");
		return -1;
	}
	const void *code[9] = { nr_xess_clear, nr_xess_claim, nr_xess_track, nr_xess_match,
				nr_xess_reduce, nr_xess_apply, nr_xess_encode, nr_xess_motion, nr_xess_compose };
	SIZE_T sizes[9] = { sizeof nr_xess_clear, sizeof nr_xess_claim, sizeof nr_xess_track,
			    sizeof nr_xess_match, sizeof nr_xess_reduce, sizeof nr_xess_apply,
			    sizeof nr_xess_encode, sizeof nr_xess_motion, sizeof nr_xess_compose };
	for (int i = 0; i < 9; i++) {
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {
			.pRootSignature = nr.signature, .CS = { code[i], sizes[i] },
		};
		if (FAILED(ID3D12Device_CreateComputePipelineState(device, &desc, &IID_ID3D12PipelineState,
				(void **)pipelines[i]))) {
			say("the compute passes' pipelines were refused\n");
			return -1;
		}
	}
	/* The CPU's encode and decode tables, once, where the passes read them. */
	nr.srgb_table = make_buffer(D3D12_HEAP_TYPE_UPLOAD, (SRGB_STEPS + 4) & ~3u, D3D12_RESOURCE_FLAG_NONE,
				    D3D12_RESOURCE_STATE_GENERIC_READ);
	nr.linear_table = make_buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof linear8, D3D12_RESOURCE_FLAG_NONE,
				      D3D12_RESOURCE_STATE_GENERIC_READ);
	void *mapped = NULL;
	D3D12_RANGE none = { 0, 0 };
	if (!nr.srgb_table || FAILED(ID3D12Resource_Map(nr.srgb_table, 0, &none, &mapped))) return -1;
	memcpy(mapped, srgb8, sizeof srgb8);
	ID3D12Resource_Unmap(nr.srgb_table, 0, NULL);
	if (!nr.linear_table || FAILED(ID3D12Resource_Map(nr.linear_table, 0, &none, &mapped))) return -1;
	memcpy(mapped, linear8, sizeof linear8);
	ID3D12Resource_Unmap(nr.linear_table, 0, NULL);
	nr.marker = make_buffer(D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE,
				D3D12_RESOURCE_STATE_COPY_DEST);
	nr.sequence = 0;
	return nr.marker ? 0 : -1;
}

/* What both modes need for a colour: it as a buffer for the passes, their result as one, and
 * the texture XeSS reads instead of the game's. */
static int build_colour(UINT width, UINT height, DXGI_FORMAT format, int kind, UINT texel)
{
	D3D12_RESOURCE_DESC desc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = width, .Height = height,
		.DepthOrArraySize = 1, .MipLevels = 1, .Format = format, .SampleDesc = { 1, 0 },
		.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN, .Flags = D3D12_RESOURCE_FLAG_NONE,
	};
	UINT rows;
	UINT64 row_bytes;
	ID3D12Device_GetCopyableFootprints(nr.device, &desc, 0, 1, 0, &nr.footprint, &rows,
					   &row_bytes, &nr.bytes);
	D3D12_HEAP_PROPERTIES device_local = { .Type = D3D12_HEAP_TYPE_DEFAULT };
	if (FAILED(ID3D12Device_CreateCommittedResource(nr.device, &device_local, D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
			(void **)&nr.mixed)))
		return -1;
	nr.mixed_state = D3D12_RESOURCE_STATE_COPY_DEST;
	nr.staging = make_buffer(D3D12_HEAP_TYPE_DEFAULT, nr.bytes, D3D12_RESOURCE_FLAG_NONE,
				 D3D12_RESOURCE_STATE_COMMON);
	nr.output = make_buffer(D3D12_HEAP_TYPE_DEFAULT, nr.bytes,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
	if (!nr.staging || !nr.output) return -1;
	nr.width = width;
	nr.height = height;
	nr.format = format;
	nr.kind = kind;
	nr.texel = texel;
	return 0;
}

static int build_sized(UINT width, UINT height, DXGI_FORMAT format, int kind, UINT texel)
{
	if (build_colour(width, height, format, kind, texel) != 0) return -1;
	nr.width = nr.height = 0;               /* until the rest is made too */
	nr.readback = make_buffer(D3D12_HEAP_TYPE_READBACK, nr.bytes, D3D12_RESOURCE_FLAG_NONE,
				  D3D12_RESOURCE_STATE_COPY_DEST);
	if (!nr.readback) return -1;
	nr.cells_width = (width + CELL - 1) / CELL;
	nr.cells_height = (height + CELL - 1) / CELL;
	nr.claims = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)width * height * 16,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
	nr.matched = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)width * height * 28,
				 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
	nr.sums = make_buffer(D3D12_HEAP_TYPE_DEFAULT, (UINT64)nr.cells_width * nr.cells_height * 36,
			      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
	if (!nr.claims || !nr.matched || !nr.sums) return -1;
	for (int k = 0; k < CHANGES; k++) {
		nr.change[k] = make_buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)width * height * 16,
					   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
		D3D12_RANGE none = { 0, 0 };
		if (!nr.change[k] || FAILED(ID3D12Resource_Map(nr.change[k], 0, &none,
						(void **)&nr.change_map[k])))
			return -1;
		nr.change_used[k] = nr.sequence;
	}
	for (int t = 0; t < TRACKERS; t++)
		for (int b = 0; b < 2; b++)
			if (!(nr.tracker[t].buffer[b] = make_buffer(D3D12_HEAP_TYPE_DEFAULT,
					(UINT64)width * height * 8, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					D3D12_RESOURCE_STATE_COMMON)))
				return -1;
	nr.check_readback = make_buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)width * height * 8,
					D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	nr.frame_bytes = (size_t)width * height * 4;
	nr.frame = malloc(nr.frame_bytes);
	nr.answer = malloc(nr.frame_bytes);
	nr.previous = malloc(nr.frame_bytes);
	nr.offsets = malloc(nr.frame_bytes * 2);
	if (!nr.check_readback || !nr.frame || !nr.answer || !nr.previous || !nr.offsets) return -1;
	nr.width = width;
	nr.height = height;
	nr.format = format;
	nr.kind = kind;
	nr.texel = texel;
	return 0;
}

/* A new device or a new colour: the worker is let finish, the GPU is let finish with the
 * old buffers, and everything sized to the old colour goes. Rare — a resolution or
 * quality change — so the game waits here rather than the code getting clever. */
static int rebuild(ID3D12Device *device, UINT width, UINT height, DXGI_FORMAT format, int kind,
		   UINT texel)
{
	for (ULONGLONG until = GetTickCount64() + 5000; GetTickCount64() < until; ) {
		AcquireSRWLockExclusive(&nr.lock);
		int busy = nr.capture == 2;
		if (!busy) {
			nr.capture = 0;
			nr.ready = nr.fading = nr.applied = -1;
			nr.epoch++;
		}
		ReleaseSRWLockExclusive(&nr.lock);
		if (!busy) break;
		Sleep(1);
	}
	if (nr.marker && !wait_for(nr.sequence, 1000))
		say("the GPU had not finished with the old buffers after 1 s; going on\n");
	release_sized();
	if (device != nr.device && build_device(device) != 0) return -1;
	if (build_sized(width, height, format, kind, texel) != 0) {
		say("could not make the buffers for a %ux%u colour\n", width, height);
		return -1;
	}
	if (!nr.thread) {
		nr.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
		nr.idle = CreateEventW(NULL, FALSE, FALSE, NULL);
		nr.thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
		if (!nr.wake || !nr.idle || !nr.thread) return -1;
	}
	return 0;
}

static D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
					 D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = 0;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	return barrier;
}

/* ---- Recording: what the game hands XeSS, frame after frame, for replay_nr_xess ----
 *
 * The colour before any change of ours and the motion vectors, each frame's jitter and
 * exposure, and what XeSS was told at init: enough to run the same frames through the daemon
 * and Intel's XeSS again without the game, as often as a change to this file needs. A
 * writer thread puts each frame on disk once the GPU has copied it; when it falls behind,
 * the game waits for it, so no frame is missing from a recording. */

#define RECORD_SLOTS 8

static struct {
	int left;                    /* frames still to record */
	uint32_t index;              /* frames recorded so far */
	wchar_t dir[MAX_PATH];
	HANDLE wake, thread;
	UINT width, height, motion_width, motion_height;
	ID3D12Resource *colour[RECORD_SLOTS], *motion[RECORD_SLOTS];
	struct {
		volatile LONG full;
		uint32_t sequence, index, reset;
		float jitter[2], exposure;
	} slot[RECORD_SLOTS];
	uint32_t next;
} rec;

static void record_line(const wchar_t *name, const char *line)
{
	wchar_t path[MAX_PATH + 32];
	_snwprintf(path, MAX_PATH + 32, L"%ls\\%ls", rec.dir, name);
	HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) return;
	DWORD written;
	WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
	CloseHandle(file);
}

/* One frame's file: the colour's rows, then the vectors' rows, each packed tight. */
static void record_write(int s)
{
	wchar_t path[MAX_PATH + 32];
	_snwprintf(path, MAX_PATH + 32, L"%ls\\%04u.bin", rec.dir, rec.slot[s].index);
	HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) return;
	ID3D12Resource *parts[2] = { rec.colour[s], rec.motion[s] };
	const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layout[2] = { &nr.footprint, &nr.motion_footprint };
	UINT rows[2] = { rec.height, rec.motion_height };
	UINT row_bytes[2] = { rec.width * nr.texel, rec.motion_width * 4 };
	for (int i = 0; i < 2; i++) {
		const uint8_t *mapped = NULL;
		/* To the last row's last texel: the colour's buffer ends there, short of a whole
		 * pitch, and a range past its end is refused. */
		D3D12_RANGE all = { 0, (SIZE_T)(layout[i]->Offset + (UINT64)(rows[i] - 1) * layout[i]->Footprint.RowPitch
						+ row_bytes[i]) };
		D3D12_RANGE none = { 0, 0 };
		if (FAILED(ID3D12Resource_Map(parts[i], 0, &all, (void **)&mapped))) {
			say("the recording could not read frame %u's %s back\n", rec.slot[s].index,
			    i ? "vectors" : "colour");
			continue;
		}
		for (UINT y = 0; y < rows[i]; y++) {
			DWORD written;
			WriteFile(file, mapped + layout[i]->Offset + (size_t)y * layout[i]->Footprint.RowPitch,
				  row_bytes[i], &written, NULL);
		}
		ID3D12Resource_Unmap(parts[i], 0, &none);
	}
	CloseHandle(file);
	char line[160];
	snprintf(line, sizeof line, "%u %u %.6f %.6f %.6f %u\n", rec.slot[s].index, rec.slot[s].sequence,
		 rec.slot[s].jitter[0], rec.slot[s].jitter[1], rec.slot[s].exposure, rec.slot[s].reset);
	record_line(L"frames.txt", line);
}

static DWORD WINAPI recorder(LPVOID unused)
{
	(void)unused;
	for (uint32_t next = 0;;) {
		WaitForSingleObject(rec.wake, 1000);
		for (int s = next % RECORD_SLOTS; rec.slot[s].full; s = next % RECORD_SLOTS) {
			if (wait_for(rec.slot[s].sequence, 5000)) record_write(s);
			InterlockedExchange(&rec.slot[s].full, 0);
			next++;
		}
	}
}

/* A recording asked for: buffers for this colour and these vectors, a fresh folder's
 * description of them, and the writer started. 0 when it cannot be. */
static int record_start(int frames, UINT motion_width, UINT motion_height)
{
	for (int s = 0; s < RECORD_SLOTS; s++)          /* the last recording's writes are done */
		for (int waited = 0; rec.slot[s].full && waited < 5000; waited++) Sleep(1);
	if (rec.width != nr.width || rec.height != nr.height || rec.motion_width != motion_width
	    || rec.motion_height != motion_height) {
		for (int s = 0; s < RECORD_SLOTS; s++) {
			drop(&rec.colour[s]);
			drop(&rec.motion[s]);
			rec.colour[s] = make_buffer(D3D12_HEAP_TYPE_READBACK, nr.bytes, D3D12_RESOURCE_FLAG_NONE,
						    D3D12_RESOURCE_STATE_COPY_DEST);
			rec.motion[s] = make_buffer(D3D12_HEAP_TYPE_READBACK,
				nr.motion_footprint.Offset + (UINT64)motion_height * nr.motion_footprint.Footprint.RowPitch,
				D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
			if (!rec.colour[s] || !rec.motion[s]) {
				rec.width = 0;
				return 0;
			}
		}
		rec.width = nr.width;
		rec.height = nr.height;
		rec.motion_width = motion_width;
		rec.motion_height = motion_height;
	}
	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t base[MAX_PATH];
	MultiByteToWideChar(CP_UTF8, 0, config.record_dir, -1, base, MAX_PATH);
	_snwprintf(rec.dir, MAX_PATH, L"%ls\\%04u%02u%02u-%02u%02u%02u", base, t.wYear, t.wMonth, t.wDay,
		   t.wHour, t.wMinute, t.wSecond);
	CreateDirectoryW(base, NULL);
	if (!CreateDirectoryW(rec.dir, NULL)) return 0;
	char line[512];
	snprintf(line, sizeof line, "colour %u %u %d %u\nmotion %u %u\noutput %u %u\nquality %d\n"
		 "flags %u\nvelocity_scale %.6f %.6f\njitter_scale %.6f %.6f\nwhite %.6f\n",
		 nr.width, nr.height, (int)nr.format, nr.texel, motion_width, motion_height, xess.output.x,
		 xess.output.y, xess.quality, xess.flags, xess.velocity[0], xess.velocity[1],
		 xess.jitter[0], xess.jitter[1], config.white);
	record_line(L"meta.txt", line);
	if (!rec.thread) {
		rec.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
		rec.thread = CreateThread(NULL, 0, recorder, NULL, 0, NULL);
		if (!rec.wake || !rec.thread) return 0;
	}
	rec.left = frames;
	rec.index = 0;
	say("recording %d frames into %ls\n", frames, rec.dir);
	return 1;
}

/* ---- The hook ---- */

struct batch {
	D3D12_RESOURCE_BARRIER barrier[24];
	UINT count;
};

static void add(struct batch *b, ID3D12Resource *resource, D3D12_RESOURCE_STATES from,
		D3D12_RESOURCE_STATES to)
{
	b->barrier[b->count++] = transition(resource, from, to);
}

static void flush(ID3D12GraphicsCommandList *list, struct batch *b)
{
	if (b->count) ID3D12GraphicsCommandList_ResourceBarrier(list, b->count, b->barrier);
	b->count = 0;
}

/* The region of the motion texture XeSS reads: the output's size with high-resolution
 * vectors, the colour's otherwise. 0 when the vectors cannot be followed here. */
static int motion_region(const xess_d3d12_execute_params_t *p, UINT width, UINT height,
			 UINT *motion_width, UINT *motion_height)
{
	static int told;
	if (!p->pVelocityTexture) return 0;
	D3D12_RESOURCE_DESC desc;
	ID3D12Resource_GetDesc(p->pVelocityTexture, &desc);
	UINT w = width, h = height;
	if (xess.flags & XESS_INIT_FLAG_HIGH_RES_MV) {
		w = xess.output.x;
		h = xess.output.y;
	}
	if (desc.Format != DXGI_FORMAT_R16G16_FLOAT || !w || !h
	    || (UINT64)p->inputMotionVectorBase.x + w > desc.Width
	    || (UINT64)p->inputMotionVectorBase.y + h > desc.Height) {
		if (!told++)
			say("motion vectors format %d, %ux%u at (%u, %u) in %llux%u: not followed (mode=carry: "
			    "NR's change stays where it was captured; mode=cut: the daemon's history stays "
			    "where it was)\n", desc.Format, w, h,
			    p->inputMotionVectorBase.x, p->inputMotionVectorBase.y, desc.Width, desc.Height);
		return 0;
	}
	*motion_width = w;
	*motion_height = h;
	return 1;
}

/* Two buffers for the vectors, sized to their region; the trackers start over with them. */
static int ensure_motion(UINT width, UINT height)
{
	if (width == nr.motion_width && height == nr.motion_height) return 0;
	if (nr.marker && !wait_for(nr.sequence, 1000))
		say("the GPU had not finished with the old motion buffers after 1 s; going on\n");
	release_motion();
	for (int t = 0; t < TRACKERS; t++) nr.tracker[t].live = 0;
	D3D12_RESOURCE_DESC desc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = width, .Height = height,
		.DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_R16G16_FLOAT,
		.SampleDesc = { 1, 0 },
	};
	UINT rows;
	UINT64 row_bytes, bytes;
	ID3D12Device_GetCopyableFootprints(nr.device, &desc, 0, 1, 0, &nr.motion_footprint, &rows,
					   &row_bytes, &bytes);
	for (int i = 0; i < 2; i++)
		if (!(nr.motion[i] = make_buffer(D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_FLAG_NONE,
						 D3D12_RESOURCE_STATE_COMMON))) {
			release_motion();
			return -1;
		}
	nr.motion_width = width;
	nr.motion_height = height;
	say("following motion vectors %ux%u; velocity scale (%g, %g), jitter scale (%g, %g)\n",
	    width, height, xess.velocity[0], xess.velocity[1], xess.jitter[0], xess.jitter[1]);
	return 0;
}

/* The five buffers a pass reads, t0-t4 in nr_xess.hlsl, and the one it writes, then the
 * pass over an extent in 8x8 groups. A pass that reads fewer is given a readable buffer for
 * each of the rest. */
static void bind_extent(ID3D12GraphicsCommandList *list, const struct constants *k,
			ID3D12Resource *const read[5], ID3D12Resource *written, UINT width, UINT height)
{
	ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, sizeof *k / 4, k, 0);
	for (UINT i = 0; i < 5; i++)
		ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(list, 1 + i,
			ID3D12Resource_GetGPUVirtualAddress(read[i]));
	ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 6,
		ID3D12Resource_GetGPUVirtualAddress(written));
	ID3D12GraphicsCommandList_Dispatch(list, (width + 7) / 8, (height + 7) / 8, 1);
}

/* ... over the colour's extent. */
static void bind(ID3D12GraphicsCommandList *list, const struct constants *k,
		 ID3D12Resource *const read[5], ID3D12Resource *written)
{
	bind_extent(list, k, read, written, k->width, k->height);
}

/* Records the capture, the trackers and the change on the game's list, ahead of XeSS.
 * Returns the texture XeSS should read instead of the game's colour, or NULL to leave it
 * alone.
 *
 * The colour and the motion vectors are in NON_PIXEL_SHADER_RESOURCE, as XeSS requires,
 * and go back to it. Our buffers start and end every execute in COMMON, so their states
 * never depend on where one command list ends and the next begins. No descriptor heap is
 * touched; the compute root signature and pipeline are, and XeSS sets its own straight
 * after. */
static ID3D12Resource *record(ID3D12GraphicsCommandList *list, const xess_d3d12_execute_params_t *p)
{
	D3D12_RESOURCE_DESC desc;
	ID3D12Resource_GetDesc(p->pColorTexture, &desc);
	UINT width = p->inputWidth ? p->inputWidth : (UINT)desc.Width;
	UINT height = p->inputHeight ? p->inputHeight : desc.Height;
	UINT texel;
	int kind = kind_of(desc.Format, &texel);
	static int told;
	if (kind < 0 || (UINT64)p->inputColorBase.x + width > desc.Width
	    || (UINT64)p->inputColorBase.y + height > desc.Height) {
		if (!told++)
			say("colour format %d, %ux%u at (%u, %u) in %llux%u: not handled; XeSS gets it as it is\n",
			    desc.Format, width, height, p->inputColorBase.x, p->inputColorBase.y,
			    desc.Width, desc.Height);
		return NULL;
	}
	ID3D12Device *device = NULL;
	if (FAILED(ID3D12GraphicsCommandList_GetDevice(list, &IID_ID3D12Device, (void **)&device)))
		return NULL;
	if (device != nr.device || width != nr.width || height != nr.height || desc.Format != nr.format) {
		say("colour %ux%u format %d at (%u, %u) in a %llux%u texture; motion %s, depth %s, "
		    "exposure %g, jitter (%.3f, %.3f)\n", width, height, desc.Format,
		    p->inputColorBase.x, p->inputColorBase.y, desc.Width, desc.Height,
		    p->pVelocityTexture ? "yes" : "no", p->pDepthTexture ? "yes" : "no",
		    p->exposureScale, p->jitterOffsetX, p->jitterOffsetY);
		if (rebuild(device, width, height, desc.Format, kind, texel) != 0) {
			say("setup failed; from here on XeSS gets every frame as it is\n");
			nr.broken = 1;
		}
	}
	ID3D12Device_Release(device);
	if (nr.broken) return NULL;
	ID3D12GraphicsCommandList2 *list2 = NULL;
	if (FAILED(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList2,
			(void **)&list2))) {
		say("the command list has no WriteBufferImmediate; XeSS gets every frame as it is\n");
		nr.broken = 1;
		return NULL;
	}
	UINT motion_width = 0, motion_height = 0;
	int warp = config.warp && motion_region(p, width, height, &motion_width, &motion_height)
		   && ensure_motion(motion_width, motion_height) == 0;
	float jitter[2] = { p->jitterOffsetX * xess.jitter[0], p->jitterOffsetY * xess.jitter[1] };
	int recording = 0, slot = 0;
	LONG asked = InterlockedExchange(&record_asked, 0);
	if (asked > 0 && !(warp && record_start((int)asked, motion_width, motion_height)))
		say("no recording: %s\n", warp ? "its buffers or folder could not be made"
					      : "this frame has no motion vectors to record");
	if (rec.left > 0 && warp && rec.width == width && rec.height == height
	    && rec.motion_width == motion_width && rec.motion_height == motion_height) {
		slot = (int)(rec.next % RECORD_SLOTS);
		for (ULONGLONG until = GetTickCount64() + 2000; rec.slot[slot].full && GetTickCount64() < until; )
			Sleep(1);
		recording = !rec.slot[slot].full;
		if (!recording) {
			say("the recording's writer fell 2 s behind; stopped at %u frames\n", rec.index);
			rec.left = 0;
		}
	}

	/* `sync`: the worker has the frame before (the game submitted it after its execute
	 * returned); its answer is published before the worker lets go of it, so once it does,
	 * this frame takes a change one frame old and is captured itself. A second without an
	 * answer — a game that submits late, or no daemon — and the frame goes on as it would. */
	if (config.sync) {
		ULONGLONG until = GetTickCount64() + 1000;
		for (;;) {
			AcquireSRWLockExclusive(&nr.lock);
			int busy = nr.capture != 0;
			ReleaseSRWLockExclusive(&nr.lock);
			ULONGLONG now = GetTickCount64();
			if (!busy) break;
			if (now >= until) {
				static ULONGLONG said;
				if (!said || now - said >= 5000) {
					said = now;
					say("sync: no answer to the frame before after 1 s; this one goes on without\n");
				}
				break;
			}
			WaitForSingleObject(nr.idle, (DWORD)(until - now));
		}
	}

	float white = config.white;
	if (p->exposureScale > 0) white /= p->exposureScale;   /* XeSS multiplies the colour by it */
	uint32_t sequence = ++nr.sequence;
	AcquireSRWLockExclusive(&nr.lock);
	if (p->resetHistory) {                  /* a cut: the change belongs to the old shot */
		nr.ready = -1;
		nr.epoch++;
		nr.rising = 1;
	}
	uint32_t in_flight = nr.capture ? nr.capture_sequence : 0;
	int capture = nr.capture == 0;
	if (capture) {
		nr.capture = 3;                 /* the worker's to take once all of it is recorded */
		nr.capture_sequence = sequence;
		nr.capture_epoch = nr.epoch;
		nr.capture_white = white;
		nr.capture_jitter[0] = jitter[0];
		nr.capture_jitter[1] = jitter[1];
	}
	int change = nr.ready;
	/* A new change: the one applied till now gives way to it over as many frames as a change
	 * takes to come, so NR's detail turns from one answer into the next instead of jumping
	 * a few times a second — the network does not draw the same detail twice. */
	if (change != nr.applied) {
		nr.fading = change >= 0 ? nr.applied : -1;
		if (change >= 0 && nr.applied >= 0) {
			nr.fade_start = sequence;
			if (nr.arrived)
				nr.period = 0.75f * nr.period + 0.25f * (float)(sequence - nr.arrived);
		}
		if (change >= 0 && nr.applied < 0) nr.rise_start = sequence;
		nr.applied = change;
		nr.arrived = sequence;
	}
	float fade = 1;
	if (nr.fading >= 0) {
		fade = (float)(sequence - nr.fade_start + 1) / (nr.period > 1 ? nr.period : 1);
		if (fade >= 1) {
			nr.fading = -1;
			fade = 1;
		}
	}
	/* The first change after a cut comes in over a period from nothing, as later ones turn
	 * into each other: a few frames of the plain shot, then NR's look all at once, a few times
	 * a cutscene, read as the picture jumping. */
	float rise = 1;
	if (nr.rising && change >= 0) {
		rise = (float)(sequence - nr.rise_start + 1) / (nr.period > 1 ? nr.period : 1);
		if (rise >= 1) {
			nr.rising = 0;
			rise = 1;
		}
	}
	int old = nr.fading;
	uint32_t anchor = 0, old_anchor = 0;
	float shift[2] = { 0, 0 }, old_shift[2] = { 0, 0 };
	if (change >= 0) {
		nr.change_used[change] = sequence;
		anchor = nr.change_anchor[change];
		shift[0] = nr.change_jitter[change][0] - jitter[0];
		shift[1] = nr.change_jitter[change][1] - jitter[1];
	}
	if (old >= 0) {
		nr.change_used[old] = sequence;
		old_anchor = nr.change_anchor[old];
		old_shift[0] = nr.change_jitter[old][0] - jitter[0];
		old_shift[1] = nr.change_jitter[old][1] - jitter[1];
	}
	ReleaseSRWLockExclusive(&nr.lock);

	/* The trackers: one follows the frame the newest change came from, one the frame of the
	 * change fading out, one the frame NR has now, and a capture starts one afresh. Any other
	 * is done with. */
	int updates[TRACKERS], update_count = 0, fresh = -1, use = -1, use_old = -1;
	for (int t = 0; t < TRACKERS; t++) {
		int wanted = (change >= 0 && nr.tracker[t].anchor == anchor)
			     || (old >= 0 && nr.tracker[t].anchor == old_anchor)
			     || nr.tracker[t].anchor == in_flight;
		if (!warp || p->resetHistory || !wanted) nr.tracker[t].live = 0;
		if (!nr.tracker[t].live) continue;
		updates[update_count++] = t;
		if (change >= 0 && nr.tracker[t].anchor == anchor) use = t;
		if (old >= 0 && nr.tracker[t].anchor == old_anchor) use_old = t;
	}
	if (warp && capture)
		for (int t = 0; t < TRACKERS && fresh < 0; t++)
			if (!nr.tracker[t].live) fresh = t;
	ID3D12Resource *motion_now = warp ? nr.motion[nr.motion_next] : NULL;
	ID3D12Resource *motion_before = warp && nr.motion_previous && !p->resetHistory
					? nr.motion[1 - nr.motion_next] : NULL;
	int apply = change >= 0;
	int jobs = update_count + (fresh >= 0);
	int claiming = warp && update_count > 0;      /* CLAIM, for the trackers TRACK carries on */

	ID3D12GraphicsCommandList *l = list;
	struct batch b = { .count = 0 };
	const D3D12_RESOURCE_STATES common = D3D12_RESOURCE_STATE_COMMON,
		read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		source = D3D12_RESOURCE_STATE_COPY_SOURCE, dest = D3D12_RESOURCE_STATE_COPY_DEST,
		write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	/* the copies */
	if (capture || apply || recording) add(&b, p->pColorTexture, read, source);
	if (apply) add(&b, nr.staging, common, dest);
	if (warp) {
		add(&b, p->pVelocityTexture, read, source);
		add(&b, motion_now, common, dest);
	}
	flush(l, &b);
	D3D12_BOX box = { p->inputColorBase.x, p->inputColorBase.y, 0,
			  p->inputColorBase.x + width, p->inputColorBase.y + height, 1 };
	D3D12_TEXTURE_COPY_LOCATION colour = { .pResource = p->pColorTexture,
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
	D3D12_TEXTURE_COPY_LOCATION into = { .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
		.PlacedFootprint = nr.footprint };
	if (capture) {
		into.pResource = nr.readback;
		ID3D12GraphicsCommandList_CopyTextureRegion(l, &into, 0, 0, 0, &colour, &box);
	}
	if (apply) {
		into.pResource = nr.staging;
		ID3D12GraphicsCommandList_CopyTextureRegion(l, &into, 0, 0, 0, &colour, &box);
	}
	if (warp) {
		D3D12_BOX region = { p->inputMotionVectorBase.x, p->inputMotionVectorBase.y, 0,
				     p->inputMotionVectorBase.x + motion_width,
				     p->inputMotionVectorBase.y + motion_height, 1 };
		D3D12_TEXTURE_COPY_LOCATION vectors = { .pResource = p->pVelocityTexture,
			.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
		D3D12_TEXTURE_COPY_LOCATION to = { .pResource = motion_now,
			.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = nr.motion_footprint };
		ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &vectors, &region);
		if (recording) {
			into.pResource = rec.colour[slot];
			ID3D12GraphicsCommandList_CopyTextureRegion(l, &into, 0, 0, 0, &colour, &box);
			to.pResource = rec.motion[slot];
			ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &vectors, &region);
		}
	}

	/* what the passes read, readable; what they write, writable */
	if (capture || apply || recording) add(&b, p->pColorTexture, source, read);
	if (warp) {
		add(&b, p->pVelocityTexture, source, read);
		add(&b, motion_now, dest, read);
		if (motion_before) add(&b, motion_before, common, read);
	}
	for (int i = 0; i < update_count; i++) {
		int t = updates[i], c = nr.tracker[t].current;
		add(&b, nr.tracker[t].buffer[c], common, read);
		add(&b, nr.tracker[t].buffer[1 - c], common, write);
	}
	if (fresh >= 0) add(&b, nr.tracker[fresh].buffer[1 - nr.tracker[fresh].current], common, write);
	if (claiming) add(&b, nr.claims, common, write);
	if (apply) {
		add(&b, nr.staging, dest, read);
		add(&b, nr.matched, common, write);
		add(&b, nr.sums, common, write);
		add(&b, nr.output, common, write);
		if (nr.mixed_state != dest) add(&b, nr.mixed, nr.mixed_state, dest);
	}
	flush(l, &b);

	if (jobs || apply) ID3D12GraphicsCommandList_SetComputeRootSignature(l, nr.signature);
	if (jobs) {
		struct constants k = {
			.width = width, .height = height,
			.motion_pitch = nr.motion_footprint.Footprint.RowPitch,
			.motion_size = { motion_width, motion_height },
			.motion_scale = { xess.velocity[0] * width / motion_width,
					  xess.velocity[1] * height / motion_height },
			.motion_texels = { (float)motion_width / width, (float)motion_height / height },
			.jitter = { jitter[0], jitter[1] },
			.previous_jitter = { nr.previous_jitter[0], nr.previous_jitter[1] },
			.vectors = (uint32_t)claiming | (claiming && motion_before ? 2u : 0u),
		};
		if (claiming) {
			/* CLEAR, then CLAIM: which surfaces came from each of the last frame's pixels */
			ID3D12Resource *vectors[5] = { motion_now, motion_now, motion_now, motion_now, motion_now };
			ID3D12GraphicsCommandList_SetPipelineState(l, nr.clear_pipeline);
			bind(l, &k, vectors, nr.claims);
			D3D12_RESOURCE_BARRIER cleared = { .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
							   .UAV = { .pResource = nr.claims } };
			ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &cleared);
			ID3D12GraphicsCommandList_SetPipelineState(l, nr.claim_pipeline);
			bind(l, &k, vectors, nr.claims);
			add(&b, nr.claims, write, read);
			flush(l, &b);
		}
		ID3D12GraphicsCommandList_SetPipelineState(l, nr.track_pipeline);
		for (int i = 0; i < update_count; i++) {
			int t = updates[i], c = nr.tracker[t].current;
			ID3D12Resource *read_by[5] = { motion_now, nr.tracker[t].buffer[c], nr.claims,
						       motion_before ? motion_before : motion_now, motion_now };
			bind(l, &k, read_by, nr.tracker[t].buffer[1 - c]);
		}
		if (fresh >= 0) {
			ID3D12Resource *read_by[5] = { motion_now, motion_now, motion_now, motion_now,
						       motion_now };
			k.reset = 1;
			bind(l, &k, read_by, nr.tracker[fresh].buffer[1 - nr.tracker[fresh].current]);
		}
		for (int i = 0; i < update_count; i++) {
			int t = updates[i];
			add(&b, nr.tracker[t].buffer[1 - nr.tracker[t].current], write, read);
			nr.tracker[t].current ^= 1;
		}
		if (fresh >= 0) {
			add(&b, nr.tracker[fresh].buffer[1 - nr.tracker[fresh].current], write, read);
			nr.tracker[fresh].current ^= 1;
			nr.tracker[fresh].live = 1;
			nr.tracker[fresh].anchor = sequence;
		}
		flush(l, &b);
	}
	if (apply) {
		/* Through the trackers when the newest change has one, and the change fading out
		 * along with it only when that one has too; unwarped, both are read in place. */
		int warped = use >= 0;
		int fading = old >= 0 && (!warped || use_old >= 0);
		struct constants k = {
			.width = width, .height = height, .pitch = nr.footprint.Footprint.RowPitch,
			.kind = (uint32_t)nr.kind, .white = white, .strength = config.strength * rise,
			.change_pitch = width * 16, .warp = (uint32_t)warped, .shift = { shift[0], shift[1] },
			.fading = (uint32_t)fading, .old_shift = { old_shift[0], old_shift[1] },
			.fade = fade, .cell = CELL, .cells = { nr.cells_width, nr.cells_height },
			.match_from = config.match_from, .match_to = config.match_to,
			.jitter = { jitter[0], jitter[1] }, .vectors = (uint32_t)warp,
		};
		if (warp) {             /* REDUCE and APPLY fill by the vectors too */
			k.motion_pitch = nr.motion_footprint.Footprint.RowPitch;
			k.motion_size[0] = motion_width;
			k.motion_size[1] = motion_height;
			k.motion_scale[0] = xess.velocity[0] * width / motion_width;
			k.motion_scale[1] = xess.velocity[1] * height / motion_height;
			k.motion_texels[0] = (float)motion_width / width;
			k.motion_texels[1] = (float)motion_height / height;
		}
		/* MATCH: each change where its tracker says, and how far to trust it */
		ID3D12Resource *matching[5] = {
			nr.staging, nr.change[change],
			warped ? nr.tracker[use].buffer[nr.tracker[use].current] : nr.staging,
			fading ? nr.change[old] : nr.change[change],
			fading && warped ? nr.tracker[use_old].buffer[nr.tracker[use_old].current] : nr.staging,
		};
		ID3D12GraphicsCommandList_SetPipelineState(l, nr.match_pipeline);
		bind(l, &k, matching, nr.matched);
		add(&b, nr.matched, write, read);
		flush(l, &b);
		/* REDUCE: what was trusted, summed over cells, one thread a cell */
		ID3D12Resource *reducing[5] = { nr.matched, warp ? motion_now : nr.matched, nr.matched,
						nr.matched, nr.matched };
		ID3D12GraphicsCommandList_SetPipelineState(l, nr.reduce_pipeline);
		bind_extent(l, &k, reducing, nr.sums, nr.cells_width, nr.cells_height);
		add(&b, nr.sums, write, read);
		flush(l, &b);
		/* APPLY: the change, and the fill where it was not trusted, into the colour */
		ID3D12Resource *applying[5] = { nr.staging, nr.matched, nr.sums, warp ? motion_now : nr.staging,
						nr.staging };
		ID3D12GraphicsCommandList_SetPipelineState(l, nr.pipeline);
		bind(l, &k, applying, nr.output);
	}

	/* everything of ours back to COMMON, the result into the texture XeSS reads */
	for (int i = 0; i < update_count; i++) {
		int t = updates[i];
		add(&b, nr.tracker[t].buffer[0], read, common);
		add(&b, nr.tracker[t].buffer[1], read, common);
	}
	if (fresh >= 0) add(&b, nr.tracker[fresh].buffer[nr.tracker[fresh].current], read, common);
	if (claiming) add(&b, nr.claims, read, common);
	if (warp) {
		add(&b, motion_now, read, common);
		if (motion_before) add(&b, motion_before, read, common);
	}
	if (apply) {
		add(&b, nr.staging, read, common);
		add(&b, nr.matched, read, common);
		add(&b, nr.sums, read, common);
		add(&b, nr.output, write, source);
	}
	flush(l, &b);
	if (apply) {
		D3D12_TEXTURE_COPY_LOCATION from = { .pResource = nr.output,
			.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = nr.footprint };
		D3D12_TEXTURE_COPY_LOCATION mixed = { .pResource = nr.mixed,
			.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
		ID3D12GraphicsCommandList_CopyTextureRegion(l, &mixed, 0, 0, 0, &from, NULL);
		add(&b, nr.output, source, common);
		add(&b, nr.mixed, dest, read);
		flush(l, &b);
		nr.mixed_state = read;
	}
	int check = capture && use >= 0;
	if (check) {
		ID3D12Resource *offsets = nr.tracker[use].buffer[nr.tracker[use].current];
		add(&b, offsets, common, source);
		flush(l, &b);
		ID3D12GraphicsCommandList_CopyBufferRegion(l, nr.check_readback, 0, offsets, 0,
							   (UINT64)width * height * 8);
		add(&b, offsets, source, common);
		flush(l, &b);
	}
	if (warp) {
		nr.motion_next ^= 1;
		nr.motion_previous = 1;
	} else {
		nr.motion_previous = 0;
	}
	nr.previous_jitter[0] = jitter[0];
	nr.previous_jitter[1] = jitter[1];

	D3D12_WRITEBUFFERIMMEDIATE_PARAMETER mark = {
		ID3D12Resource_GetGPUVirtualAddress(nr.marker), sequence };
	D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
	ID3D12GraphicsCommandList2_WriteBufferImmediate(list2, 1, &mark, &mode);
	ID3D12GraphicsCommandList2_Release(list2);
	if (recording) {
		rec.slot[slot].sequence = sequence;
		rec.slot[slot].index = rec.index++;
		rec.slot[slot].jitter[0] = p->jitterOffsetX;
		rec.slot[slot].jitter[1] = p->jitterOffsetY;
		rec.slot[slot].exposure = p->exposureScale;
		rec.slot[slot].reset = p->resetHistory;
		InterlockedExchange(&rec.slot[slot].full, 1);
		rec.next++;
		if (--rec.left == 0) say("recorded %u frames into %ls\n", rec.index, rec.dir);
		SetEvent(rec.wake);
	}
	if (capture) {
		AcquireSRWLockExclusive(&nr.lock);
		nr.capture = 1;
		nr.capture_check = check;
		nr.check_anchor = anchor;
		nr.check_shift[0] = shift[0];
		nr.check_shift[1] = shift[1];
		ReleaseSRWLockExclusive(&nr.lock);
		SetEvent(nr.wake);
	}
	return apply ? nr.mixed : NULL;
}

/* ---- mode=cut: the list cut at XeSS, and NR's answer to that very frame ----
 *
 * One set of shared buffers and one fence for the session: the queue runs the halves in
 * order, so a frame's first half, which writes the input, runs only once the daemon is done
 * with the frame before (its V + 1), and the answer is read before the next frame's is
 * written. */

/* nr_xess.hlsl's cbuffer for ENCODE and COMPOSE, field for field; the rest of the 32 root
 * constants unused. */
struct cut_constants {
	uint32_t width, height, pitch, kind;
	float white, strength;
	uint32_t frame[2];
	uint32_t motion_pitch, offsets;
	uint32_t motion_size[2];
	float motion_scale[2];
	float motion_texels[2];
	float jitter[2];
	float previous_jitter[2];
	uint32_t unused[12];
};

_Static_assert(sizeof(struct cut_constants) == 32 * 4, "the root signature takes 32 constants");

/* What one frame's cut hands the daemon: its fence, held, since the session may close before
 * the worker gets to it, the frame's value V, the session it belongs to, and the V of the
 * frame its offsets lead back to, or 0 when it has none to follow. */
struct handover {
	ID3D12Fence *fence;
	uint64_t value, session, previous;
	LARGE_INTEGER queued;
};

#define HANDOVERS 16

/* The daemon's side of a control exchange (nr_gpu_rpc.h); test_nr_xess puts its own in. */
static int exchange_rpc(const struct nr_gpu_packet *packet, struct nr_gpu_reply *reply)
{
	return nr_gpu_rpc(config.pipe, packet, reply);
}

static int (*exchange_gpu)(const struct nr_gpu_packet *, struct nr_gpu_reply *) = exchange_rpc;

static struct {
	SRWLOCK lock;
	HANDLE wake, thread;
	struct nr_gpu_d3d12 gpu;     /* the buffers and fence the daemon shares, sized to the colour */
	uint64_t session;            /* the current session's id, or 0 */
	volatile LONG hello;         /* 1 the worker is to say HELLO, 2 the daemon has answered it */
	volatile LONG broken;        /* a frame went unanswered: no more cuts until a new session */
	volatile LONG releasing;     /* the worker is to send `release` */
	struct nr_gpu_packet release;
	ULONGLONG retry;             /* when a refused HELLO is tried again */
	struct handover queue[HANDOVERS];
	unsigned head, count;
	uint64_t last;               /* the newest value handed over */
	uint64_t calls, last_call;   /* executes so far, and the one that handed `last` over */
	float last_jitter[2];        /* that frame's jitter */
	int followed;                /* that frame carries offsets for the daemon's history */
	char said[160];              /* the last reason a frame went through whole */
	uint32_t answered, declined, failed;
	uint64_t window_start, window_frames;
	double window_ms;
} cut = { .lock = SRWLOCK_INIT };

static ID3D12Resource *handed;      /* what the last execute gave XeSS, for replay_nr_xess */
static int cut_now;                 /* ... and whether NR's answer reaches its frame */

static void say_line(const char *line)
{
	say("cut: %s\n", line);
}

/* Why this frame went to XeSS whole, when that changes. */
static void cut_said(const char *why)
{
	if (!strcmp(why, cut.said)) return;
	snprintf(cut.said, sizeof cut.said, "%s", why);
	say("this frame goes to XeSS without NR: %s\n", why);
}

/* The frame's queue waits on the GPU for V + 1. Where the daemon cannot give it, this does,
 * from the CPU, once the queue has reached V: earlier, the queue's own signal of V would take
 * the fence back, and the wait would never end. */
static void unblock(ID3D12Fence *fence, uint64_t value)
{
	if (ID3D12Fence_GetCompletedValue(fence) > value) return;
	HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (event && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(fence, value, event)))
		WaitForSingleObject(event, 10000);
	if (event) CloseHandle(event);
	if (ID3D12Fence_GetCompletedValue(fence) == value) ID3D12Fence_Signal(fence, value + 1);
	else if (ID3D12Fence_GetCompletedValue(fence) < value)
		say("the game's queue did not reach frame %llu's copy in 10 s; its wait is left to it\n",
		    (unsigned long long)value);
}

static void say_hello(void)
{
	struct nr_gpu_packet packet;
	AcquireSRWLockShared(&cut.lock);
	nr_gpu_d3d12_packet(&cut.gpu, NR_GPU_HELLO, 0, &packet);
	uint32_t width = cut.gpu.width, height = cut.gpu.height;
	ReleaseSRWLockShared(&cut.lock);
	struct nr_gpu_reply reply;
	int ok = exchange_gpu(&packet, &reply);
	if (ok && reply.status == NR_GPU_SUCCESS) {
		InterlockedExchange(&cut.hello, 2);
		say("the daemon at %s takes this game's %ux%u frames on the GPU (process %u)\n", config.pipe,
		    width, height, reply.detail);
		return;
	}
	cut.retry = GetTickCount64() + 5000;
	InterlockedExchange(&cut.hello, 0);
	char why[96];
	snprintf(why, sizeof why, "no daemon at %s took a D3D12 session (%s); asked again every 5 s",
		 config.pipe, ok ? "it declined, and its log says why" : "no answer");
	cut_said(why);
}

static void hand_over(struct handover *h)
{
	if (h->session != cut.session || cut.broken) {
		unblock(h->fence, h->value);
		return;
	}
	struct nr_gpu_packet packet;
	AcquireSRWLockShared(&cut.lock);
	nr_gpu_d3d12_packet(&cut.gpu, NR_GPU_FRAME, h->value, &packet);
	nr_gpu_d3d12_follow(&packet, h->previous);
	ReleaseSRWLockShared(&cut.lock);
	struct nr_gpu_reply reply;
	int ok = exchange_gpu(&packet, &reply);
	if (ok && reply.status == NR_GPU_SUCCESS) {
		cut.answered++;
		LARGE_INTEGER now, f;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&f);
		cut.window_ms += (double)(now.QuadPart - h->queued.QuadPart) * 1000 / f.QuadPart;
		cut.window_frames++;
		uint64_t tick = GetTickCount64();
		if (!cut.window_start) cut.window_start = tick;
		if (tick - cut.window_start >= 5000) {
			say("NR: %llu frames in %.1f s, each answered on its own frame, %.0f ms from its copy to "
			    "the answer; %u declined so far\n", cut.window_frames, (tick - cut.window_start) / 1000.0,
			    cut.window_ms / (double)cut.window_frames, cut.declined);
			cut.window_start = tick;
			cut.window_frames = 0;
			cut.window_ms = 0;
		}
		return;
	}
	if (ok && reply.status == NR_GPU_UNSUPPORTED) {
		/* The daemon has moved the fence on itself; the frame goes on as it was. */
		if (cut.declined++ % 100 == 0)
			say("the daemon declined frame %llu (%u so far); its log says why\n",
			    (unsigned long long)h->value, cut.declined);
		return;
	}
	/* No answer, or the daemon's session failed: it moved the fence where it could, and this
	 * makes sure. Its session is done with either way. */
	unblock(h->fence, h->value);
	InterlockedExchange(&cut.broken, 1);
	cut.failed++;
	say("frame %llu: %s; NR is off until the colour or the device changes\n", (unsigned long long)h->value,
	    ok ? "the daemon's session failed, and its log says why" : "no answer from the daemon");
}

static DWORD WINAPI cut_worker(LPVOID unused)
{
	(void)unused;
	for (;;) {
		WaitForSingleObject(cut.wake, 1000);
		read_settings(0);
		if (cut.releasing) {
			struct nr_gpu_reply reply;
			exchange_gpu(&cut.release, &reply);
			InterlockedExchange(&cut.releasing, 0);
		}
		if (cut.hello == 1) say_hello();
		ULONGLONG read_at = GetTickCount64();
		for (;;) {
			struct handover h;
			AcquireSRWLockExclusive(&cut.lock);
			int any = cut.count > 0;
			if (any) {
				h = cut.queue[cut.head];
				cut.head = (cut.head + 1) % HANDOVERS;
				cut.count--;
			}
			ReleaseSRWLockExclusive(&cut.lock);
			if (!any) break;
			hand_over(&h);
			ID3D12Fence_Release(h.fence);
			/* While the daemon is slower than the game the queue never empties, and the
			 * settings would be read again only when the game paused (Hogwarts, 10-28). */
			if (GetTickCount64() - read_at >= 1000) {
				read_settings(0);
				read_at = GetTickCount64();
			}
		}
	}
}

/* Between the halves, on the queue that runs them, inside the game's ExecuteCommandLists: the
 * first half is in the queue. Nothing here waits on the CPU. */
static void run_between(void *user, ID3D12CommandQueue *queue)
{
	struct handover *h = user;
	ID3D12CommandQueue_Signal(queue, h->fence, h->value);          /* the frame is in the input */
	QueryPerformanceCounter(&h->queued);
	AcquireSRWLockExclusive(&cut.lock);
	int room = cut.count < HANDOVERS;
	if (room) {
		cut.queue[(cut.head + cut.count) % HANDOVERS] = *h;        /* the worker releases the fence */
		cut.count++;
	}
	ReleaseSRWLockExclusive(&cut.lock);
	if (room) {
		SetEvent(cut.wake);
		/* the continuation waits for the daemon's V + 1: its answer, or its word there is none */
		ID3D12CommandQueue_Wait(queue, h->fence, h->value + 1);
	} else {
		/* the daemon is sixteen frames behind: this one goes on unanswered */
		ID3D12CommandQueue_Signal(queue, h->fence, h->value + 1);
		ID3D12Fence_Release(h->fence);
	}
	free(h);
}

/* The list was reset or released unexecuted: the frame never reached the queue. */
static void drop_handover(void *user)
{
	struct handover *h = user;
	ID3D12Fence_Release(h->fence);
	free(h);
}

/* The session's last frame done with its buffers, the daemon told to let them go, and ours
 * let go. */
static void cut_close(void)
{
	if (!cut.session) return;
	if (cut.last) {
		HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
		if (event && SUCCEEDED(ID3D12Fence_SetEventOnCompletion(cut.gpu.fence, cut.last + 1, event))
		    && WaitForSingleObject(event, 2000) != WAIT_OBJECT_0)
			say("the session's last frame was not done with its buffers after 2 s; going on\n");
		if (event) CloseHandle(event);
	}
	AcquireSRWLockExclusive(&cut.lock);
	if (cut.hello == 2) {
		nr_gpu_d3d12_packet(&cut.gpu, NR_GPU_RELEASE, 0, &cut.release);
		InterlockedExchange(&cut.releasing, 1);
	}
	nr_gpu_d3d12_close(&cut.gpu);
	cut.session = 0;
	cut.last = 0;
	InterlockedExchange(&cut.hello, 0);
	InterlockedExchange(&cut.broken, 0);
	cut.retry = 0;
	ReleaseSRWLockExclusive(&cut.lock);
	if (cut.wake) SetEvent(cut.wake);
}

/* A new device or a new colour: the old session goes, and the buffers come again, the shared
 * ones on the game's own device. Rare, so the game waits here. */
static int cut_rebuild(ID3D12Device *device, UINT width, UINT height, DXGI_FORMAT format, int kind,
		       UINT texel)
{
	cut_close();
	release_sized();
	if (device != nr.device && build_device(device) != 0) return -1;
	if (build_colour(width, height, format, kind, texel) != 0) {
		say("could not make the buffers for a %ux%u colour\n", width, height);
		return -1;
	}
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	uint64_t session = ((uint64_t)GetCurrentProcessId() << 40) ^ (uint64_t)now.QuadPart;
	AcquireSRWLockExclusive(&cut.lock);
	HRESULT hr = nr_gpu_d3d12_open(&cut.gpu, device, width, height, 37, session ? session : 1, 1);
	if (SUCCEEDED(hr)) cut.session = cut.gpu.session;
	cut.last = cut.last_call = 0;   /* a new session's frames lead back to none of the old's */
	ReleaseSRWLockExclusive(&cut.lock);
	if (FAILED(hr)) {
		say("could not make the buffers and fence the daemon shares (0x%08lx)\n", (unsigned long)hr);
		return -1;
	}
	if (!cut.thread) {
		cut.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
		cut.thread = CreateThread(NULL, 0, cut_worker, NULL, 0, NULL);
		if (!cut.wake || !cut.thread) return -1;
	}
	return 0;
}

static void bind_cut(ID3D12GraphicsCommandList *list, const struct cut_constants *k,
		     ID3D12Resource *const read[5], ID3D12Resource *written)
{
	ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 0, sizeof *k / 4, k, 0);
	for (UINT i = 0; i < 5; i++)
		ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(list, 1 + i,
			ID3D12Resource_GetGPUVirtualAddress(read[i]));
	ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 6,
		ID3D12Resource_GetGPUVirtualAddress(written));
	ID3D12GraphicsCommandList_Dispatch(list, (k->width + 7) / 8, (k->height + 7) / 8, 1);
}

/* Cuts the game's list at this execute: before the cut, the frame NR is given; after it, NR's
 * answer to it added to that frame. With mode=before the frame is the colour XeSS is about to
 * upscale, and the return is the texture XeSS should read instead, or NULL to leave the frame
 * to XeSS whole. With mode=after it is XeSS's output, recorded already, which the continuation
 * overwrites with NR's answer before the game goes on to its post-processing and interface;
 * the return is that output, or NULL when the frame goes on without NR. The game's textures
 * end each half in the state the call found them in (the colour NON_PIXEL_SHADER_RESOURCE,
 * the output UNORDERED_ACCESS), and our buffers and the shared ones start and end each half
 * in COMMON, which the daemon's side needs. */
static ID3D12Resource *record_cut(ID3D12GraphicsCommandList *list, const xess_d3d12_execute_params_t *p,
				  int after)
{
	uint64_t call = ++cut.calls;   /* offsets lead back only to the execute before this one */
	ID3D12Resource *frame = after ? p->pOutputTexture : p->pColorTexture;
	xess_2d_t base = after ? p->outputColorBase : p->inputColorBase;
	const char *what = after ? "XeSS's output" : "colour";
	D3D12_RESOURCE_DESC desc, colour_desc;
	ID3D12Resource_GetDesc(frame, &desc);
	ID3D12Resource_GetDesc(p->pColorTexture, &colour_desc);
	/* the render extent, which the vectors are at unless they are the output's */
	UINT render_width = p->inputWidth ? p->inputWidth : (UINT)colour_desc.Width;
	UINT render_height = p->inputHeight ? p->inputHeight : colour_desc.Height;
	UINT width = !after ? render_width : xess.output.x ? xess.output.x : (UINT)desc.Width - base.x;
	UINT height = !after ? render_height : xess.output.y ? xess.output.y : desc.Height - base.y;
	UINT texel;
	int kind = kind_of(desc.Format, &texel);
	if (kind < 0 || !width || !height || (UINT64)base.x + width > desc.Width
	    || (UINT64)base.y + height > desc.Height) {
		char why[160];
		snprintf(why, sizeof why, "%s format %d, %ux%u at (%u, %u) in %llux%u is not handled", what,
			 desc.Format, width, height, base.x, base.y, desc.Width, desc.Height);
		cut_said(why);
		return NULL;
	}
	ID3D12Device *device = NULL;
	if (FAILED(ID3D12GraphicsCommandList_GetDevice(list, &IID_ID3D12Device, (void **)&device)))
		return NULL;
	if (device != nr.device || width != nr.width || height != nr.height || desc.Format != nr.format) {
		say("%s %ux%u format %d at (%u, %u) in a %llux%u texture; exposure %g, jitter (%.3f, %.3f)\n",
		    what, width, height, desc.Format, base.x, base.y, desc.Width, desc.Height,
		    p->exposureScale, p->jitterOffsetX, p->jitterOffsetY);
		if (cut_rebuild(device, width, height, desc.Format, kind, texel) != 0) {
			say("setup failed; from here on XeSS gets every frame as it is\n");
			nr.broken = 1;
		}
	}
	ID3D12Device_Release(device);
	if (nr.broken) return NULL;
	if (cut.hello != 2) {
		if (cut.hello == 0 && GetTickCount64() >= cut.retry) {
			InterlockedExchange(&cut.hello, 1);
			SetEvent(cut.wake);
		}
		return NULL;
	}
	if (cut.broken) {
		cut_said("a frame went unanswered this session");
		return NULL;
	}
	char why[160];
	if (!nr_split_prepare(list, why, sizeof why)) {
		cut_said(why);
		return NULL;
	}
	nr_split_snapshot *state = nr_split_snapshot_take(list);   /* before anything of ours */
	if (!state) {
		cut_said("the command list's state is not known");
		return NULL;
	}
	/* XeSS multiplies the colour by the exposure; its output is taken to be in the colour's
	 * own scale, as other upscalers' is (Hogwarts' exposure is 1, so this is untried). */
	float white = config.white;
	if (p->exposureScale > 0) white /= p->exposureScale;
	/* the output is drawn without jitter */
	float jitter[2] = { after ? 0 : p->jitterOffsetX * xess.jitter[0], after ? 0 : p->jitterOffsetY * xess.jitter[1] };
	/* Where each pixel's surface was on the frame NR was given before, for the daemon's
	 * history: only when that frame came from the execute before this one, and the game's
	 * vectors, and its own history, say where. */
	UINT motion_width = 0, motion_height = 0;
	int follow = config.motion_history && cut.last && cut.last_call == call - 1 && !p->resetHistory
		     && motion_region(p, render_width, render_height, &motion_width, &motion_height)
		     && ensure_motion(motion_width, motion_height) == 0;
	AcquireSRWLockExclusive(&cut.lock);
	uint64_t value = nr_gpu_d3d12_next(&cut.gpu);
	ReleaseSRWLockExclusive(&cut.lock);
	struct cut_constants k = {
		.width = width, .height = height, .pitch = nr.footprint.Footprint.RowPitch,
		.kind = (uint32_t)nr.kind, .white = white, .strength = config.strength,
		.frame = { (uint32_t)value, (uint32_t)(value >> 32) },
		.offsets = (uint32_t)cut.gpu.motion,
		.jitter = { jitter[0], jitter[1] },
		.previous_jitter = { cut.last_jitter[0], cut.last_jitter[1] },
	};
	if (follow) {
		k.motion_pitch = nr.motion_footprint.Footprint.RowPitch;
		k.motion_size[0] = motion_width;
		k.motion_size[1] = motion_height;
		k.motion_scale[0] = xess.velocity[0] * width / motion_width;
		k.motion_scale[1] = xess.velocity[1] * height / motion_height;
		k.motion_texels[0] = (float)motion_width / width;
		k.motion_texels[1] = (float)motion_height / height;
	}
	ID3D12GraphicsCommandList *l = list;
	struct batch b = { .count = 0 };
	const D3D12_RESOURCE_STATES common = D3D12_RESOURCE_STATE_COMMON,
		read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		source = D3D12_RESOURCE_STATE_COPY_SOURCE, dest = D3D12_RESOURCE_STATE_COPY_DEST,
		write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	const D3D12_RESOURCE_STATES at = after ? write : read;   /* the frame's state at the call */

	/* The first half: the frame as a buffer, and the frame NR is given from it; after the frame
	 * in the same buffer, from a copy of the vectors, the offsets when they are to be followed. */
	add(&b, frame, at, source);
	add(&b, nr.staging, common, dest);
	if (follow) {
		add(&b, p->pVelocityTexture, read, source);
		add(&b, nr.motion[0], common, dest);
	}
	flush(l, &b);
	D3D12_BOX box = { base.x, base.y, 0, base.x + width, base.y + height, 1 };
	D3D12_TEXTURE_COPY_LOCATION colour = { .pResource = frame,
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
	D3D12_TEXTURE_COPY_LOCATION into = { .pResource = nr.staging,
		.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = nr.footprint };
	ID3D12GraphicsCommandList_CopyTextureRegion(l, &into, 0, 0, 0, &colour, &box);
	if (follow) {
		D3D12_BOX region = { p->inputMotionVectorBase.x, p->inputMotionVectorBase.y, 0,
				     p->inputMotionVectorBase.x + motion_width,
				     p->inputMotionVectorBase.y + motion_height, 1 };
		D3D12_TEXTURE_COPY_LOCATION vectors = { .pResource = p->pVelocityTexture,
			.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
		D3D12_TEXTURE_COPY_LOCATION to = { .pResource = nr.motion[0],
			.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = nr.motion_footprint };
		ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, 0, 0, 0, &vectors, &region);
		add(&b, p->pVelocityTexture, source, read);
		add(&b, nr.motion[0], dest, read);
	}
	add(&b, frame, source, at);
	add(&b, nr.staging, dest, read);
	add(&b, cut.gpu.input, common, write);
	flush(l, &b);
	ID3D12GraphicsCommandList_SetComputeRootSignature(l, nr.signature);
	ID3D12GraphicsCommandList_SetPipelineState(l, nr.encode_pipeline);
	ID3D12Resource *encoding[5] = { nr.staging, nr.srgb_table, nr.staging, nr.staging, nr.staging };
	bind_cut(l, &k, encoding, cut.gpu.input);
	if (follow) {   /* the offsets lie after the frame ENCODE writes, so no barrier between */
		ID3D12GraphicsCommandList_SetPipelineState(l, nr.motion_pipeline);
		ID3D12Resource *moving[5] = { nr.motion[0], nr.motion[0], nr.motion[0], nr.motion[0], nr.motion[0] };
		bind_cut(l, &k, moving, cut.gpu.input);
	}
	add(&b, cut.gpu.input, write, common);
	add(&b, nr.staging, read, common);
	if (follow) add(&b, nr.motion[0], read, common);
	flush(l, &b);

	struct handover *h = malloc(sizeof *h);
	if (!h) {
		nr_split_restore(list, state);
		nr_split_snapshot_free(state);
		return NULL;
	}
	h->fence = cut.gpu.fence;
	ID3D12Fence_AddRef(h->fence);
	h->value = value;
	h->session = cut.session;
	h->previous = follow ? cut.last : 0;
	if (!nr_split_cut(list, run_between, drop_handover, h, state, why, sizeof why)) {
		ID3D12Fence_Release(h->fence);
		free(h);
		nr_split_restore(list, state);
		nr_split_snapshot_free(state);
		cut_said(why);
		return NULL;
	}
	nr_split_snapshot_free(state);
	cut.last = value;
	cut.last_call = call;
	cut.last_jitter[0] = jitter[0];
	cut.last_jitter[1] = jitter[1];
	cut.followed = follow;
	cut.said[0] = '\0';
	cut_now = 1;

	/* The continuation: once the queue has the daemon's V + 1, NR's answer into the frame. */
	add(&b, nr.staging, common, read);
	add(&b, cut.gpu.input, common, read);
	add(&b, cut.gpu.output, common, read);
	add(&b, nr.output, common, write);
	flush(l, &b);
	ID3D12GraphicsCommandList_SetComputeRootSignature(l, nr.signature);
	ID3D12GraphicsCommandList_SetPipelineState(l, nr.compose_pipeline);
	ID3D12Resource *composing[5] = { nr.staging, cut.gpu.input, cut.gpu.output, nr.linear_table, nr.staging };
	bind_cut(l, &k, composing, nr.output);
	add(&b, nr.staging, read, common);
	add(&b, cut.gpu.input, read, common);
	add(&b, cut.gpu.output, read, common);
	add(&b, nr.output, write, source);
	/* before XeSS, into the texture XeSS reads instead; after it, back into its output */
	ID3D12Resource *target = after ? frame : nr.mixed;
	if (after) add(&b, frame, at, dest);
	else if (nr.mixed_state != dest) add(&b, nr.mixed, nr.mixed_state, dest);
	flush(l, &b);
	D3D12_TEXTURE_COPY_LOCATION from = { .pResource = nr.output,
		.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = nr.footprint };
	D3D12_TEXTURE_COPY_LOCATION to = { .pResource = target,
		.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
	ID3D12GraphicsCommandList_CopyTextureRegion(l, &to, after ? base.x : 0, after ? base.y : 0, 0, &from, NULL);
	add(&b, nr.output, source, common);
	add(&b, target, dest, after ? at : read);
	flush(l, &b);
	if (!after) nr.mixed_state = read;
	return target;
}

xess_result_t xessD3D12Execute(xess_context_handle_t context, ID3D12GraphicsCommandList *list,
			       const xess_d3d12_execute_params_t *params)
{
	if (!real_execute) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
	handed = params ? params->pColorTexture : NULL;
	cut_now = 0;
	if (nr.broken || !config.enabled || !list || !params || !params->pColorTexture)
		return real_execute(context, list, params);
	if (config.cut == AFTER) {
		/* XeSS's work goes into the list first, and the cut after it */
		xess_result_t result = real_execute(context, list, params);
		if (result == 0 && params->pOutputTexture) record_cut(list, params, 1);
		return result;
	}
	ID3D12Resource *mixed = config.cut == BEFORE ? record_cut(list, params, 0) : record(list, params);
	if (!mixed) return real_execute(context, list, params);
	handed = mixed;
	xess_d3d12_execute_params_t changed = *params;
	changed.pColorTexture = mixed;
	changed.inputColorBase.x = 0;
	changed.inputColorBase.y = 0;
	return real_execute(context, list, &changed);
}

xess_result_t xessD3D12Init(xess_context_handle_t context, const xess_d3d12_init_params_t *params)
{
	if (!real_init) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
	xess_result_t result = real_init(context, params);
	if (params) {
		say("XeSS init: output %ux%u, quality %d, flags 0x%x -> %d\n",
		    params->outputResolution.x, params->outputResolution.y, params->qualitySetting,
		    params->initFlags, result);
		if (result == 0) {
			xess.flags = params->initFlags;
			xess.output = params->outputResolution;
			xess.quality = params->qualitySetting;
		}
	}
	return result;
}

/* The two scales the game may set: they say what a motion texel and a jitter offset mean,
 * so the trackers read both the way XeSS does. Said in the log when they change. */
xess_result_t xessSetVelocityScale(xess_context_handle_t context, float x, float y)
{
	if (!real_velocity_scale) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
	xess_result_t result = real_velocity_scale(context, x, y);
	if (result == 0 && (x != xess.velocity[0] || y != xess.velocity[1])) {
		xess.velocity[0] = x;
		xess.velocity[1] = y;
		say("XeSS velocity scale (%g, %g)\n", x, y);
	}
	return result;
}

xess_result_t xessSetJitterScale(xess_context_handle_t context, float x, float y)
{
	if (!real_jitter_scale) return XESS_RESULT_ERROR_CANT_LOAD_LIBRARY;
	xess_result_t result = real_jitter_scale(context, x, y);
	if (result == 0 && (x != xess.jitter[0] || y != xess.jitter[1])) {
		xess.jitter[0] = x;
		xess.jitter[1] = y;
		say("XeSS jitter scale (%g, %g)\n", x, y);
	}
	return result;
}

#ifndef NR_XESS_TEST
/* What a jump lands on when the original does not export the name: an older libxess. */
static xess_result_t missing_export(void)
{
	return -7;                      /* XESS_RESULT_ERROR_NOT_IMPLEMENTED */
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH) return TRUE;
	DisableThreadLibraryCalls(instance);
	DWORD length = GetModuleFileNameW(instance, here, MAX_PATH);
	if (!length || length >= MAX_PATH) return FALSE;
	wchar_t *slash = wcsrchr(here, L'\\');
	if (!slash) return FALSE;
	slash[1] = L'\0';
	_snwprintf(log_path, MAX_PATH + 16, L"%lsnr-xess.log", here);
	/* By its full path: the game's DLL search path need not include this directory. */
	wchar_t original[MAX_PATH + 32];
	_snwprintf(original, MAX_PATH + 32, L"%lslibxess_nr_orig.dll", here);
	HMODULE module = LoadLibraryExW(original, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!module) {
		say("cannot load libxess_nr_orig.dll beside this one (error %lu); XeSS is unavailable\n",
		    GetLastError());
		return FALSE;
	}
	for (int i = 0; i < NR_XESS_FORWARDED; i++) {
		nr_xess_forward[i] = (void *)GetProcAddress(module, nr_xess_forwarded[i]);
		if (!nr_xess_forward[i]) nr_xess_forward[i] = (void *)missing_export;
	}
	real_execute = (execute_fn)(void *)GetProcAddress(module, "xessD3D12Execute");
	real_init = (init_fn)(void *)GetProcAddress(module, "xessD3D12Init");
	real_velocity_scale = (scale_fn)(void *)GetProcAddress(module, "xessSetVelocityScale");
	real_jitter_scale = (scale_fn)(void *)GetProcAddress(module, "xessSetJitterScale");
	build_tables();
	nr_split_set_log(say_line);
	read_settings(1);
	say("NR in front of XeSS, process %lu\n", GetCurrentProcessId());
	return TRUE;
}
#endif
