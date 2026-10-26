/* replay_nr_xess.c — a recording of what a game handed XeSS (nr_xess.c, `record=N`), run
 * again through the proxy, the daemon and Intel's own XeSS, without the game.
 *
 *   replay_nr_xess <recording> <out> <Intel's libxess.dll> [options]
 *     --pipe P          the daemon (default \\.\pipe\nr-xess-replay); for mode=cut it runs with
 *                       NR_GPU_IMAGE=1 and NR_GPU_TRANSPORT=1
 *     --mode cut|carry  the proxy's mode (default cut: NR answers each frame between the halves
 *                       of a cut list; carry: its change to an earlier frame carried along motion)
 *     --interval MS     at least this long a frame, as the game ran (default 28)
 *     --strength S      NR's strength (default 1; 0 runs the whole path and adds nothing)
 *     --off             XeSS alone: the proxy hands every frame straight on
 *     --warp 0|1, --match FROM TO, --history 0|1   mode=carry's own settings
 *     --sync            mode=carry: each frame waits for the answer to the one before (`sync=1`)
 *     --answers DIR     each frame NR was given and its answer, DIR\given_%04u.ppm and
 *                       answer_%04u.ppm by frame: in mode=cut, or in mode=carry with --sync,
 *                       NR on every frame as it is
 *     --every N         write every Nth frame (default 1)
 *     --inputs DIR      also the colour XeSS was handed, the proxy's change in it, to
 *                       DIR\%04u.ppm: whether something in the output came in with the
 *                       change or grew in XeSS's own history
 *
 * Writes <out>\%04u.ppm: XeSS's output for each frame through the proxy's display encode,
 * so a change to the proxy can be seen, and compared with XeSS alone, without anyone
 * playing. The daemon runs at its own pace beside the frames, as it does in the game.
 */
#define NR_XESS_TEST
#include "nr_xess.c"

typedef xess_result_t (*create_fn)(ID3D12Device *, xess_context_handle_t *);

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
		printf("the GPU did not finish (device removed: 0x%lx)\n",
		       (unsigned long)ID3D12Device_GetDeviceRemovedReason(gpu.device));
		return -1;
	}
	ID3D12CommandAllocator_Reset(gpu.allocator);
	ID3D12GraphicsCommandList_Reset(gpu.list, gpu.allocator, NULL);
	return 0;
}

static ID3D12Resource *make_texture(DXGI_FORMAT format, UINT width, UINT height,
				    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
	D3D12_RESOURCE_DESC desc = {
		.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = width, .Height = height,
		.DepthOrArraySize = 1, .MipLevels = 1, .Format = format, .SampleDesc = { 1, 0 },
		.Flags = flags,
	};
	D3D12_HEAP_PROPERTIES local = { .Type = D3D12_HEAP_TYPE_DEFAULT };
	ID3D12Resource *texture = NULL;
	if (FAILED(ID3D12Device_CreateCommittedResource(gpu.device, &local, D3D12_HEAP_FLAG_NONE, &desc,
			state, NULL, &IID_ID3D12Resource, (void **)&texture)))
		return NULL;
	return texture;
}

static D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_of(ID3D12Resource *texture, UINT64 *bytes)
{
	D3D12_RESOURCE_DESC desc;
	ID3D12Resource_GetDesc(texture, &desc);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT rows;
	UINT64 row_bytes;
	ID3D12Device_GetCopyableFootprints(gpu.device, &desc, 0, 1, 0, &footprint, &rows, &row_bytes, bytes);
	return footprint;
}

/* Rows packed tight in `data` into the texture, which is left in NON_PIXEL_SHADER_RESOURCE. */
static void upload(ID3D12Resource *texture, ID3D12Resource *staging, const uint8_t *data,
		   UINT row_bytes, UINT rows)
{
	UINT64 bytes;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = footprint_of(texture, &bytes);
	uint8_t *mapped;
	ID3D12Resource_Map(staging, 0, NULL, (void **)&mapped);
	for (UINT y = 0; y < rows; y++)
		memcpy(mapped + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch,
		       data + (size_t)y * row_bytes, row_bytes);
	ID3D12Resource_Unmap(staging, 0, NULL);
	D3D12_RESOURCE_BARRIER b = transition(texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
					      D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
	D3D12_TEXTURE_COPY_LOCATION to = { .pResource = texture, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	D3D12_TEXTURE_COPY_LOCATION from = { .pResource = staging, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
					     .PlacedFootprint = footprint };
	ID3D12GraphicsCommandList_CopyTextureRegion(gpu.list, &to, 0, 0, 0, &from, NULL);
	b = transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
}

static int read_file(const char *path, uint8_t *data, size_t bytes)
{
	FILE *file = fopen(path, "rb");
	if (!file) return -1;
	size_t got = fread(data, 1, bytes, file);
	fclose(file);
	return got == bytes ? 0 : -1;
}

/* XeSS's output (RGBA16F), or the colour it was handed, through the proxy's encode, as a PPM. */
static void write_frame(const char *path, const uint8_t *mapped, D3D12_PLACED_SUBRESOURCE_FOOTPRINT f,
			UINT width, UINT height, int kind, UINT texel)
{
	FILE *file = fopen(path, "wb");
	if (!file) return;
	fprintf(file, "P6\n%u %u\n255\n", width, height);
	uint8_t *row = malloc((size_t)width * 3);
	float inverse_white = 1 / config.white, rgb[3];
	uint8_t px[4];
	for (UINT y = 0; y < height; y++) {
		const uint8_t *line = mapped + f.Offset + (size_t)y * f.Footprint.RowPitch;
		for (UINT x = 0; x < width; x++) {
			load_texel(line + (size_t)x * texel, kind, rgb);
			encode_proxy(rgb, inverse_white, px);
			memcpy(row + (size_t)x * 3, px, 3);
		}
		fwrite(row, 1, (size_t)width * 3, file);
	}
	free(row);
	fclose(file);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		printf("usage: replay_nr_xess <recording> <out> <Intel's libxess.dll> [options]\n");
		return 2;
	}
	const char *recording = argv[1], *out = argv[2], *intel = argv[3], *inputs = NULL;
	int interval = 28, every = 1, off = 0;
	snprintf(config.pipe, sizeof config.pipe, "\\\\.\\pipe\\nr-xess-replay");
	for (int i = 4; i < argc; i++) {
		if (!strcmp(argv[i], "--pipe") && i + 1 < argc) snprintf(config.pipe, sizeof config.pipe, "%s", argv[++i]);
		else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--strength") && i + 1 < argc) config.strength = (float)atof(argv[++i]);
		else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--off")) off = 1;
		else if (!strcmp(argv[i], "--mode") && i + 1 < argc) config.cut = strcmp(argv[++i], "carry") != 0;
		else if (!strcmp(argv[i], "--sync")) config.sync = 1;
		else if (!strcmp(argv[i], "--answers") && i + 1 < argc) {
			answers_dir = argv[++i];
			CreateDirectoryA(answers_dir, NULL);
		}
		else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) inputs = argv[++i];
		else if (!strcmp(argv[i], "--warp") && i + 1 < argc) config.warp = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--history") && i + 1 < argc) config.motion_history = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--match") && i + 2 < argc) {
			config.match_from = (float)atof(argv[++i]);
			config.match_to = (float)atof(argv[++i]);
		}
	}
	if (every < 1) every = 1;
	CreateDirectoryA(out, NULL);
	char path[MAX_PATH + 64];
	snprintf(path, sizeof path, "%s\\replay.log", out);
	MultiByteToWideChar(CP_UTF8, 0, path, -1, log_path, MAX_PATH + 16);
	build_tables();
	nr_split_set_log(say_line);
	config.enabled = !off;

	/* what the recording says */
	UINT width = 0, height = 0, texel = 0, motion_width = 0, motion_height = 0, out_width = 0, out_height = 0;
	int format = 0, quality = 0;
	unsigned flags = 0;
	float velocity[2] = { 1, 1 }, jitter_scale[2] = { 1, 1 };
	snprintf(path, sizeof path, "%s\\meta.txt", recording);
	FILE *meta = fopen(path, "r");
	if (!meta) {
		printf("no %s\n", path);
		return 1;
	}
	char key[64];
	while (fscanf(meta, "%63s", key) == 1) {
		if (!strcmp(key, "colour")) fscanf(meta, "%u %u %d %u", &width, &height, &format, &texel);
		else if (!strcmp(key, "motion")) fscanf(meta, "%u %u", &motion_width, &motion_height);
		else if (!strcmp(key, "output")) fscanf(meta, "%u %u", &out_width, &out_height);
		else if (!strcmp(key, "quality")) fscanf(meta, "%d", &quality);
		else if (!strcmp(key, "flags")) fscanf(meta, "%u", &flags);
		else if (!strcmp(key, "velocity_scale")) fscanf(meta, "%f %f", &velocity[0], &velocity[1]);
		else if (!strcmp(key, "jitter_scale")) fscanf(meta, "%f %f", &jitter_scale[0], &jitter_scale[1]);
		else if (!strcmp(key, "white")) fscanf(meta, "%f", &config.white);
	}
	fclose(meta);
	struct { unsigned index, sequence, reset; float jitter[2], exposure; } *frames = NULL;
	size_t count = 0, room = 0;
	snprintf(path, sizeof path, "%s\\frames.txt", recording);
	FILE *list = fopen(path, "r");
	if (!list) {
		printf("no %s\n", path);
		return 1;
	}
	unsigned index, sequence, reset;
	float jx, jy, exposure;
	while (fscanf(list, "%u %u %f %f %f %u", &index, &sequence, &jx, &jy, &exposure, &reset) == 6) {
		if (count == room) frames = realloc(frames, (room = room ? room * 2 : 256) * sizeof *frames);
		frames[count].index = index;
		frames[count].sequence = sequence;
		frames[count].reset = reset;
		frames[count].jitter[0] = jx;
		frames[count].jitter[1] = jy;
		frames[count].exposure = exposure;
		count++;
	}
	fclose(list);
	printf("%zu frames of %ux%u colour (format %d), %ux%u vectors, to %ux%u, quality %d, flags 0x%x\n",
	       count, width, height, format, motion_width, motion_height, out_width, out_height, quality, flags);
	if (!count || !width || !motion_width || !out_width) return 1;

	/* the device, and Intel's XeSS on it */
	if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&gpu.device))) {
		printf("no D3D12 device\n");
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
	HMODULE xess_dll = LoadLibraryA(intel);
	if (!xess_dll) {
		printf("cannot load %s (%lu)\n", intel, GetLastError());
		return 1;
	}
	create_fn create = (create_fn)(void *)GetProcAddress(xess_dll, "xessD3D12CreateContext");
	real_init = (init_fn)(void *)GetProcAddress(xess_dll, "xessD3D12Init");
	real_execute = (execute_fn)(void *)GetProcAddress(xess_dll, "xessD3D12Execute");
	real_velocity_scale = (scale_fn)(void *)GetProcAddress(xess_dll, "xessSetVelocityScale");
	real_jitter_scale = (scale_fn)(void *)GetProcAddress(xess_dll, "xessSetJitterScale");
	xess_context_handle_t context = NULL;
	if (!create || !real_init || !real_execute || create(gpu.device, &context) != 0) {
		printf("XeSS would not make a context on this device\n");
		return 1;
	}
	xess_d3d12_init_params_t init = { .outputResolution = { out_width, out_height },
		.qualitySetting = quality, .initFlags = flags };
	xess_result_t result = xessD3D12Init(context, &init);
	if (result != 0) {
		printf("XeSS init failed (%d)\n", result);
		return 1;
	}
	if (velocity[0] != 1 || velocity[1] != 1) xessSetVelocityScale(context, velocity[0], velocity[1]);
	if (jitter_scale[0] != 1 || jitter_scale[1] != 1) xessSetJitterScale(context, jitter_scale[0], jitter_scale[1]);

	/* the frame's three textures, and what moves the data */
	ID3D12Resource *colour = make_texture((DXGI_FORMAT)format, width, height, D3D12_RESOURCE_FLAG_NONE,
					      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12Resource *motion = make_texture(DXGI_FORMAT_R16G16_FLOAT, motion_width, motion_height,
					      D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	ID3D12Resource *output = make_texture(DXGI_FORMAT_R16G16B16A16_FLOAT, out_width, out_height,
					      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
					      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	if (!colour || !motion || !output) {
		printf("could not make the textures\n");
		return 1;
	}
	nr.device = gpu.device;                 /* make_buffer's device for what follows */
	UINT64 colour_bytes, motion_bytes, output_bytes;
	footprint_of(colour, &colour_bytes);
	footprint_of(motion, &motion_bytes);
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT output_layout = footprint_of(output, &output_bytes);
	ID3D12Resource *colour_up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, colour_bytes, D3D12_RESOURCE_FLAG_NONE,
						D3D12_RESOURCE_STATE_GENERIC_READ);
	ID3D12Resource *motion_up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, motion_bytes, D3D12_RESOURCE_FLAG_NONE,
						D3D12_RESOURCE_STATE_GENERIC_READ);
	ID3D12Resource *readback = make_buffer(D3D12_HEAP_TYPE_READBACK, output_bytes, D3D12_RESOURCE_FLAG_NONE,
					       D3D12_RESOURCE_STATE_COPY_DEST);
	UINT64 input_bytes;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT input_layout = footprint_of(colour, &input_bytes);
	ID3D12Resource *input_readback = inputs ? make_buffer(D3D12_HEAP_TYPE_READBACK, input_bytes,
				D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST) : NULL;
	UINT input_texel;
	int input_kind = kind_of((DXGI_FORMAT)format, &input_texel);
	if (inputs) CreateDirectoryA(inputs, NULL);
	nr.device = NULL;                       /* the hook takes it from the command list */
	size_t colour_size = (size_t)width * height * texel, motion_size = (size_t)motion_width * motion_height * 4;
	uint8_t *data = malloc(colour_size + motion_size);
	ID3D12Resource *answers_readback = NULL;

	LARGE_INTEGER frequency, start, now;
	QueryPerformanceFrequency(&frequency);
	for (size_t i = 0; i < count; i++) {
		QueryPerformanceCounter(&start);
		snprintf(path, sizeof path, "%s\\%04u.bin", recording, frames[i].index);
		if (read_file(path, data, colour_size + motion_size)) {
			printf("cannot read %s\n", path);
			return 1;
		}
		upload(colour, colour_up, data, width * texel, height);
		upload(motion, motion_up, data + colour_size, motion_width * 4, motion_height);
		xess_d3d12_execute_params_t params = {
			.pColorTexture = colour, .pVelocityTexture = motion, .pOutputTexture = output,
			.jitterOffsetX = frames[i].jitter[0], .jitterOffsetY = frames[i].jitter[1],
			.exposureScale = frames[i].exposure, .resetHistory = frames[i].reset,
			.inputWidth = width, .inputHeight = height,
		};
		result = xessD3D12Execute(context, gpu.list, &params);
		if (result != 0) printf("frame %zu: XeSS said %d\n", i, result);
		D3D12_RESOURCE_BARRIER b = transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
						      D3D12_RESOURCE_STATE_COPY_SOURCE);
		ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
		D3D12_TEXTURE_COPY_LOCATION to = { .pResource = readback, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
						   .PlacedFootprint = output_layout };
		D3D12_TEXTURE_COPY_LOCATION from = { .pResource = output, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
		ID3D12GraphicsCommandList_CopyTextureRegion(gpu.list, &to, 0, 0, 0, &from, NULL);
		b = transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
		/* what the hook handed XeSS: its own colour when it changed the frame, else the game's */
		int input = inputs && i % every == 0;
		ID3D12Resource *given_xess = handed ? handed : colour;
		if (input) {
			b = transition(given_xess, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
			D3D12_TEXTURE_COPY_LOCATION into = { .pResource = input_readback,
				.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = input_layout };
			D3D12_TEXTURE_COPY_LOCATION taken = { .pResource = given_xess, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			ID3D12GraphicsCommandList_CopyTextureRegion(gpu.list, &into, 0, 0, 0, &taken, NULL);
			b = transition(given_xess, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			ID3D12GraphicsCommandList_ResourceBarrier(gpu.list, 1, &b);
		}
		/* mode=cut's --answers: the frame NR was given and its answer, out of the shared buffers,
		 * read after the continuation's COMPOSE */
		int cut_frame = config.cut && answers_dir && given_xess == nr.mixed && nr.mixed && i % every == 0;
		UINT64 shared = (UINT64)width * height * 4;
		if (cut_frame) {
			if (!answers_readback)
				answers_readback = make_buffer(D3D12_HEAP_TYPE_READBACK, 2 * shared + 16, D3D12_RESOURCE_FLAG_NONE,
							       D3D12_RESOURCE_STATE_COPY_DEST);
			ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, answers_readback, 0, cut.gpu.input, 0, shared);
			ID3D12GraphicsCommandList_CopyBufferRegion(gpu.list, answers_readback, shared, cut.gpu.output, 0,
								   shared + 16);
		}
		uint64_t cut_value = cut.last;
		if (submit()) return 1;
		if (cut_frame) {
			uint8_t *mapped;
			D3D12_RANGE all = { 0, (SIZE_T)(2 * shared + 16) }, none = { 0, 0 };
			ID3D12Resource_Map(answers_readback, 0, &all, (void **)&mapped);
			struct nr_gpu_stamp stamp;
			memcpy(&stamp, mapped + 2 * shared, sizeof stamp);
			if (nr_gpu_d3d12_answered(&stamp, cut_value)) {
				const char *names[2] = { "given", "answer" };
				for (int k = 0; k < 2; k++) {
					snprintf(path, sizeof path, "%s\\%s_%04zu.ppm", answers_dir, names[k], i);
					FILE *file = fopen(path, "wb");
					if (!file) continue;
					fprintf(file, "P6\n%u %u\n255\n", width, height);
					for (size_t p = 0; p < (size_t)width * height; p++)
						fwrite(mapped + k * shared + p * 4, 1, 3, file);
					fclose(file);
				}
			}
			ID3D12Resource_Unmap(answers_readback, 0, &none);
		}
		if (i % every == 0) {
			uint8_t *mapped;
			D3D12_RANGE all = { 0, (SIZE_T)output_bytes }, none = { 0, 0 };
			ID3D12Resource_Map(readback, 0, &all, (void **)&mapped);
			snprintf(path, sizeof path, "%s\\%04zu.ppm", out, i);
			write_frame(path, mapped, output_layout, out_width, out_height, KIND_RGBA16F, 8);
			ID3D12Resource_Unmap(readback, 0, &none);
		}
		if (input) {
			uint8_t *mapped;
			D3D12_RANGE all = { 0, (SIZE_T)input_bytes }, none = { 0, 0 };
			ID3D12Resource_Map(input_readback, 0, &all, (void **)&mapped);
			snprintf(path, sizeof path, "%s\\%04zu.ppm", inputs, i);
			write_frame(path, mapped, input_layout, width, height, input_kind, input_texel);
			ID3D12Resource_Unmap(input_readback, 0, &none);
		}
		/* the game's own pace, so the daemon keeps up with the frames as it did there */
		do {
			QueryPerformanceCounter(&now);
		} while ((now.QuadPart - start.QuadPart) * 1000 / frequency.QuadPart < interval && (Sleep(1), 1));
	}
	printf("replayed %zu frames into %s\n", count, out);
	return 0;
}
