/* test_nr_xess.c — the XeSS proxy's GPU work on this machine's D3D12 device, without a
 * game. A colour texture goes through two executes: the first is captured for NR, the
 * second is handed NR's change. Both are checked against the CPU, for each colour format
 * the proxy takes, and with the strength at 0 the pass must give the colour back exactly.
 * Then the change is followed through motion — a uniform motion with the picture moving
 * along, a half-pixel jitter, and a block whose vectors say it moved while the picture did
 * not, where the change is not trusted and is filled from around it — and one change fades into the
 * next over three executes. Every expected value comes from a CPU transcription of
 * nr_xess.hlsl below.
 *
 *   test_nr_xess              a stand-in daemon
 *   test_nr_xess <pipe>       the daemon listening at <pipe>, for the whole round trip
 */
#define NR_XESS_TEST
#include "nr_xess.c"

static xess_d3d12_execute_params_t seen;

static xess_result_t stub_execute(xess_context_handle_t context, ID3D12GraphicsCommandList *list,
				  const xess_d3d12_execute_params_t *params)
{
	(void)context;
	(void)list;
	seen = *params;
	return 0;
}

static xess_result_t stub_init(xess_context_handle_t context, const xess_d3d12_init_params_t *params)
{
	(void)context;
	(void)params;
	return 0;
}

/* What the stand-in daemons were told about motion: the sum and count of the known
 * offsets, over every request that carried any. */
static double motion_sum[2];
static size_t motion_known;

static void note_motion(const float *motion, uint32_t width, uint32_t height)
{
	for (size_t i = 0; motion && i < (size_t)width * height; i++)
		if (!isnan(motion[2 * i])) {
			motion_sum[0] += motion[2 * i];
			motion_sum[1] += motion[2 * i + 1];
			motion_known++;
		}
}

/* Every byte brightened by 20. */
static int brighten(const uint8_t *frame, const float *motion, uint8_t *answer, uint32_t width,
		    uint32_t height)
{
	note_motion(motion, width, height);
	for (size_t i = 0; i < (size_t)width * height * 4; i++)
		answer[i] = (uint8_t)(frame[i] > 235 ? 255 : frame[i] + 20);
	return 0;
}

/* Unchanged but for one square, brightened by `spot_level`, so where a change lands is plain
 * to see and two answers can differ. */
#define SPOT_X 30
#define SPOT_Y 18
static int spot_level = 60;

static int spot(const uint8_t *frame, const float *motion, uint8_t *answer, uint32_t width,
		uint32_t height)
{
	note_motion(motion, width, height);
	memcpy(answer, frame, (size_t)width * height * 4);
	for (uint32_t y = SPOT_Y; y < SPOT_Y + 6; y++)
		for (uint32_t x = SPOT_X; x < SPOT_X + 6; x++)
			for (int c = 0; c < 3; c++) {
				uint8_t *b = answer + ((size_t)y * width + x) * 4 + c;
				*b = (uint8_t)(*b + spot_level > 255 ? 255 : *b + spot_level);
			}
	return 0;
}

static struct {
	ID3D12Device *device;
	ID3D12CommandQueue *queue;
	ID3D12CommandAllocator *allocator;
	ID3D12GraphicsCommandList *list;
	ID3D12Fence *fence;
	UINT64 value;
	HANDLE event;
} gpu;

static int submit(void)
{
	ID3D12GraphicsCommandList_Close(gpu.list);
	ID3D12CommandList *lists[] = { (ID3D12CommandList *)gpu.list };
	ID3D12CommandQueue_ExecuteCommandLists(gpu.queue, 1, lists);
	ID3D12CommandQueue_Signal(gpu.queue, gpu.fence, ++gpu.value);
	ID3D12Fence_SetEventOnCompletion(gpu.fence, gpu.value, gpu.event);
	if (WaitForSingleObject(gpu.event, 10000) != WAIT_OBJECT_0) {
		printf("FAIL: the GPU did not finish (device removed: 0x%lx)\n",
		       (unsigned long)ID3D12Device_GetDeviceRemovedReason(gpu.device));
		return -1;
	}
	ID3D12CommandAllocator_Reset(gpu.allocator);
	ID3D12GraphicsCommandList_Reset(gpu.list, gpu.allocator, NULL);
	return 0;
}

#define W 80
#define H 50
#define BASE_X 5
#define BASE_Y 3
#define IN_W 70
#define IN_H 45
#define MOTION_W (2 * IN_W)    /* high-resolution vectors, twice the colour's extent */
#define MOTION_H (2 * IN_H)

static void pattern(int x, int y, float *rgba)
{
	rgba[0] = x / 40.0f;               /* to 2: past the knee, so the gain is exercised */
	rgba[1] = y / 25.0f;
	rgba[2] = ((x + y) % 7) / 3.0f;
	rgba[3] = 0.5f;
}

static void pack(int kind, const float *c, uint8_t *out)
{
	if (kind == KIND_RGBA16F) {
		uint16_t h[4] = { to_half(c[0]), to_half(c[1]), to_half(c[2]), to_half(c[3]) };
		memcpy(out, h, 8);
		return;
	}
	uint32_t r = (uint32_t)((to_half(c[0]) + 0x8) >> 4), g = (uint32_t)((to_half(c[1]) + 0x8) >> 4);
	uint32_t b = (uint32_t)((to_half(c[2]) + 0x10) >> 5);
	uint32_t v = (r > 0x7BF ? 0x7BF : r) | (g > 0x7BF ? 0x7BF : g) << 11 | (b > 0x3DF ? 0x3DF : b) << 22;
	memcpy(out, &v, 4);
}

/* A block of the picture that moved by (dx, dy) colour pixels on the second frame of
 * `run_uncovered`: its pixels show what was (dx, dy) behind them, and the strip it left
 * shows the background — the same as the block showed there, so the picture alone cannot
 * tell them apart. Empty outside that case. */
static struct { int x0, y0, x1, y1, dx, dy; } moved;

/* The pattern at texel (x, y) of the colour texture, the moved block in it. */
static void scene(int x, int y, float *rgba)
{
	int cx = x - BASE_X, cy = y - BASE_Y;
	int inside = cx >= moved.x0 && cy >= moved.y0 && cx < moved.x1 && cy < moved.y1;
	pattern(x - (inside ? moved.dx : 0), y - (inside ? moved.dy : 0), rgba);
}

/* The colour of input pixel (x, y) as the format holds it, the picture moved so that the
 * pixel shows the pattern at (x + sx, y + sy). */
static void colour_at(int kind, int x, int y, int sx, int sy, float *rgba)
{
	uint8_t packed[8];
	scene(x + BASE_X + sx, y + BASE_Y + sy, rgba);
	pack(kind, rgba, packed);
	load_texel(packed, kind, rgba);
}

/* A texture of the given texels, in NON_PIXEL_SHADER_RESOURCE as XeSS's inputs are. */
/* A texture filled by `fill`, left in `state`; `flags` for one XeSS writes its output into. */
static ID3D12Resource *texture_as(DXGI_FORMAT format, UINT width, UINT height, UINT texel,
				  void (*fill)(int x, int y, uint8_t *out, const void *how), const void *how,
				  D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
	D3D12_RESOURCE_DESC desc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = width, .Height = height,
		.DepthOrArraySize = 1, .MipLevels = 1, .Format = format, .SampleDesc = { 1, 0 }, .Flags = flags,
	};
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT rows;
	UINT64 row_bytes, bytes;
	ID3D12Device_GetCopyableFootprints(gpu.device, &desc, 0, 1, 0, &footprint, &rows, &row_bytes, &bytes);
	D3D12_HEAP_PROPERTIES local = { .Type = D3D12_HEAP_TYPE_DEFAULT };
	ID3D12Resource *tex = NULL, *upload;
	if (FAILED(ID3D12Device_CreateCommittedResource(gpu.device, &local, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex)))
		return NULL;
	ID3D12Device *keep = nr.device;         /* make_buffer works on nr's device */
	nr.device = gpu.device;
	upload = make_buffer(D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_FLAG_NONE,
			     D3D12_RESOURCE_STATE_GENERIC_READ);
	nr.device = keep;
	uint8_t *mapped;
	ID3D12Resource_Map(upload, 0, NULL, (void **)&mapped);
	for (UINT y = 0; y < height; y++)
		for (UINT x = 0; x < width; x++)
			fill((int)x, (int)y, mapped + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch
			     + (size_t)x * texel, how);
	ID3D12Resource_Unmap(upload, 0, NULL);
	D3D12_TEXTURE_COPY_LOCATION to = { .pResource = tex, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	D3D12_TEXTURE_COPY_LOCATION from = { .pResource = upload, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
					     .PlacedFootprint = footprint };
	ID3D12GraphicsCommandList_CopyTextureRegion(gpu.list, &to, 0, 0, 0, &from, NULL);
	D3D12_RESOURCE_BARRIER b = transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, state);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
	if (submit()) return NULL;
	ID3D12Resource_Release(upload);
	return tex;
}

static ID3D12Resource *texture(DXGI_FORMAT format, UINT width, UINT height, UINT texel,
			       void (*fill)(int x, int y, uint8_t *out, const void *how), const void *how)
{
	return texture_as(format, width, height, texel, fill, how, D3D12_RESOURCE_FLAG_NONE,
			  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

struct colour_how { int kind, sx, sy; };

static void fill_colour(int x, int y, uint8_t *out, const void *how)
{
	const struct colour_how *c = how;
	float rgba[4];
	scene(x + c->sx, y + c->sy, rgba);
	pack(c->kind, rgba, out);
}

/* Motion in texels: `how` is { x, y } everywhere, or { x, y, x0, y0, x1, y1 } inside a block
 * of texels and nothing outside it. */
static void fill_motion(int x, int y, uint8_t *out, const void *how)
{
	const float *m = how;
	int inside = m[2] == 0 && m[4] == 0 ? 1 : x >= m[2] && y >= m[3] && x < m[4] && y < m[5];
	uint16_t v[2] = { to_half(inside ? m[0] : 0), to_half(inside ? m[1] : 0) };
	memcpy(out, v, 4);
}

/* Until the worker has published a change from a capture after `after` and is idle again. */
static int wait_change(uint32_t after)
{
	for (ULONGLONG until = GetTickCount64() + 60000; GetTickCount64() < until; Sleep(5)) {
		AcquireSRWLockExclusive(&nr.lock);
		int done = nr.ready >= 0 && nr.change_anchor[nr.ready] > after && nr.capture == 0;
		ReleaseSRWLockExclusive(&nr.lock);
		if (done) return 0;
	}
	printf("FAIL: no change was published within 60 s (daemon failures: %u)\n", nr.failed);
	return -1;
}

/* The colour an execute handed XeSS, as XeSS would read it. */
static uint8_t *read_mixed(void)
{
	ID3D12Resource *readback = make_buffer(D3D12_HEAP_TYPE_READBACK, nr.bytes,
					       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_RESOURCE_BARRIER b = transition(nr.mixed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					      D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
	D3D12_TEXTURE_COPY_LOCATION to = { .pResource = readback, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
					   .PlacedFootprint = nr.footprint };
	D3D12_TEXTURE_COPY_LOCATION from = { .pResource = nr.mixed, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	ID3D12GraphicsCommandList_CopyTextureRegion(gpu.list, &to, 0, 0, 0, &from, NULL);
	b = transition(nr.mixed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
	if (submit()) return NULL;
	uint8_t *raw, *copy = malloc((size_t)nr.bytes);
	D3D12_RANGE all = { 0, (SIZE_T)nr.bytes }, none = { 0, 0 };
	ID3D12Resource_Map(readback, 0, &all, (void **)&raw);
	memcpy(copy, raw, (size_t)nr.bytes);
	ID3D12Resource_Unmap(readback, 0, &none);
	ID3D12Resource_Release(readback);
	return copy;
}

/* ---- nr_xess.hlsl's MATCH, REDUCE and APPLY on the CPU ---- */

/* A capture's change as the worker published it: what NR was given and what it answered. */
struct snapshot {
	uint8_t given[IN_W * IN_H * 4], answer[IN_W * IN_H * 4];
};

static void take(struct snapshot *s)
{
	memcpy(s->given, nr.frame, sizeof s->given);
	memcpy(s->answer, nr.answer, sizeof s->answer);
}

/* The change (c < 3), or what NR was given as `likeness` measures it (c 3-5), as published. */
static float fine_value(const struct snapshot *s, int x, int y, int c)
{
	const uint8_t *g = s->given + ((size_t)y * IN_W + x) * 4, *a = s->answer + ((size_t)y * IN_W + x) * 4;
	if (c < 3) return half_value[change_half[a[c] << 8 | g[c]]];
	return half_value[likeness_half[g[c - 3]]];
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void fine_at(const struct snapshot *s, float gx, float gy, float *out)
{
	float cx = clampf(gx, 0, IN_W - 1), cy = clampf(gy, 0, IN_H - 1);
	int ix = (int)floorf(cx), iy = (int)floorf(cy);
	int jx = ix + 1 < IN_W ? ix + 1 : IN_W - 1, jy = iy + 1 < IN_H ? iy + 1 : IN_H - 1;
	float fx = cx - floorf(cx), fy = cy - floorf(cy);
	for (int c = 0; c < 6; c++) {
		float top = fine_value(s, ix, iy, c) * (1 - fx) + fine_value(s, jx, iy, c) * fx;
		float bottom = fine_value(s, ix, jy, c) * (1 - fx) + fine_value(s, jx, jy, c) * fx;
		out[c] = top * (1 - fy) + bottom * fy;
	}
}

static void likeness(const float *rgb, float *out)
{
	float d[3], white = config.white;
	for (int c = 0; c < 3; c++) d[c] = (rgb[c] > 0 ? rgb[c] : 0) / white;
	float lit = 0.2126f * d[0] + 0.7152f * d[1] + 0.0722f * d[2];
	if (lit > 0.75f)
		for (int c = 0; c < 3; c++) d[c] *= (0.75f + 0.25f * (1 - expf(-(lit - 0.75f) / 0.25f))) / lit;
	for (int c = 0; c < 3; c++) out[c] = sqrtf(clampf(d[c], 0, 1));
}

/* Where the tracker sends pixel (x, y) on the change's grid, jitter included. NULL for an
 * execute without motion vectors: the change is read in place, wholly trusted. */
typedef void (*place_fn)(int x, int y, float *gx, float *gy);

static void trusted(const struct snapshot *s, place_fn place, int x, int y, const float *low,
		    const float *high, float *out)
{
	if (!place) {
		for (int c = 0; c < 3; c++) out[c] = fine_value(s, x, y, c);
		out[3] = 1;
		return;
	}
	float gx, gy, d[6];
	place(x, y, &gx, &gy);
	out[0] = out[1] = out[2] = out[3] = 0;
	if (gx >= -0.5f && gy >= -0.5f && gx <= IN_W - 0.5f && gy <= IN_H - 0.5f) {
		fine_at(s, gx, gy, d);
		float outside = -1e30f;
		for (int c = 0; c < 3; c++) {
			float o = low[c] - d[3 + c] > d[3 + c] - high[c] ? low[c] - d[3 + c] : d[3 + c] - high[c];
			outside = o > outside ? o : outside;
		}
		for (int c = 0; c < 3; c++) out[c] = d[c];
		out[3] = clampf((config.match_to - outside) / (config.match_to - config.match_from), 0, 1);
	}
}

#define CELLS_W ((IN_W + CELL - 1) / CELL)
#define CELLS_H ((IN_H + CELL - 1) / CELL)

/* The vectors of the frame a change is applied to, as the test drew them (`fill_motion`'s
 * `how`), and the last frame's (NULL when it had none), with both frames' jitter: what CLAIM,
 * TRACK, REDUCE and APPLY read of them. */
struct field {
	const float *how, *previous_how;
	float jitter[2], previous_jitter[2];
};

/* nr_xess.hlsl's `vector_in`: the nearest texel to an unjittered position, in colour pixels. */
static void field_vector(const float *how, float ux, float uy, float *m)
{
	int tx = (int)floorf((ux + 0.5f) * ((float)MOTION_W / IN_W));
	int ty = (int)floorf((uy + 0.5f) * ((float)MOTION_H / IN_H));
	tx = tx < 0 ? 0 : tx > MOTION_W - 1 ? MOTION_W - 1 : tx;
	ty = ty < 0 ? 0 : ty > MOTION_H - 1 ? MOTION_H - 1 : ty;
	uint8_t texel[4];
	uint16_t h[2];
	fill_motion(tx, ty, texel, how);
	memcpy(h, texel, 4);
	m[0] = half_value[h[0]] * ((float)IN_W / MOTION_W);
	m[1] = half_value[h[1]] * ((float)IN_H / MOTION_H);
}

static float spread(const float *m)
{
	return 1.5f + 0.15f * sqrtf(m[0] * m[0] + m[1] * m[1]);
}

/* CLAIM, then TRACK's `contested`, for every pixel: 1 where the tracker gives up. */
static void contested_map(const struct field *f, uint8_t *out)
{
	static float low[IN_H][IN_W][2], high[IN_H][IN_W][2];
	static uint8_t claimed_at[IN_H][IN_W];
	memset(claimed_at, 0, sizeof claimed_at);
	float m[2];
	for (int y = 0; y < IN_H; y++)
		for (int x = 0; x < IN_W; x++) {
			float ux = x - f->jitter[0], uy = y - f->jitter[1];
			field_vector(f->how, ux, uy, m);
			int ix = (int)floorf(ux + m[0] + f->previous_jitter[0] + 0.5f);
			int iy = (int)floorf(uy + m[1] + f->previous_jitter[1] + 0.5f);
			if (ix < 0 || iy < 0 || ix >= IN_W || iy >= IN_H) continue;
			for (int c = 0; c < 2; c++) {
				if (!claimed_at[iy][ix] || m[c] < low[iy][ix][c]) low[iy][ix][c] = m[c];
				if (!claimed_at[iy][ix] || m[c] > high[iy][ix][c]) high[iy][ix][c] = m[c];
			}
			claimed_at[iy][ix] = 1;
		}
	for (int y = 0; y < IN_H; y++)
		for (int x = 0; x < IN_W; x++) {
			float ux = x - f->jitter[0], uy = y - f->jitter[1];
			field_vector(f->how, ux, uy, m);
			int nx = (int)floorf(ux + m[0] + f->previous_jitter[0] + 0.5f);
			int ny = (int)floorf(uy + m[1] + f->previous_jitter[1] + 0.5f);
			nx = nx < 0 ? 0 : nx > IN_W - 1 ? IN_W - 1 : nx;
			ny = ny < 0 ? 0 : ny > IN_H - 1 ? IN_H - 1 : ny;
			float lo[2] = { m[0], m[1] }, hi[2] = { m[0], m[1] }, apart = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int qx = nx + dx < 0 ? 0 : nx + dx > IN_W - 1 ? IN_W - 1 : nx + dx;
					int qy = ny + dy < 0 ? 0 : ny + dy > IN_H - 1 ? IN_H - 1 : ny + dy;
					if (!claimed_at[qy][qx]) continue;
					for (int c = 0; c < 2; c++) {
						lo[c] = low[qy][qx][c] < lo[c] ? low[qy][qx][c] : lo[c];
						hi[c] = high[qy][qx][c] > hi[c] ? high[qy][qx][c] : hi[c];
					}
				}
			for (int c = 0; c < 2; c++) {
				apart = m[c] - lo[c] > apart ? m[c] - lo[c] : apart;
				apart = hi[c] - m[c] > apart ? hi[c] - m[c] : apart;
			}
			int lost = apart > spread(m);
			if (lost && f->previous_how) {       /* the surface that was there keeps it */
				float then[2], bar = spread(m) > 0.5f * apart ? spread(m) : 0.5f * apart;
				field_vector(f->previous_how, ux + m[0], uy + m[1], then);
				lost = !(sqrtf((then[0] - m[0]) * (then[0] - m[0]) + (then[1] - m[1]) * (then[1] - m[1])) < bar);
			}
			out[(size_t)y * IN_W + x] = (uint8_t)lost;
		}
}

/* The change each pixel of a frame whose picture moved by (sx, sy) takes: the newest change
 * through `place`, the one before it through `old_place` given way `fade` of the way, and the
 * fill where neither was trusted. `f`, the frame's vectors, NULL for an execute without:
 * where they contest the trackers, and how alike the fill's cells move. */
static void expected(const struct snapshot *newest, place_fn place, const struct snapshot *old,
		     place_fn old_place, float fade, int kind, int sx, int sy, const struct field *f,
		     float *delta)
{
	static float matched[IN_W * IN_H * 7], sums[CELLS_W * CELLS_H * 9], velocity[IN_W * IN_H * 2];
	static uint8_t contested[IN_W * IN_H];
	memset(contested, 0, sizeof contested);
	if (f) {
		if (place) contested_map(f, contested);
		for (int y = 0; y < IN_H; y++)
			for (int x = 0; x < IN_W; x++)
				field_vector(f->how, x - f->jitter[0], y - f->jitter[1], velocity + ((size_t)y * IN_W + x) * 2);
	}
	for (int y = 0; y < IN_H; y++)
		for (int x = 0; x < IN_W; x++) {
			float low[3] = { 1, 1, 1 }, high[3] = { 0, 0, 0 }, rgba[4], own[3] = { 0 }, l[3];
			float now[4], then[4];
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int qx = x + dx < 0 ? 0 : x + dx > IN_W - 1 ? IN_W - 1 : x + dx;
					int qy = y + dy < 0 ? 0 : y + dy > IN_H - 1 ? IN_H - 1 : y + dy;
					colour_at(kind, qx, qy, sx, sy, rgba);
					likeness(rgba, l);
					for (int c = 0; c < 3; c++) {
						low[c] = l[c] < low[c] ? l[c] : low[c];
						high[c] = l[c] > high[c] ? l[c] : high[c];
						if (!dx && !dy) own[c] = l[c];
					}
				}
			trusted(newest, place, x, y, low, high, now);
			int lost = contested[(size_t)y * IN_W + x];    /* the trackers hold UNKNOWN */
			if (lost) now[0] = now[1] = now[2] = now[3] = 0;
			float *m = matched + ((size_t)y * IN_W + x) * 7;
			for (int c = 0; c < 3; c++) m[c] = now[c] * now[3];
			m[3] = now[3];
			if (old) {
				trusted(old, old_place, x, y, low, high, then);
				if (lost && old_place) then[0] = then[1] = then[2] = then[3] = 0;
				for (int c = 0; c < 3; c++) m[c] = then[c] * then[3] + fade * (m[c] - then[c] * then[3]);
				m[3] = then[3] + fade * (m[3] - then[3]);
			}
			for (int c = 0; c < 3; c++) m[4 + c] = own[c];
		}
	for (int cy = 0; cy < CELLS_H; cy++)
		for (int cx = 0; cx < CELLS_W; cx++) {
			float *s = sums + ((size_t)cy * CELLS_W + cx) * 9;
			for (int c = 0; c < 9; c++) s[c] = 0;
			for (int y = cy * CELL; y < (cy + 1) * CELL && y < IN_H; y++)
				for (int x = cx * CELL; x < (cx + 1) * CELL && x < IN_W; x++) {
					const float *m = matched + ((size_t)y * IN_W + x) * 7;
					for (int c = 0; c < 4; c++) s[c] += m[c];
					for (int c = 4; c < 7; c++) s[c] += m[3] * m[c];
					for (int c = 0; c < 2 && f; c++) s[7 + c] += m[3] * velocity[((size_t)y * IN_W + x) * 2 + c];
				}
		}
	for (int y = 0; y < IN_H; y++)
		for (int x = 0; x < IN_W; x++) {
			const float *m = matched + ((size_t)y * IN_W + x) * 7;
			const float *mine = velocity + ((size_t)y * IN_W + x) * 2;
			float *out = delta + ((size_t)y * IN_W + x) * 3;
			float fill[3] = { 0, 0, 0 }, total = 0, reach = f ? spread(mine) : 1;
			for (int c = 0; c < 3; c++) out[c] = m[c];
			if (m[3] >= 1) continue;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					int cx = x / CELL + dx, cy = y / CELL + dy;
					if (cx < 0 || cy < 0 || cx >= CELLS_W || cy >= CELLS_H) continue;
					const float *s = sums + ((size_t)cy * CELLS_W + cx) * 9;
					if (s[3] < 1e-3f) continue;
					float apart = 0;
					for (int c = 0; c < 3; c++) {
						float alike = s[4 + c] / s[3] - m[4 + c];
						apart += alike * alike;
					}
					float ex = x - ((cx + 0.5f) * CELL - 0.5f), ey = y - ((cy + 0.5f) * CELL - 0.5f);
					float closeness = clampf(1.5f - sqrtf(ex * ex + ey * ey) / CELL, 0, 1);
					float weight = s[3] * closeness * expf(-apart / (0.04f * 0.04f));
					if (f) {
						float vx = s[7] / s[3] - mine[0], vy = s[8] / s[3] - mine[1];
						weight *= expf(-(vx * vx + vy * vy) / (reach * reach));
					}
					for (int c = 0; c < 3; c++) fill[c] += weight * s[c] / s[3];
					total += weight;
				}
			for (int c = 0; c < 3; c++) out[c] += (1 - m[3]) * fill[c] / (total > 8 ? total : 8);
		}
}

/* ---- the cases ---- */

/* The colour an execute handed XeSS, against its own colour plus `delta`. */
static int compare(const char *name, int kind, UINT texel, const uint8_t *mixed, int sx, int sy,
		   float strength, const float *delta, int exact_where_zero)
{
	int failures = 0;
	double worst = 0, added = 0;
	for (int y = 0; y < IN_H; y++)
		for (int x = 0; x < IN_W; x++) {
			float o[4], got[4] = { 0 }, white = config.white;
			colour_at(kind, x, y, sx, sy, o);
			load_texel(mixed + nr.footprint.Offset + (size_t)y * nr.footprint.Footprint.RowPitch
				   + (size_t)x * texel, kind, got);
			float lit = (0.2126f * o[0] + 0.7152f * o[1] + 0.0722f * o[2]) / white;
			float gain = lit > 0.75f ? lit / (0.75f + 0.25f * (1 - expf(-(lit - 0.75f) / 0.25f))) : 1;
			for (int c = 0; c < 3; c++) {
				float d = delta[((size_t)y * IN_W + x) * 3 + c];
				float want = o[c] + white * strength * gain * d;
				if (want < 0) want = 0;
				double tolerance = kind == KIND_RGBA16F ? 2e-3 * (1 + fabs(want)) : 4e-2 * (1 + fabs(want));
				double error = fabs(got[c] - want);
				if (exact_where_zero && (strength == 0 || d == 0) && got[c] != o[c]) error = 1e9;
				if (error > worst) worst = error;
				added += fabs(got[c] - o[c]);
				if (error > tolerance && failures++ < 3)
					printf("FAIL %s: (%d, %d) channel %d is %.5f, expected %.5f\n",
					       name, x, y, c, got[c], want);
			}
		}
	printf("%s %s: the colour moved by %.4f on average; worst error %.2g\n",
	       failures ? "FAIL" : "ok  ", name, added / (IN_W * IN_H * 3), worst);
	return failures;
}

static xess_d3d12_execute_params_t params_for(ID3D12Resource *tex, ID3D12Resource *motion,
					      const float *jitter)
{
	xess_d3d12_execute_params_t p = { .pColorTexture = tex, .pVelocityTexture = motion,
		.exposureScale = 1, .inputWidth = IN_W, .inputHeight = IN_H,
		.inputColorBase = { BASE_X, BASE_Y },
		.jitterOffsetX = jitter ? jitter[0] : 0, .jitterOffsetY = jitter ? jitter[1] : 0 };
	return p;
}

/* One case: execute on the first frame, wait for NR, execute on the second — the picture
 * moved by (sx, sy), with its own vectors (`how2`, as `motion2` was drawn) and jitter — and
 * compare what XeSS was handed with the CPU's apply pass. */
static int run(DXGI_FORMAT format, const char *name, float strength, int sx, int sy,
	       ID3D12Resource *motion1, ID3D12Resource *motion2, const float *how1, const float *how2,
	       const float *jitter1, const float *jitter2, place_fn place)
{
	UINT texel;
	int kind = kind_of(format, &texel), failures = 0;
	struct colour_how first = { kind, 0, 0 }, second = { kind, sx, sy };
	ID3D12Resource *tex1 = texture(format, W, H, texel, fill_colour, &first);
	ID3D12Resource *tex2 = sx || sy ? texture(format, W, H, texel, fill_colour, &second) : tex1;
	if (!tex1 || !tex2) {
		printf("FAIL %s: no texture\n", name);
		return 1;
	}
	nr.width = 0;                       /* start over: a rebuild, as at a resolution change */
	config.strength = strength;

	/* first execute: captured, and XeSS gets the game's own colour */
	xess_d3d12_execute_params_t params = params_for(tex1, motion1, jitter1);
	uint32_t before = nr.sequence;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (submit() || wait_change(before)) return 1;
	if (seen.pColorTexture != tex1 || seen.inputColorBase.x != BASE_X) {
		printf("FAIL %s: the first execute did not hand XeSS the game's colour\n", name);
		failures++;
	}
	uint8_t *raw;
	D3D12_RANGE all = { 0, (SIZE_T)nr.bytes }, none = { 0, 0 };
	ID3D12Resource_Map(nr.readback, 0, &all, (void **)&raw);
	for (int y = 0; y < IN_H; y++)
		for (int x = 0; x < IN_W; x++) {
			float c[4];
			uint8_t want[8];
			pattern(x + BASE_X, y + BASE_Y, c);
			pack(kind, c, want);
			if (memcmp(raw + nr.footprint.Offset + (size_t)y * nr.footprint.Footprint.RowPitch
				   + (size_t)x * texel, want, texel) && failures++ < 3)
				printf("FAIL %s: captured texel (%d, %d) differs\n", name, x, y);
		}
	ID3D12Resource_Unmap(nr.readback, 0, &none);
	static struct snapshot change;
	take(&change);

	/* second execute: XeSS gets the colour with the change added */
	params = params_for(tex2, motion2, jitter2);
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (seen.pColorTexture != nr.mixed || seen.inputColorBase.x || seen.inputColorBase.y) {
		printf("FAIL %s: the second execute did not hand XeSS the changed colour\n", name);
		return 1;
	}
	uint8_t *mixed = read_mixed();
	if (!mixed) return 1;
	static float delta[IN_W * IN_H * 3];
	struct field f = { how2, motion1 ? how1 : NULL, { jitter2 ? jitter2[0] : 0, jitter2 ? jitter2[1] : 0 },
			   { jitter1 ? jitter1[0] : 0, jitter1 ? jitter1[1] : 0 } };
	expected(&change, place, NULL, NULL, 1, kind, sx, sy, motion2 ? &f : NULL, delta);
	failures += compare(name, kind, texel, mixed, sx, sy, strength, delta, !place);
	free(mixed);
	if (tex2 != tex1) ID3D12Resource_Release(tex2);
	ID3D12Resource_Release(tex1);
	return failures != 0;
}

/* The surface at p was at p + (3, -2), and the picture moved so: its change is read there. */
static void moved_uniformly(int x, int y, float *gx, float *gy)
{
	*gx = x + 3.0f;
	*gy = y - 2.0f;
}

/* No motion, the first frame drawn half a pixel left of the second: its grid is read half a
 * pixel to the right. */
static void jittered(int x, int y, float *gx, float *gy)
{
	*gx = x + 0.5f;
	*gy = (float)y;
}

/* A block of texels whose vectors say it moved by 10 colour pixels, over a picture that did
 * not move: its pixels look where it says, see something else, and are filled from around. */
#define BLOCK_X0 50
#define BLOCK_Y0 30
#define BLOCK_X1 66     /* through the middle of the spot: half of it takes the change */
#define BLOCK_Y1 60
static void claimed(int x, int y, float *gx, float *gy)
{
	int tx = 2 * x + 1, ty = 2 * y + 1;           /* the texel nearest the pixel's centre */
	int inside = tx >= BLOCK_X0 && ty >= BLOCK_Y0 && tx < BLOCK_X1 && ty < BLOCK_Y1;
	*gx = x + (inside ? 10.0f : 0.0f);
	*gy = (float)y;
}

static void still(int x, int y, float *gx, float *gy)
{
	*gx = (float)x;
	*gy = (float)y;
}

/* Two answers that differ, and the third execute shows the first giving way to the second:
 * a third of the way, since a change came every execute or so. */
static int run_fade(ID3D12Resource *motion)
{
	UINT texel;
	int kind = kind_of(DXGI_FORMAT_R16G16B16A16_FLOAT, &texel), failures = 0;
	struct colour_how how = { kind, 0, 0 };
	ID3D12Resource *tex = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, texel, fill_colour, &how);
	nr.width = 0;
	config.strength = 1;
	static struct snapshot first, second;
	xess_d3d12_execute_params_t params = params_for(tex, motion, NULL);

	spot_level = 60;
	uint32_t before = nr.sequence;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (submit() || wait_change(before)) return 1;
	take(&first);

	spot_level = 20;
	before = nr.sequence;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (submit() || wait_change(before)) return 1;
	take(&second);

	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	float fade = (float)(nr.sequence - nr.fade_start + 1) / (nr.period > 1 ? nr.period : 1);
	if (nr.fading < 0 || fade >= 1) {
		printf("FAIL fade: the first change is not fading (fade %.2f)\n", fade);
		return 1;
	}
	uint8_t *mixed = read_mixed();
	if (!mixed) return 1;
	static float delta[IN_W * IN_H * 3];
	static const float none_drawn[6] = { 0 };
	struct field f = { none_drawn, none_drawn, { 0, 0 }, { 0, 0 } };
	expected(&second, still, &first, still, fade, kind, 0, 0, &f, delta);
	char name[80];
	snprintf(name, sizeof name, "one answer giving way to the next, %.0f%% of the way", 100 * fade);
	failures += compare(name, kind, texel, mixed, 0, 0, 1, delta, 0);
	free(mixed);
	ID3D12Resource_Release(tex);
	spot_level = 60;
	return failures != 0;
}

/* Where the second frame of `run_uncovered` sends its pixels: the moved block's by the
 * texel nearest each pixel's centre, as `claimed`. */
static void uncovered_place(int x, int y, float *gx, float *gy)
{
	int tx = 2 * x + 1, ty = 2 * y + 1;
	int inside = tx >= 2 * moved.x0 && ty >= 2 * moved.y0 && tx < 2 * moved.x1 && ty < 2 * moved.y1;
	*gx = x - (inside ? (float)moved.dx : 0.0f);
	*gy = (float)y;
}

/* A block of the picture moves on and uncovers background that looks just as the block did
 * there — as hair can look like dark wood. The vectors trace the uncovered background back
 * to the block, whose change the tracker used to carry onto it: the trail behind a moving
 * character. Now the background's pixels and the block's that came from the same place are
 * contested and filled, each from the surfaces round it that move as it does, and the strip
 * the block left takes nothing of the block's spot. */
static int run_uncovered(void)
{
	UINT texel;
	int kind = kind_of(DXGI_FORMAT_R16G16B16A16_FLOAT, &texel), failures = 0;
	struct colour_how how = { kind, 0, 0 };
	ID3D12Resource *first = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, texel, fill_colour, &how);
	moved.x0 = 32;          /* from 26-41 to 32-47, the spot's 30-35 on it */
	moved.y0 = 14;
	moved.x1 = 48;
	moved.y1 = 28;
	moved.dx = 6;
	moved.dy = 0;
	ID3D12Resource *second = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, texel, fill_colour, &how);
	/* In texels: the block's pixels were 6 pixels to their left, on both frames — it was
	 * moving already, as a character walking on is. */
	float drawn1[6] = { -12, 0, (float)(2 * (moved.x0 - moved.dx)), (float)(2 * moved.y0),
			    (float)(2 * (moved.x1 - moved.dx)), (float)(2 * moved.y1) };
	float drawn[6] = { -12, 0, (float)(2 * moved.x0), (float)(2 * moved.y0), (float)(2 * moved.x1),
			   (float)(2 * moved.y1) };
	ID3D12Resource *motion1 = texture(DXGI_FORMAT_R16G16_FLOAT, MOTION_W, MOTION_H, 4, fill_motion, drawn1);
	ID3D12Resource *motion = texture(DXGI_FORMAT_R16G16_FLOAT, MOTION_W, MOTION_H, 4, fill_motion, drawn);
	if (!first || !second || !motion1 || !motion) {
		printf("FAIL uncovered: no texture\n");
		memset(&moved, 0, sizeof moved);
		return 1;
	}
	nr.width = 0;
	config.strength = 1;
	spot_level = 60;
	xess_d3d12_execute_params_t params = params_for(first, motion1, NULL);
	uint32_t before = nr.sequence;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	uint8_t *mixed = NULL;
	static struct snapshot change;
	if (!submit() && !wait_change(before)) {
		take(&change);
		params = params_for(second, motion, NULL);
		xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
		mixed = read_mixed();
	}
	if (!mixed) {
		memset(&moved, 0, sizeof moved);
		return 1;
	}
	static float delta[IN_W * IN_H * 3];
	struct field f = { drawn, drawn1, { 0, 0 }, { 0, 0 } };
	expected(&change, uncovered_place, NULL, NULL, 1, kind, 0, 0, &f, delta);
	failures += compare("a block moving on over background that looks as it did", kind, texel, mixed,
			    0, 0, 1, delta, 0);
	double leaked = 0;
	for (int y = SPOT_Y; y < SPOT_Y + 6; y++)
		for (int x = moved.x0 - moved.dx; x < moved.x0; x++) {
			float o[4], got[4];
			colour_at(kind, x, y, 0, 0, o);
			load_texel(mixed + nr.footprint.Offset + (size_t)y * nr.footprint.Footprint.RowPitch
				   + (size_t)x * texel, kind, got);
			for (int c = 0; c < 3; c++) leaked = fmax(leaked, fabs(got[c] - o[c]));
		}
	printf("%s the strip it uncovered takes %.4f of its spot\n", leaked < 0.01 ? "ok  " : "FAIL", leaked);
	failures += leaked >= 0.01;
	free(mixed);
	memset(&moved, 0, sizeof moved);
	ID3D12Resource_Release(motion);
	ID3D12Resource_Release(motion1);
	ID3D12Resource_Release(second);
	ID3D12Resource_Release(first);
	return failures != 0;
}

/* A cut, and the first change after it comes in over the period a change takes to come
 * rather than all at once. */
static int run_rise(ID3D12Resource *still_motion)
{
	UINT texel;
	int kind = kind_of(DXGI_FORMAT_R16G16B16A16_FLOAT, &texel);
	struct colour_how how = { kind, 0, 0 };
	ID3D12Resource *tex = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, texel, fill_colour, &how);
	if (!tex) {
		printf("FAIL rise: no texture\n");
		return 1;
	}
	nr.width = 0;
	config.strength = 1;
	spot_level = 60;
	xess_d3d12_execute_params_t params = params_for(tex, still_motion, NULL);
	params.resetHistory = 1;
	uint32_t before = nr.sequence;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (submit() || wait_change(before)) return 1;
	static struct snapshot change;
	take(&change);
	params.resetHistory = 0;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	float rise = (float)(nr.sequence - nr.rise_start + 1) / (nr.period > 1 ? nr.period : 1);
	if (!(rise < 1)) {
		printf("FAIL rise: the first change after a cut came in whole (%.2f)\n", rise);
		return 1;
	}
	uint8_t *mixed = read_mixed();
	if (!mixed) return 1;
	static float delta[IN_W * IN_H * 3];
	static const float none_drawn[6] = { 0 };
	struct field f = { none_drawn, none_drawn, { 0, 0 }, { 0, 0 } };
	expected(&change, still, NULL, NULL, 1, kind, 0, 0, &f, delta);
	char name[80];
	snprintf(name, sizeof name, "after a cut, the first change %.0f%% of the way in", 100 * rise);
	int failures = compare(name, kind, texel, mixed, 0, 0, rise, delta, 0);
	free(mixed);
	ID3D12Resource_Release(tex);
	return failures != 0;
}

/* `record=2`: two frames on disk as the game handed them, the colour's rows and then the
 * vectors' rows, packed tight. In R11G11B10, whose rows a copy pads: the game's format, and
 * the one whose colour went missing from the first recording out of Hogwarts. */
static int run_record(ID3D12Resource *motion)
{
	UINT texel;
	int kind = kind_of(DXGI_FORMAT_R11G11B10_FLOAT, &texel), failures = 0;
	struct colour_how how = { kind, 0, 0 };
	ID3D12Resource *tex = texture(DXGI_FORMAT_R11G11B10_FLOAT, W, H, texel, fill_colour, &how);
	if (!tex) {
		printf("FAIL recording: no texture\n");
		return 1;
	}
	nr.width = 0;
	snprintf(config.record_dir, sizeof config.record_dir, "test_nr_xess_recording");
	InterlockedExchange(&record_asked, 2);
	xess_d3d12_execute_params_t params = params_for(tex, motion, NULL);
	for (int i = 0; i < 2; i++) {
		xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
		if (submit()) return 1;
	}
	for (ULONGLONG until = GetTickCount64() + 10000;
	     GetTickCount64() < until && (rec.left || rec.slot[0].full || rec.slot[1].full); Sleep(5)) {}

	size_t colour_bytes = (size_t)IN_W * IN_H * texel, motion_bytes = (size_t)MOTION_W * MOTION_H * 4;
	uint8_t *data = malloc(colour_bytes + motion_bytes + 1);
	uint16_t vector[2] = { to_half(6), to_half(-4) };
	wchar_t path[MAX_PATH + 32];
	for (int frame = 0; frame < 2; frame++) {
		_snwprintf(path, MAX_PATH + 32, L"%ls\\%04d.bin", rec.dir, frame);
		FILE *file = _wfopen(path, L"rb");
		size_t got = file ? fread(data, 1, colour_bytes + motion_bytes + 1, file) : 0;
		if (file) fclose(file);
		if (got != colour_bytes + motion_bytes) {
			printf("FAIL recording: frame %d holds %zu bytes, not %zu\n", frame, got,
			       colour_bytes + motion_bytes);
			failures++;
			continue;
		}
		for (int y = 0; y < IN_H; y++)
			for (int x = 0; x < IN_W; x++) {
				float c[4];
				uint8_t want[8];
				pattern(x + BASE_X, y + BASE_Y, c);
				pack(kind, c, want);
				if (memcmp(data + ((size_t)y * IN_W + x) * texel, want, texel) && failures++ < 3)
					printf("FAIL recording: frame %d's texel (%d, %d) differs\n", frame, x, y);
			}
		for (size_t i = 0; i < (size_t)MOTION_W * MOTION_H; i++)
			if (memcmp(data + colour_bytes + i * 4, vector, 4) && failures++ < 3)
				printf("FAIL recording: frame %d's vector %zu differs\n", frame, i);
	}
	free(data);
	char line[160] = "";
	_snwprintf(path, MAX_PATH + 32, L"%ls\\meta.txt", rec.dir);
	FILE *meta = _wfopen(path, L"r");
	if (meta) {
		if (!fgets(line, sizeof line, meta)) line[0] = '\0';
		fclose(meta);
	}
	char want[64];
	snprintf(want, sizeof want, "colour %d %d %d %u\n", IN_W, IN_H, (int)DXGI_FORMAT_R11G11B10_FLOAT, texel);
	if (strcmp(line, want) && failures++ < 3) printf("FAIL recording: meta.txt begins \"%s\"\n", line);

	const wchar_t *names[] = { L"0000.bin", L"0001.bin", L"frames.txt", L"meta.txt" };
	for (int i = 0; i < 4; i++) {
		_snwprintf(path, MAX_PATH + 32, L"%ls\\%ls", rec.dir, names[i]);
		DeleteFileW(path);
	}
	RemoveDirectoryW(rec.dir);
	RemoveDirectoryW(L"test_nr_xess_recording");
	config.record_dir[0] = '\0';
	ID3D12Resource_Release(tex);
	printf("%s recording: two frames of %dx%d R11G11B10 colour and %dx%d vectors, as handed\n",
	       failures ? "FAIL" : "ok  ", IN_W, IN_H, MOTION_W, MOTION_H);
	return failures != 0;
}

/* ---- mode=cut ---- */

/* The cut on its own: what the list had bound before it holds after it, and the work handed to
 * the queue between the halves runs after the first and before the second. */
struct between {
	ID3D12GraphicsCommandList *list;
	int ran, discarded;
};

static void between_run(void *user, ID3D12CommandQueue *queue)
{
	struct between *b = user;
	ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&b->list);
	b->ran++;
}

static void between_discard(void *user)
{
	((struct between *)user)->discarded++;
}

static ID3D12Resource *word_buffer(D3D12_HEAP_TYPE heap, uint32_t value)
{
	ID3D12Resource *buffer = make_buffer(heap, 256, D3D12_RESOURCE_FLAG_NONE,
					     heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
					     : D3D12_RESOURCE_STATE_COPY_DEST);
	uint32_t *mapped;
	if (buffer && heap == D3D12_HEAP_TYPE_UPLOAD && SUCCEEDED(ID3D12Resource_Map(buffer, 0, NULL, (void **)&mapped))) {
		*mapped = value;
		ID3D12Resource_Unmap(buffer, 0, NULL);
	}
	return buffer;
}

static int run_split(void)
{
	char why[160];
	/* The first call hooks the classes; the list's state is followed from its next Reset. */
	nr_split_prepare(gpu.list, why, sizeof why);
	if (submit()) return 1;
	if (!nr_split_prepare(gpu.list, why, sizeof why)) {
		printf("FAIL cut: the list cannot be cut: %s\n", why);
		return 1;
	}
	/* What ENCODE reads: an 8x8 RGBA16F picture below the knee, and where it writes. */
	enum { SIDE = 8 };
	ID3D12Resource *picture = make_buffer(D3D12_HEAP_TYPE_UPLOAD, SIDE * SIDE * 8, D3D12_RESOURCE_FLAG_NONE,
					      D3D12_RESOURCE_STATE_GENERIC_READ);
	ID3D12Resource *out = make_buffer(D3D12_HEAP_TYPE_DEFAULT, SIDE * SIDE * 4,
					  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
	ID3D12Resource *marker = make_buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_NONE,
					     D3D12_RESOURCE_STATE_COMMON);
	ID3D12Resource *one = word_buffer(D3D12_HEAP_TYPE_UPLOAD, 1), *two = word_buffer(D3D12_HEAP_TYPE_UPLOAD, 2);
	ID3D12Resource *readback = make_buffer(D3D12_HEAP_TYPE_READBACK, SIDE * SIDE * 4 + 256,
					       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!picture || !out || !marker || !one || !two || !readback) {
		printf("FAIL cut: no buffers\n");
		return 1;
	}
	uint16_t *texels;
	ID3D12Resource_Map(picture, 0, NULL, (void **)&texels);
	for (int i = 0; i < SIDE * SIDE; i++) {
		float c[4] = { (i % SIDE) / 16.0f, (i / SIDE) / 16.0f, 0.25f, 1 };
		for (int k = 0; k < 4; k++) texels[4 * i + k] = to_half(c[k]);
	}
	ID3D12Resource_Unmap(picture, 0, NULL);
	struct between b = { .ran = 0 };
	ID3D12CommandAllocator *allocator;
	ID3D12Device_CreateCommandAllocator(gpu.device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
					    (void **)&allocator);
	ID3D12Device_CreateCommandList(gpu.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
				       &IID_ID3D12GraphicsCommandList, (void **)&b.list);
	ID3D12GraphicsCommandList_CopyBufferRegion(b.list, marker, 0, two, 0, 4);
	ID3D12GraphicsCommandList_Close(b.list);

	/* Bound before the cut, used after it with nothing bound again. */
	struct cut_constants k = { .width = SIDE, .height = SIDE, .pitch = SIDE * 8, .kind = KIND_RGBA16F, .white = 1 };
	ID3D12GraphicsCommandList_SetComputeRootSignature(gpu.list, nr.signature);
	ID3D12GraphicsCommandList_SetPipelineState(gpu.list, nr.encode_pipeline);
	ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(gpu.list, 0, sizeof k / 4, &k, 0);
	ID3D12Resource *reads[5] = { picture, nr.srgb_table, picture, picture, picture };
	for (UINT i = 0; i < 5; i++)
		ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(gpu.list, 1 + i,
			ID3D12Resource_GetGPUVirtualAddress(reads[i]));
	ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(gpu.list, 6, ID3D12Resource_GetGPUVirtualAddress(out));
	ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, marker, 0, one, 0, 4);
	nr_split_snapshot *state = nr_split_snapshot_take(gpu.list);
	if (!state || !nr_split_cut(gpu.list, between_run, between_discard, &b, state, why, sizeof why)) {
		printf("FAIL cut: %s\n", state ? why : "no snapshot");
		return 1;
	}
	nr_split_snapshot_free(state);
	D3D12_RESOURCE_BARRIER barrier = transition(out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &barrier);
	ID3D12GraphicsCommandList_Dispatch(gpu.list, 1, 1, 1);
	barrier = transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &barrier);
	ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, readback, 0, out, 0, SIDE * SIDE * 4);
	ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, readback, SIDE * SIDE * 4, marker, 0, 4);
	if (submit()) return 1;

	int failures = 0;
	uint8_t *got;
	ID3D12Resource_Map(readback, 0, NULL, (void **)&got);
	uint32_t mark;
	memcpy(&mark, got + SIDE * SIDE * 4, 4);
	if (mark != 2 || b.ran != 1 || b.discarded) {
		printf("FAIL cut: the work between the halves %s (marker %u, ran %d, discarded %d)\n",
		       mark == 1 ? "ran before the first half" : "did not run between them", mark, b.ran, b.discarded);
		failures++;
	}
	for (int i = 0; i < SIDE * SIDE; i++) {
		float rgb[3] = { half_value[to_half((i % SIDE) / 16.0f)], half_value[to_half((i / SIDE) / 16.0f)],
				 half_value[to_half(0.25f)] };
		uint8_t want[4];
		encode_proxy(rgb, 1, want);
		if (memcmp(got + 4 * i, want, 4) && failures++ < 3)
			printf("FAIL cut: after it, pixel %d is %02x%02x%02x, not %02x%02x%02x: the list's state was "
			       "not carried over\n", i, got[4 * i], got[4 * i + 1], got[4 * i + 2], want[0], want[1], want[2]);
	}
	ID3D12Resource_Unmap(readback, 0, NULL);
	printf("%s cut: the state bound before it holds after it, and the work between the halves runs "
	       "between them\n", failures ? "FAIL" : "ok  ");
	ID3D12GraphicsCommandList_Release(b.list);
	ID3D12CommandAllocator_Release(allocator);
	ID3D12Resource *made[] = { picture, out, marker, one, two, readback };
	for (int i = 0; i < 6; i++) ID3D12Resource_Release(made[i]);
	return failures != 0;
}

/* A stand-in for the daemon's protocol 3, on its own queue: it waits there for a frame's V,
 * puts `answer` and the stamp naming V into the shared output — nothing when declining — and
 * signals V + 1. A frame of a session it has not taken fails, as the daemon's does, with the
 * fence left where it is; `down` answers nothing, as no daemon at all; `dies` ends it with the
 * next frame, unanswered and the fence at UINT64_MAX, as a daemon's process ending left it
 * with Intel's driver. */
static struct {
	ID3D12CommandQueue *queue;
	ID3D12CommandAllocator *allocator;
	ID3D12GraphicsCommandList *list;
	ID3D12Resource *upload;
	uint8_t answer[IN_W * IN_H * 4];
	int decline, down, dies, frames, hellos;
	uint64_t input_alloc, previous;   /* the HELLO's input, the last FRAME's motion */
	uint64_t session, released;       /* the session taken, the last one released */
} stand;

static int stand_in_gpu(const struct nr_gpu_packet *p, struct nr_gpu_reply *reply)
{
	memset(reply, 0, sizeof *reply);
	reply->session_low = (uint32_t)p->session;
	reply->session_high = (uint32_t)(p->session >> 32);
	if (p->action == NR_GPU_RELEASE) stand.released = p->session;
	if (stand.down) return 0;
	if (p->action == NR_GPU_HELLO) {
		stand.hellos++;
		stand.input_alloc = p->input_alloc;
		reply->detail = GetCurrentProcessId();
		int taken = p->bytes == sizeof stand.answer && p->version == NR_GPU_D3D12_VERSION;
		if (taken) stand.session = p->session;
		return taken;
	}
	if (p->action != NR_GPU_FRAME) {
		if (p->session == stand.session) stand.session = 0;
		return 1;
	}
	if (stand.dies) {
		stand.dies = 0;
		stand.session = 0;
		HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
		ID3D12Fence_SetEventOnCompletion(cut.gpu.fence, p->done_sem, event);
		WaitForSingleObject(event, 10000);
		CloseHandle(event);
		ID3D12Fence_Signal(cut.gpu.fence, UINT64_MAX);
		return 0;
	}
	if (p->session != stand.session) {
		reply->status = NR_GPU_FAILURE;
		return 1;
	}
	uint64_t value = p->done_sem;
	memcpy(&stand.previous, p->driver_uuid, sizeof stand.previous);
	ID3D12CommandAllocator_Reset(stand.allocator);
	ID3D12GraphicsCommandList_Reset(stand.list, stand.allocator, NULL);
	if (!stand.decline) {
		uint8_t *mapped;
		ID3D12Resource_Map(stand.upload, 0, NULL, (void **)&mapped);
		memcpy(mapped, stand.answer, sizeof stand.answer);
		struct nr_gpu_stamp stamp = { NR_GPU_STAMP_MAGIC, (uint32_t)value, (uint32_t)(value >> 32), 0 };
		memcpy(mapped + sizeof stand.answer, &stamp, sizeof stamp);
		ID3D12Resource_Unmap(stand.upload, 0, NULL);
		ID3D12GraphicsCommandList_CopyBufferRegion(stand.list, cut.gpu.output, 0, stand.upload, 0,
							   sizeof stand.answer + sizeof stamp);
	}
	ID3D12GraphicsCommandList_Close(stand.list);
	ID3D12CommandQueue_Wait(stand.queue, cut.gpu.fence, value);
	ID3D12CommandQueue_ExecuteCommandLists(stand.queue, 1, (ID3D12CommandList **)&stand.list);
	ID3D12CommandQueue_Signal(stand.queue, cut.gpu.fence, value + 1);
	HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
	ID3D12Fence_SetEventOnCompletion(cut.gpu.fence, value + 1, event);
	int done = WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
	CloseHandle(event);
	stand.frames++;
	reply->status = stand.decline ? NR_GPU_UNSUPPORTED : NR_GPU_SUCCESS;
	return done;
}

/* An execute in mode=cut with the stand-in: the frame NR is given as the CPU encodes it, the
 * stamp naming the frame's V, and the colour XeSS is handed against the colour plus NR's change
 * to this very frame — or the colour unchanged, declined or at strength 0. */
static int run_cut(DXGI_FORMAT format, const char *name, float strength, int decline)
{
	UINT texel;
	int kind = kind_of(format, &texel), failures = 0;
	struct colour_how how = { kind, 0, 0 };
	ID3D12Resource *tex = texture(format, W, H, texel, fill_colour, &how);
	if (!tex) {
		printf("FAIL %s: no texture\n", name);
		return 1;
	}
	nr.width = 0;                       /* start over: a new session, as at a resolution change */
	config.strength = strength;
	stand.decline = decline;
	static uint8_t given[IN_W * IN_H * 4];
	for (int y = 0; y < IN_H; y++)
		for (int x = 0; x < IN_W; x++) {
			float o[4];
			colour_at(kind, x, y, 0, 0, o);
			encode_proxy(o, 1 / config.white, given + ((size_t)y * IN_W + x) * 4);
		}
	brighten(given, NULL, stand.answer, IN_W, IN_H);
	xess_d3d12_execute_params_t params = params_for(tex, NULL, NULL);
	/* The first execute of a colour closes the last session, opens one and goes to XeSS whole. */
	for (int tries = 0; tries == 0 || (tries < 250 && cut.hello != 2); tries++) {
		xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
		if (submit()) return 1;
		Sleep(20);
	}
	if (cut.hello != 2) {
		printf("FAIL %s: the session was not opened\n", name);
		return 1;
	}
	int before = stand.frames;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	uint64_t value = cut.last;
	if (seen.pColorTexture != nr.mixed || seen.inputColorBase.x || seen.inputColorBase.y) {
		printf("FAIL %s: XeSS was not handed the composed colour (%s)\n", name, cut.said);
		return 1;
	}
	/* Read back, in the continuation: the colour XeSS got, and the two shared buffers. */
	UINT64 shared = (UINT64)IN_W * IN_H * 4;
	ID3D12Resource *readback = make_buffer(D3D12_HEAP_TYPE_READBACK, nr.bytes + 2 * shared + 16,
					       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_RESOURCE_BARRIER b = transition(nr.mixed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					      D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
	D3D12_TEXTURE_COPY_LOCATION to = { .pResource = readback, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
					   .PlacedFootprint = nr.footprint };
	D3D12_TEXTURE_COPY_LOCATION from = { .pResource = nr.mixed, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	ID3D12GraphicsCommandList_CopyTextureRegion(gpu.list, &to, 0, 0, 0, &from, NULL);
	b = transition(nr.mixed, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
	ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, readback, nr.bytes, cut.gpu.input, 0, shared);
	ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, readback, nr.bytes + shared, cut.gpu.output, 0, shared + 16);
	if (submit()) return 1;
	if (stand.frames != before + 1) {
		printf("FAIL %s: the stand-in was handed %d frames, not one\n", name, stand.frames - before);
		failures++;
	}
	uint8_t *raw;
	ID3D12Resource_Map(readback, 0, NULL, (void **)&raw);
	const uint8_t *gpu_given = raw + nr.bytes, *answer = raw + nr.bytes + shared;
	/* The GPU's exp is not the CPU's to the last bit: a level apart now and then, no more. */
	int off = 0;
	for (size_t i = 0; i < shared; i++) {
		if (i % 4 == 3) continue;
		int d = abs((int)gpu_given[i] - (int)given[i]);
		off += d != 0;
		if (d > 1 && failures++ < 3)
			printf("FAIL %s: the GPU encoded byte %zu as %u, the CPU as %u\n", name, i, gpu_given[i], given[i]);
	}
	struct nr_gpu_stamp stamp;
	memcpy(&stamp, answer + shared, sizeof stamp);
	if (nr_gpu_d3d12_answered(&stamp, value) == decline) {
		printf("FAIL %s: the stamp %s frame %llu\n", name, decline ? "names the declined" : "does not name",
		       (unsigned long long)value);
		failures++;
	}
	static float delta[IN_W * IN_H * 3];
	for (int i = 0; i < IN_W * IN_H; i++)
		for (int c = 0; c < 3; c++)
			delta[3 * i + c] = decline ? 0 : linear8[answer[4 * i + c]] - linear8[gpu_given[4 * i + c]];
	uint8_t *mixed = malloc((size_t)nr.bytes);
	memcpy(mixed, raw, (size_t)nr.bytes);
	ID3D12Resource_Unmap(readback, 0, NULL);
	ID3D12Resource_Release(readback);
	if (off) printf("     %s: %d of %zu channels the GPU encoded a level away from the CPU\n", name, off, shared / 4 * 3);
	failures += compare(name, kind, texel, mixed, 0, 0, strength, delta, 1);
	free(mixed);
	ID3D12Resource_Release(tex);
	return failures != 0;
}

/* mode=cut's motion: of two executes in a row, the second hands the daemon, after the frame,
 * each pixel's offset to where its surface was on the first — the game's vector, (3, -2)
 * pixels here, and the two frames' jitter — NaN where that was off screen, and names the first
 * frame in its FRAME. The first names none. */
static int run_cut_motion(ID3D12Resource *motion)
{
	UINT texel;
	int kind = kind_of(DXGI_FORMAT_R16G16B16A16_FLOAT, &texel), failures = 0;
	struct colour_how how = { kind, 0, 0 };
	ID3D12Resource *tex = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, texel, fill_colour, &how);
	if (!tex) {
		printf("FAIL cut, motion: no texture\n");
		return 1;
	}
	nr.width = 0;
	config.strength = 1;
	stand.decline = 0;
	float first[2] = { 0.25f, -0.25f }, second[2] = { -0.25f, 0.5f };
	xess_d3d12_execute_params_t params = params_for(tex, motion, first);
	for (int tries = 0; tries == 0 || (tries < 250 && cut.hello != 2); tries++) {
		xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
		if (submit()) return 1;
		Sleep(20);
	}
	if (cut.hello != 2) {
		printf("FAIL cut, motion: the session was not opened\n");
		return 1;
	}
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (submit()) return 1;
	uint64_t earlier = cut.last, named_first = stand.previous;
	params = params_for(tex, motion, second);
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	UINT64 bytes = NR_GPU_MOTION_BYTES(IN_W, IN_H);
	ID3D12Resource *readback = make_buffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE,
					       D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, readback, 0, cut.gpu.input, cut.gpu.motion, bytes);
	if (submit()) return 1;
	if (stand.input_alloc != (UINT64)IN_W * IN_H * 4 + bytes) {
		printf("FAIL cut, motion: the session's input is %llu bytes, with no room for the offsets\n",
		       (unsigned long long)stand.input_alloc);
		failures++;
	}
	if (named_first != 0 || !earlier || stand.previous != earlier) {
		printf("FAIL cut, motion: the frames named %llu and %llu, not 0 and %llu\n",
		       (unsigned long long)named_first, (unsigned long long)stand.previous,
		       (unsigned long long)earlier);
		failures++;
	}
	float *offsets;
	ID3D12Resource_Map(readback, 0, NULL, (void **)&offsets);
	float dx = 3 + first[0] - second[0], dy = -2 + first[1] - second[1];
	int wrong = 0, known = 0;
	for (int y = 0; y < IN_H; y++)
		for (int x = 0; x < IN_W; x++) {
			const float *o = offsets + 2 * ((size_t)y * IN_W + x);
			int off = x + dx < -0.5f || y + dy < -0.5f || x + dx > IN_W - 0.5f || y + dy > IN_H - 0.5f;
			int right = off ? isnan(o[0]) && isnan(o[1]) : o[0] == dx && o[1] == dy;
			known += !off;
			if (!right && wrong++ < 3)
				printf("FAIL cut, motion: pixel (%d, %d) has (%g, %g)\n", x, y, o[0], o[1]);
		}
	ID3D12Resource_Unmap(readback, 0, NULL);
	ID3D12Resource_Release(readback);
	ID3D12Resource_Release(tex);
	failures += wrong != 0;
	printf("%s cut, motion: (%.2f, %.2f) to the frame before over %d pixels, the rest off screen\n",
	       failures ? "FAIL" : "ok  ", dx, dy, known);
	return failures != 0;
}

/* mode=after: XeSS's output, as XeSS left it, goes to the stand-in between the halves, and the
 * output the game goes on with is it plus NR's change to it, while XeSS still got the game's
 * colour. Here the output is the colour's extent and the stand-in XeSS leaves it as the test
 * filled it, so the CPU's answer is run_cut's. */
static int run_after(DXGI_FORMAT format, const char *name)
{
	UINT texel;
	int kind = kind_of(format, &texel), failures = 0;
	struct colour_how how = { kind, 0, 0 };
	ID3D12Resource *tex = texture(format, W, H, texel, fill_colour, &how);
	ID3D12Resource *out = texture_as(format, W, H, texel, fill_colour, &how,
					 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (!tex || !out) {
		printf("FAIL %s: no texture\n", name);
		return 1;
	}
	xess_d3d12_init_params_t init = { .outputResolution = { IN_W, IN_H } };
	xessD3D12Init((xess_context_handle_t)1, &init);
	nr.width = 0;
	config.strength = 1;
	stand.decline = 0;
	static uint8_t given[IN_W * IN_H * 4];
	for (int y = 0; y < IN_H; y++)
		for (int x = 0; x < IN_W; x++) {
			float o[4];
			colour_at(kind, x, y, 0, 0, o);
			encode_proxy(o, 1 / config.white, given + ((size_t)y * IN_W + x) * 4);
		}
	brighten(given, NULL, stand.answer, IN_W, IN_H);
	xess_d3d12_execute_params_t params = params_for(tex, NULL, NULL);
	params.pOutputTexture = out;
	params.outputColorBase.x = BASE_X;
	params.outputColorBase.y = BASE_Y;
	for (int tries = 0; tries == 0 || (tries < 250 && cut.hello != 2); tries++) {
		xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
		if (submit()) return 1;
		Sleep(20);
	}
	if (cut.hello != 2) {
		printf("FAIL %s: the session was not opened\n", name);
		return 1;
	}
	int before = stand.frames;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	uint64_t value = cut.last;
	if (seen.pColorTexture != tex || seen.pOutputTexture != out || !cut_now) {
		printf("FAIL %s: XeSS was not handed the game's own textures, or the frame went on without NR (%s)\n",
		       name, cut.said);
		return 1;
	}
	/* Read back, after the continuation: XeSS's output as the game goes on with it, and the two
	 * shared buffers. */
	UINT64 shared = (UINT64)IN_W * IN_H * 4;
	ID3D12Resource *readback = make_buffer(D3D12_HEAP_TYPE_READBACK, nr.bytes + 2 * shared + 16,
					       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_RESOURCE_BARRIER b = transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
					      D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
	D3D12_BOX box = { BASE_X, BASE_Y, 0, BASE_X + IN_W, BASE_Y + IN_H, 1 };
	D3D12_TEXTURE_COPY_LOCATION to = { .pResource = readback, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
					   .PlacedFootprint = nr.footprint };
	D3D12_TEXTURE_COPY_LOCATION from = { .pResource = out, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	ID3D12GraphicsCommandList_CopyTextureRegion(gpu.list, &to, 0, 0, 0, &from, &box);
	b = transition(out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
	ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, readback, nr.bytes, cut.gpu.input, 0, shared);
	ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, readback, nr.bytes + shared, cut.gpu.output, 0, shared + 16);
	if (submit()) return 1;
	if (stand.frames != before + 1) {
		printf("FAIL %s: the stand-in was handed %d frames, not one\n", name, stand.frames - before);
		failures++;
	}
	uint8_t *raw;
	ID3D12Resource_Map(readback, 0, NULL, (void **)&raw);
	const uint8_t *gpu_given = raw + nr.bytes, *answer = raw + nr.bytes + shared;
	int off = 0;
	for (size_t i = 0; i < shared; i++) {
		if (i % 4 == 3) continue;
		int d = abs((int)gpu_given[i] - (int)given[i]);
		off += d != 0;
		if (d > 1 && failures++ < 3)
			printf("FAIL %s: the GPU encoded byte %zu as %u, the CPU as %u\n", name, i, gpu_given[i], given[i]);
	}
	struct nr_gpu_stamp stamp;
	memcpy(&stamp, answer + shared, sizeof stamp);
	if (!nr_gpu_d3d12_answered(&stamp, value)) {
		printf("FAIL %s: the stamp does not name frame %llu\n", name, (unsigned long long)value);
		failures++;
	}
	static float delta[IN_W * IN_H * 3];
	for (int i = 0; i < IN_W * IN_H; i++)
		for (int c = 0; c < 3; c++)
			delta[3 * i + c] = linear8[answer[4 * i + c]] - linear8[gpu_given[4 * i + c]];
	uint8_t *mixed = malloc((size_t)nr.bytes);
	memcpy(mixed, raw, (size_t)nr.bytes);
	ID3D12Resource_Unmap(readback, 0, NULL);
	ID3D12Resource_Release(readback);
	if (off) printf("     %s: %d of %zu channels the GPU encoded a level away from the CPU\n", name, off, shared / 4 * 3);
	failures += compare(name, kind, texel, mixed, 0, 0, 1, delta, 1);
	free(mixed);
	ID3D12Resource_Release(out);
	ID3D12Resource_Release(tex);
	return failures != 0;
}

/* The daemon restarting under the game: it ends with the frame it is sent, unanswered and the
 * fence at UINT64_MAX, and the game's queue goes on all the same; for 5 s frames go on without
 * NR; then the next session, on the same buffers with a new fence: the old one released, its
 * HELLO unanswered while no daemon is there and taken once one is, and its frames answered, V
 * still rising, the first naming no frame of the old session's and the second the first. */
static int run_reopen(ID3D12Resource *motion)
{
	UINT texel;
	int kind = kind_of(DXGI_FORMAT_R16G16B16A16_FLOAT, &texel), failures = 0;
	struct colour_how how = { kind, 0, 0 };
	ID3D12Resource *tex = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, texel, fill_colour, &how);
	ID3D12Resource *out = texture_as(DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, texel, fill_colour, &how,
					 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	ID3D12Resource *readback = make_buffer(D3D12_HEAP_TYPE_READBACK, sizeof(struct nr_gpu_stamp),
					       D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!tex || !out || !readback) {
		printf("FAIL reopen: no texture\n");
		return 1;
	}
	nr.width = 0;
	config.strength = 1;
	stand.decline = 0;
	xess_d3d12_execute_params_t params = params_for(tex, motion, NULL);
	params.pOutputTexture = out;
	params.outputColorBase.x = BASE_X;
	params.outputColorBase.y = BASE_Y;
	for (int tries = 0; tries == 0 || (tries < 250 && cut.hello != 2); tries++) {
		xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
		if (submit()) return 1;
		Sleep(20);
	}
	if (cut.hello != 2) {
		printf("FAIL reopen: the session was not opened\n");
		return 1;
	}
	uint64_t old = cut.session;
	int frames = stand.frames;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (submit()) return 1;
	uint64_t first = cut.last;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (submit()) return 1;
	if (stand.frames != frames + 2 || stand.previous != first) {
		printf("FAIL reopen: before the restart %d frames were answered, the second naming %llu, not %llu\n",
		       stand.frames - frames, (unsigned long long)stand.previous, (unsigned long long)first);
		failures++;
	}

	stand.dies = 1;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	uint64_t lost = cut.last;
	if (submit()) {
		printf("FAIL reopen: the game's queue was left waiting for the unanswered frame\n");
		return 1;
	}
	if (!cut.broken || stand.frames != frames + 2) {
		printf("FAIL reopen: the unanswered frame %llu did not end the session\n", (unsigned long long)lost);
		failures++;
	}
	int hellos = stand.hellos;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	int paused = !cut_now;
	if (submit()) return 1;
	if (!paused || stand.hellos != hellos || stand.frames != frames + 2) {
		printf("FAIL reopen: within the pause a frame was cut, or the daemon was asked (%s)\n", cut.said);
		failures++;
	}

	stand.down = 1;                     /* the pause over, and no daemon yet */
	cut.retry = 0;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (submit()) return 1;
	for (ULONGLONG until = GetTickCount64() + 5000; GetTickCount64() < until && (cut.hello == 1 || cut.releasing);
	     Sleep(5)) {}
	if (stand.released != old || !cut.session || cut.session == old || cut.hello != 0
	    || cut.retry <= GetTickCount64()) {
		printf("FAIL reopen: with no daemon, the old session was %sreleased, and the next one (%s) %s\n",
		       stand.released == old ? "" : "not ", cut.session && cut.session != old ? "new" : "not new",
		       cut.hello == 0 && cut.retry > GetTickCount64() ? "is asked for again later" : "is not asked for again");
		failures++;
	}
	UINT64 fence = ID3D12Fence_GetCompletedValue(cut.gpu.fence);
	if (fence != lost + 1) {
		printf("FAIL reopen: the next session's fence is at %llu, not where the last one left off (%llu)\n",
		       (unsigned long long)fence, (unsigned long long)lost + 1);
		failures++;
	}

	stand.down = 0;                     /* a daemon there again */
	cut.retry = 0;
	for (int tries = 0; tries == 0 || (tries < 250 && cut.hello != 2); tries++) {
		xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
		if (submit()) return 1;
		Sleep(20);
	}
	if (cut.hello != 2 || stand.session != cut.session || stand.hellos != hellos + 1) {
		printf("FAIL reopen: the next session was not taken\n");
		return 1;
	}
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	uint64_t next = cut.last;
	int answered = cut_now;
	ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, readback, 0, cut.gpu.output, cut.gpu.bytes,
						   sizeof(struct nr_gpu_stamp));
	if (submit()) return 1;
	uint64_t named = stand.previous;
	xessD3D12Execute((xess_context_handle_t)1, gpu.list, &params);
	if (submit()) return 1;
	struct nr_gpu_stamp *stamp;
	ID3D12Resource_Map(readback, 0, NULL, (void **)&stamp);
	answered = answered && nr_gpu_d3d12_answered(stamp, next);
	ID3D12Resource_Unmap(readback, 0, NULL);
	if (!answered || stand.frames != frames + 4 || next <= lost || named != 0 || stand.previous != next) {
		printf("FAIL reopen: in the next session frame %llu was %sanswered (%d frames), naming %llu, and the "
		       "one after it naming %llu\n", (unsigned long long)next, answered ? "" : "not ",
		       stand.frames - frames, (unsigned long long)named, (unsigned long long)stand.previous);
		failures++;
	}
	ID3D12Resource_Release(readback);
	ID3D12Resource_Release(out);
	ID3D12Resource_Release(tex);
	printf("%s after XeSS, the daemon restarting: frame %llu unanswered, the next session taken after "
	       "the pause on a new fence, frame %llu answered\n", failures ? "FAIL" : "ok  ", (unsigned long long)lost,
	       (unsigned long long)next);
	return failures != 0;
}

int main(int argc, char **argv)
{
	_snwprintf(log_path, MAX_PATH + 16, L"test_nr_xess.log");
	build_tables();
	nr_split_set_log(say_line);
	config.cut = CARRY;                 /* mode=carry first, then before and after XeSS */
	real_execute = stub_execute;
	real_init = stub_init;
	if (argc > 1) {
		snprintf(config.pipe, sizeof config.pipe, "%s", argv[1]);
		exchange = exchange_pipe;
	} else {
		exchange = brighten;
	}
	if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&gpu.device))) {
		printf("FAIL: no D3D12 device\n");
		return 1;
	}
	D3D12_COMMAND_QUEUE_DESC queue = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
	ID3D12Device_CreateCommandQueue(gpu.device, &queue, &IID_ID3D12CommandQueue, (void **)&gpu.queue);
	ID3D12Device_CreateCommandAllocator(gpu.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
					    &IID_ID3D12CommandAllocator, (void **)&gpu.allocator);
	ID3D12Device_CreateCommandList(gpu.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator, NULL,
				       &IID_ID3D12GraphicsCommandList, (void **)&gpu.list);
	ID3D12Device_CreateFence(gpu.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&gpu.fence);
	gpu.event = CreateEventW(NULL, FALSE, FALSE, NULL);

	int failed = run(DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 1, 0, 0, NULL, NULL,
			 NULL, NULL, NULL, NULL, NULL);
	failed += run(DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", 1, 0, 0, NULL, NULL, NULL, NULL, NULL,
		      NULL, NULL);
	failed += run(DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT, strength 0", 0, 0, 0, NULL,
		      NULL, NULL, NULL, NULL, NULL, NULL);
	if (argc > 1) {
		printf(failed ? "FAILED\n" : "all passed\n");
		return failed != 0;
	}

	xess_d3d12_init_params_t init = { .outputResolution = { MOTION_W, MOTION_H },
					  .initFlags = XESS_INIT_FLAG_HIGH_RES_MV };
	xessD3D12Init((xess_context_handle_t)1, &init);
	exchange = spot;
	/* fill_motion reads six: the block's corners are left zero, for vectors everywhere */
	float rest[6] = { 0, 0 }, uniform[6] = { 6, -4 };             /* texels: (3, -2) pixels */
	float block[6] = { 20, 0, BLOCK_X0, BLOCK_Y0, BLOCK_X1, BLOCK_Y1 }; /* 10 pixels, in it only */
	ID3D12Resource *none = texture(DXGI_FORMAT_R16G16_FLOAT, MOTION_W, MOTION_H, 4, fill_motion, rest);
	ID3D12Resource *moving = texture(DXGI_FORMAT_R16G16_FLOAT, MOTION_W, MOTION_H, 4, fill_motion, uniform);
	ID3D12Resource *partly = texture(DXGI_FORMAT_R16G16_FLOAT, MOTION_W, MOTION_H, 4, fill_motion, block);
	float half_left[2] = { 0.5f, 0 };
	failed += run(DXGI_FORMAT_R16G16B16A16_FLOAT, "motion (3, -2), the picture with it", 1, 3, -2,
		      moving, moving, uniform, uniform, NULL, NULL, moved_uniformly);
	/* The second execute captured too, with the first frame's tracker: the daemon is told
	 * each pixel's offset back to the first frame NR was given, (3, -2) where it is known. */
	for (ULONGLONG until = GetTickCount64() + 5000; GetTickCount64() < until && !motion_known; Sleep(5)) {}
	for (ULONGLONG until = GetTickCount64() + 5000; GetTickCount64() < until && nr.capture; Sleep(5)) {}
	double mean_x = motion_known ? motion_sum[0] / motion_known : 0;
	double mean_y = motion_known ? motion_sum[1] / motion_known : 0;
	int told = motion_known > IN_W * IN_H / 2 && fabs(mean_x - 3) < 1e-6 && fabs(mean_y + 2) < 1e-6;
	printf("%s the daemon is told the motion: (%.3f, %.3f) over %zu pixels\n", told ? "ok  " : "FAIL",
	       mean_x, mean_y, motion_known);
	failed += !told;
	failed += run(DXGI_FORMAT_R16G16B16A16_FLOAT, "jitter (0.5, 0) then (0, 0)", 1, 0, 0, none, none,
		      rest, rest, half_left, rest, jittered);
	failed += run(DXGI_FORMAT_R16G16B16A16_FLOAT, "vectors claiming a move the picture did not make",
		      1, 0, 0, none, partly, rest, block, NULL, NULL, claimed);
	failed += run_fade(none);
	failed += run_uncovered();
	failed += run_rise(none);
	failed += run_record(moving);

	/* mode=cut, with the stand-in for the daemon on a queue of its own */
	config.cut = BEFORE;
	exchange_gpu = stand_in_gpu;
	D3D12_COMMAND_QUEUE_DESC other = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
	ID3D12Device_CreateCommandQueue(gpu.device, &other, &IID_ID3D12CommandQueue, (void **)&stand.queue);
	ID3D12Device_CreateCommandAllocator(gpu.device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
					    (void **)&stand.allocator);
	ID3D12Device_CreateCommandList(gpu.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, stand.allocator, NULL,
				       &IID_ID3D12GraphicsCommandList, (void **)&stand.list);
	ID3D12GraphicsCommandList_Close(stand.list);
	stand.upload = make_buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof stand.answer + 16, D3D12_RESOURCE_FLAG_NONE,
				   D3D12_RESOURCE_STATE_GENERIC_READ);
	failed += run_split();
	failed += run_cut(DXGI_FORMAT_R16G16B16A16_FLOAT, "cut, R16G16B16A16_FLOAT", 1, 0);
	failed += run_cut(DXGI_FORMAT_R11G11B10_FLOAT, "cut, R11G11B10_FLOAT", 1, 0);
	failed += run_cut(DXGI_FORMAT_R16G16B16A16_FLOAT, "cut, strength 0", 0, 0);
	failed += run_cut(DXGI_FORMAT_R16G16B16A16_FLOAT, "cut, declined by the daemon", 1, 1);
	failed += run_cut_motion(moving);
	config.cut = AFTER;
	failed += run_after(DXGI_FORMAT_R16G16B16A16_FLOAT, "after XeSS, R16G16B16A16_FLOAT");
	failed += run_after(DXGI_FORMAT_R11G11B10_FLOAT, "after XeSS, R11G11B10_FLOAT");
	failed += run_reopen(moving);
	printf(failed ? "FAILED\n" : "all passed\n");
	return failed != 0;
}
