/* nr_ffx.c — an amd_fidelityfx_dx12.dll in front of AMD's, so that NR works on FSR 3.1's output
 * before the game's interface is drawn, as nr_xess.c does on XeSS's: the same cut, passes, daemon
 * session and panel, built from nr_xess.c into a DLL of FidelityFX's exports (build_xess.bat).
 *
 * Which games. FSR 3.1 is reached through AMD's FidelityFX API: five exports of one DLL the game
 * loads by name, amd_fidelityfx_dx12.dll from the FidelityFX SDK 1.1 on (Hogwarts Legacy keeps
 * 1.0.1.41314 beside its executable), amd_fidelityfx_loader_dx12.dll from SDK 2.0, which finds the
 * upscaler in a DLL of its own. FSR 2 and 3.0 are linked into the game, with nothing to stand in
 * front of. The original keeps its name with _nr_orig before the extension, beside this one.
 *
 * What. All five calls go on to the original, frame generation and its swap chain included. After
 * an upscale dispatch (FFX_API_DISPATCH_DESC_TYPE_UPSCALE) of a context this file saw created, once
 * FSR's work is in the game's command list, the list is cut and NR's answer goes into FSR's
 * output: nr_xess.c's mode=after, with what FSR is told in XeSS's terms. FSR's context gives what
 * XeSS hears at init (the output's size, whether the motion vectors are at its resolution), its
 * dispatch the rest (the vectors' scale, the render size, a reset). NR always runs after FSR here:
 * mode=before and mode=carry are for XeSS.
 *
 * States. FSR is handed each resource with the state it is in (FfxApiResource.state), and leaves
 * it in that state; the cut takes the output and the motion vectors from it and gives them back in
 * it, where XeSS's are always UNORDERED_ACCESS and NON_PIXEL_SHADER_RESOURCE.
 *
 * The settings and the log are nr_xess.c's, nr-xess.txt and nr-xess.log beside this DLL, so the
 * setup window writes and reads the same files whichever upscaler NR stands in front of.
 *
 * The FidelityFX API's types, as far as this file reads them, follow AMD's headers (MIT,
 * licenses/fidelityfx-sdk-MIT.txt): ffx_api.h, ffx_api_types.h, ffx_upscale.h and
 * dx12/ffx_api_dx12.h of the FidelityFX SDK 1.1.0, which are 2.4.0's for these structures.
 */
#ifndef NR_XESS_TEST
#define NR_PROXY_FFX
#endif
#include "nr_xess.c"

#include <stdbool.h>
#include <stddef.h>

/* ---- The FidelityFX API's types ---- */

typedef void *ffxContext;
typedef uint32_t ffxReturnCode_t;
#define FFX_API_RETURN_OK 0u
#define FFX_API_RETURN_NO_PROVIDER 4u

typedef struct ffxApiHeader {
	uint64_t type;
	struct ffxApiHeader *pNext;
} ffxApiHeader;

typedef struct {
	void *pUserData;
	void *(*alloc)(void *user, uint64_t size);
	void (*dealloc)(void *user, void *memory);
} ffxAllocationCallbacks;

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE 0x00010000u
#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE 0x00010001u

/* FfxApiCreateContextUpscaleFlags */
#define FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE (1u << 0)
#define FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS (1u << 1)
#define FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION (1u << 2)
#define FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE (1u << 8)

struct ffx_dimensions { uint32_t width, height; };
struct ffx_coords { float x, y; };

/* ffxCreateContextDescUpscale */
struct ffx_create_upscale {
	ffxApiHeader header;
	uint32_t flags;
	struct ffx_dimensions max_render, max_upscale;
	void (*message)(uint32_t type, const wchar_t *text);
};

/* FfxApiResource, with its FfxApiResourceDescription */
struct ffx_resource {
	void *resource;
	struct { uint32_t type, format, width, height, depth, mips, flags, usage; } description;
	uint32_t state;
};

/* ffxDispatchDescUpscale */
struct ffx_dispatch_upscale {
	ffxApiHeader header;
	void *command_list;
	struct ffx_resource colour, depth, motion, exposure, reactive, transparency, output;
	struct ffx_coords jitter, motion_scale;
	struct ffx_dimensions render_size, upscale_size;
	bool sharpening;
	float sharpness, frame_time, pre_exposure;
	bool reset;
	float camera_near, camera_far, camera_fov, meters;
	uint32_t flags;
};

_Static_assert(sizeof(struct ffx_resource) == 48, "FfxApiResource is 48 bytes");
_Static_assert(sizeof(struct ffx_create_upscale) == 48, "ffxCreateContextDescUpscale is 48 bytes");
_Static_assert(offsetof(struct ffx_dispatch_upscale, output) == 312
	       && offsetof(struct ffx_dispatch_upscale, upscale_size) == 384
	       && offsetof(struct ffx_dispatch_upscale, reset) == 408
	       && sizeof(struct ffx_dispatch_upscale) == 432,
	       "ffxDispatchDescUpscale as the FidelityFX SDK 1.1-2.4 lays it out");

typedef ffxReturnCode_t (*ffx_create_fn)(ffxContext *, ffxApiHeader *, const ffxAllocationCallbacks *);
typedef ffxReturnCode_t (*ffx_destroy_fn)(ffxContext *, const ffxAllocationCallbacks *);
typedef ffxReturnCode_t (*ffx_configure_fn)(ffxContext *, const ffxApiHeader *);
typedef ffxReturnCode_t (*ffx_query_fn)(ffxContext *, ffxApiHeader *);
typedef ffxReturnCode_t (*ffx_dispatch_fn)(ffxContext *, const ffxApiHeader *);

static struct {
	ffx_create_fn create;
	ffx_destroy_fn destroy;
	ffx_configure_fn configure;
	ffx_query_fn query;
	ffx_dispatch_fn dispatch;
} ffx;

/* FfxApiResourceState into the D3D12 state it names, as AMD's DX12 backend reads it; -1 for one
 * this file does not know, whose resource it then leaves alone. */
static D3D12_RESOURCE_STATES ffx_state(uint32_t state)
{
	switch (state) {
	case 1u << 0: return D3D12_RESOURCE_STATE_COMMON;
	case 1u << 1: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	case 1u << 2: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	case 1u << 3: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	case (1u << 3) | (1u << 2):
		return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	case 1u << 4: return D3D12_RESOURCE_STATE_COPY_SOURCE;
	case 1u << 5: return D3D12_RESOURCE_STATE_COPY_DEST;
	case (1u << 4) | (1u << 2): return D3D12_RESOURCE_STATE_GENERIC_READ;
	case 1u << 6: return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
	case 1u << 7: return D3D12_RESOURCE_STATE_PRESENT;
	case 1u << 8: return D3D12_RESOURCE_STATE_RENDER_TARGET;
	default: return (D3D12_RESOURCE_STATES)-1;
	}
}

/* ---- The upscale contexts the game made, and what each was made with ---- */

#define FFX_CONTEXTS 8
static struct ffx_upscale_context {
	ffxContext context;      /* NULL: a free entry */
	uint32_t flags;
	struct ffx_dimensions max_render, max_upscale;
	int dispatches;          /* the first is said in the log */
} contexts[FFX_CONTEXTS];
static SRWLOCK contexts_lock = SRWLOCK_INIT;
static unsigned contexts_next;   /* where a new one goes once all are taken: the oldest */

static void keep_context(ffxContext context, const struct ffx_create_upscale *desc)
{
	AcquireSRWLockExclusive(&contexts_lock);
	int at = -1;
	for (int i = 0; i < FFX_CONTEXTS && at < 0; i++)
		if (!contexts[i].context || contexts[i].context == context) at = i;
	if (at < 0) at = (int)(contexts_next++ % FFX_CONTEXTS);
	contexts[at] = (struct ffx_upscale_context){ context, desc->flags, desc->max_render, desc->max_upscale, 0 };
	ReleaseSRWLockExclusive(&contexts_lock);
}

static void drop_context(ffxContext context)
{
	AcquireSRWLockExclusive(&contexts_lock);
	for (int i = 0; i < FFX_CONTEXTS; i++)
		if (context && contexts[i].context == context) contexts[i].context = NULL;
	ReleaseSRWLockExclusive(&contexts_lock);
}

/* A copy of the context's entry, its dispatches counted; 0 when it is not one this file saw made. */
static int find_context(ffxContext context, struct ffx_upscale_context *out)
{
	int found = 0;
	AcquireSRWLockExclusive(&contexts_lock);
	for (int i = 0; i < FFX_CONTEXTS && !found; i++)
		if (context && contexts[i].context == context) {
			contexts[i].dispatches++;
			*out = contexts[i];
			found = 1;
		}
	ReleaseSRWLockExclusive(&contexts_lock);
	return found;
}

/* ---- The five exports ---- */

ffxReturnCode_t ffxCreateContext(ffxContext *context, ffxApiHeader *desc, const ffxAllocationCallbacks *memory)
{
	if (!ffx.create) return FFX_API_RETURN_NO_PROVIDER;
	ffxReturnCode_t result = ffx.create(context, desc, memory);
	if (passive) return result;
	const struct ffx_create_upscale *upscale = NULL;
	for (const ffxApiHeader *h = desc; h; h = h->pNext)
		if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) upscale = (const void *)h;
	if (!upscale) {
		say("FidelityFX context of type 0x%llx -> %u\n", desc ? (unsigned long long)desc->type : 0ull, result);
		return result;
	}
	uint32_t f = upscale->flags;
	say("FSR 3.1 upscale context %p: render up to %ux%u, output %ux%u, flags 0x%x (%s, vectors at the %s's "
	    "resolution%s%s) -> %u\n", context && result == FFX_API_RETURN_OK ? *context : NULL,
	    upscale->max_render.width, upscale->max_render.height, upscale->max_upscale.width,
	    upscale->max_upscale.height, f, f & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE ? "HDR" : "LDR",
	    f & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS ? "output" : "render",
	    f & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION ? ", jitter in them" : "",
	    f & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE ? ", non-linear colour" : "", result);
	if (result == FFX_API_RETURN_OK && context && *context) keep_context(*context, upscale);
	return result;
}

ffxReturnCode_t ffxDestroyContext(ffxContext *context, const ffxAllocationCallbacks *memory)
{
	if (!ffx.destroy) return FFX_API_RETURN_NO_PROVIDER;
	if (context) drop_context(*context);
	return ffx.destroy(context, memory);
}

ffxReturnCode_t ffxConfigure(ffxContext *context, const ffxApiHeader *desc)
{
	if (!ffx.configure) return FFX_API_RETURN_NO_PROVIDER;
	return ffx.configure(context, desc);
}

ffxReturnCode_t ffxQuery(ffxContext *context, ffxApiHeader *desc)
{
	if (!ffx.query) return FFX_API_RETURN_NO_PROVIDER;
	return ffx.query(context, desc);
}

/* After FSR's work on the list: the cut, NR's answer into FSR's output. */
static void cut_after_fsr(ffxContext context, const struct ffx_dispatch_upscale *d, ID3D12GraphicsCommandList *list)
{
	struct ffx_upscale_context made;
	if (!find_context(context, &made)) {
		cut_said("FSR 3.1 dispatched a context this proxy did not see made");
		return;
	}
	D3D12_RESOURCE_STATES output_at = ffx_state(d->output.state), vectors_at = ffx_state(d->motion.state);
	if (made.dispatches == 1)
		say("FSR 3.1 dispatch on context %p: render %ux%u, output %ux%u, output state 0x%x, vectors state "
		    "0x%x and scale (%g, %g), pre-exposure %g, sharpening %d (%g)\n", context, d->render_size.width,
		    d->render_size.height, d->upscale_size.width, d->upscale_size.height, d->output.state,
		    d->motion.state, d->motion_scale.x, d->motion_scale.y, d->pre_exposure, d->sharpening,
		    d->sharpness);
	if (made.flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) {
		cut_said("FSR 3.1's colour is non-linear (FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE), and NR takes "
			 "linear colour");
		return;
	}
	if (output_at == (D3D12_RESOURCE_STATES)-1) {
		char why[96];
		snprintf(why, sizeof why, "FSR 3.1's output is in state 0x%x, which NR does not know", d->output.state);
		cut_said(why);
		return;
	}
	/* What XeSS is told at init and by its two scales: the output's size, the vectors' resolution,
	 * and what a vector's value is in pixels at that resolution, which is FSR's scale too. */
	struct ffx_dimensions size = d->upscale_size.width && d->upscale_size.height ? d->upscale_size : made.max_upscale;
	xess.output.x = size.width;
	xess.output.y = size.height;
	xess.flags = made.flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS ? XESS_INIT_FLAG_HIGH_RES_MV : 0;
	xess.velocity[0] = d->motion_scale.x;
	xess.velocity[1] = d->motion_scale.y;
	xess.jitter[0] = xess.jitter[1] = 1;
	xess_d3d12_execute_params_t p = {
		.pColorTexture = d->colour.resource,
		/* vectors in a state this file does not know are not followed */
		.pVelocityTexture = vectors_at == (D3D12_RESOURCE_STATES)-1 ? NULL : d->motion.resource,
		.pOutputTexture = d->output.resource,
		.jitterOffsetX = d->jitter.x, .jitterOffsetY = d->jitter.y,
		.exposureScale = 1,
		.resetHistory = d->reset,
		.inputWidth = d->render_size.width, .inputHeight = d->render_size.height,
	};
	record_cut(list, &p, 1, output_at, vectors_at);
}

ffxReturnCode_t ffxDispatch(ffxContext *context, const ffxApiHeader *desc)
{
	if (!ffx.dispatch) return FFX_API_RETURN_NO_PROVIDER;
	if (passive || !desc || desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) return ffx.dispatch(context, desc);
	const struct ffx_dispatch_upscale *d = (const void *)desc;
	ID3D12GraphicsCommandList *list = d->command_list;
#ifndef NR_XESS_TEST
	if (list) nr_xess_panel_list(list);
#endif
	handed = d->colour.resource;
	cut_now = 0;
	look_at_trigger();
	if (nr.broken || !config.enabled || !trigger_on || !list || !context || !d->colour.resource
	    || !d->output.resource)
		return ffx.dispatch(context, desc);
	/* FSR's work goes into the list first, and the cut after it */
	ffxReturnCode_t result = ffx.dispatch(context, desc);
	if (result == FFX_API_RETURN_OK) cut_after_fsr(*context, d, list);
	return result;
}

#ifndef NR_XESS_TEST
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_DETACH && !reserved) let_go_of_claim();
	if (reason != DLL_PROCESS_ATTACH) return TRUE;
	DisableThreadLibraryCalls(instance);
	if (!find_here(instance)) return FALSE;
	upscaler = "FSR 3.1";
	/* The original beside this one, by its full path: this file's name with _nr_orig before .dll,
	 * amd_fidelityfx_dx12_nr_orig.dll for SDK 1.1, amd_fidelityfx_loader_dx12_nr_orig.dll for 2.0. */
	wchar_t self[MAX_PATH], original[MAX_PATH + 16];
	DWORD length = GetModuleFileNameW(instance, self, MAX_PATH);
	wchar_t *dot = length && length < MAX_PATH ? wcsrchr(self, L'.') : NULL;
	if (!dot || dot < wcsrchr(self, L'\\')) return FALSE;
	*dot = L'\0';
	_snwprintf(original, MAX_PATH + 16, L"%ls_nr_orig.dll", self);
	const wchar_t *name = wcsrchr(original, L'\\') + 1;
	HMODULE module = LoadLibraryExW(original, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!module) {
		say("cannot load %ls beside this one (error %lu); FSR 3.1 is unavailable\n", name, GetLastError());
		return FALSE;
	}
	ffx.create = (ffx_create_fn)(void *)GetProcAddress(module, "ffxCreateContext");
	ffx.destroy = (ffx_destroy_fn)(void *)GetProcAddress(module, "ffxDestroyContext");
	ffx.configure = (ffx_configure_fn)(void *)GetProcAddress(module, "ffxConfigure");
	ffx.query = (ffx_query_fn)(void *)GetProcAddress(module, "ffxQuery");
	ffx.dispatch = (ffx_dispatch_fn)(void *)GetProcAddress(module, "ffxDispatch");
	if (another_proxy()) {
		passive = 1;
		say("NR in front of FSR 3.1 (%ls), process %lu: another of NR's proxies is in this process "
		    "already, so this one hands every call to FSR as it is\n", name, GetCurrentProcessId());
		return TRUE;
	}
	build_tables();
	nr_split_set_log(say_line);
	read_settings(1);
	if (config.cut != AFTER) say("mode=before and mode=carry are for XeSS; FSR 3.1 gets NR after it\n");
	config.cut = AFTER;
	say("NR in front of FSR 3.1 (%ls), process %lu\n", name, GetCurrentProcessId());
	start_panel();
	return TRUE;
}
#endif
