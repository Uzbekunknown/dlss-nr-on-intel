/* test_ffx_original.c — a stand-in for a custom upscaler of OptiScaler v10's, for test_nr_xess: the
 * FidelityFX API's five exports and OptiPluginInit, each noting what it was handed, and a file of
 * its own looked for beside itself, where a custom upscaler keeps the files it loads. test_nr_xess
 * puts it behind NR's FidelityFX DLL in a custom upscaler's folder, as
 * amd_fidelityfx_upscaler_dx12_nr_orig.dll (nr_ffx.c), and reads what it saw through
 * test_ffx_original_seen. */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "test_ffx_original.h"

typedef struct ffxApiHeader {
	uint64_t type;
	struct ffxApiHeader *pNext;
} ffxApiHeader;

/* ffxQueryDescGetVersions, as AMD's ffx_api.h (MIT) lays it out */
#define GET_VERSIONS 4u
struct get_versions {
	ffxApiHeader header;
	uint64_t create_type;
	void *device;
	uint64_t *count, *ids;
	const char **names;
};

static struct test_ffx_seen seen;

/* Where a custom upscaler finds its files: beside its DLL, by the DLL's own path; and, for one that
 * finds itself by the standard name, whether the DLL of that name is in the same folder. */
static void look_beside(void)
{
	HMODULE self = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)&seen, &self);
	DWORD length = self ? GetModuleFileNameW(self, seen.self, 260) : 0;
	wchar_t *slash = length && length < 260 ? wcsrchr(seen.self, L'\\') : NULL;
	if (!slash) return;
	int folder = (int)(slash - seen.self);
	wchar_t path[300], other[260];
	_snwprintf(path, 300, L"%.*ls\\test_ffx_own.txt", folder, seen.self);
	seen.own_file = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
	HMODULE standard = GetModuleHandleW(L"amd_fidelityfx_upscaler_dx12.dll");
	DWORD other_length = standard ? GetModuleFileNameW(standard, other, 260) : 0;
	wchar_t *other_slash = other_length && other_length < 260 ? wcsrchr(other, L'\\') : NULL;
	seen.beside_standard_name = other_slash && other_slash - other == folder
		&& !_wcsnicmp(other, seen.self, (size_t)folder);
}

__declspec(dllexport) uint32_t ffxCreateContext(void **context, ffxApiHeader *desc, const void *memory)
{
	(void)memory;
	seen.create++;
	seen.create_desc = desc;
	look_beside();
	if (context) *context = TEST_FFX_CONTEXT;
	return 0;
}

__declspec(dllexport) uint32_t ffxDestroyContext(void **context, const void *memory)
{
	(void)memory;
	seen.destroy++;
	seen.destroy_context = context ? *context : NULL;
	if (context) *context = NULL;
	return 0;
}

__declspec(dllexport) uint32_t ffxConfigure(void **context, const ffxApiHeader *desc)
{
	(void)context;
	seen.configure++;
	seen.configure_desc = desc;
	return 0;
}

/* Asked for its versions, as OptiScaler asks a custom upscaler before it makes a context, it lists
 * one: the count when the arrays are not given, then the id and name into them. */
__declspec(dllexport) uint32_t ffxQuery(void **context, ffxApiHeader *desc)
{
	(void)context;
	seen.query++;
	seen.query_desc = desc;
	if (!desc || desc->type != GET_VERSIONS) return 0;
	struct get_versions *q = (struct get_versions *)desc;
	if (q->count && *q->count >= 1) {
		if (q->ids) q->ids[0] = TEST_FFX_VERSION_ID;
		if (q->names) q->names[0] = TEST_FFX_VERSION_NAME;
	}
	if (q->count) *q->count = 1;
	return 0;
}

__declspec(dllexport) uint32_t ffxDispatch(void **context, const ffxApiHeader *desc)
{
	seen.dispatch++;
	seen.dispatch_context = context ? *context : NULL;
	seen.dispatch_desc = desc;
	return 0;
}

__declspec(dllexport) bool OptiPluginInit(const void *api, void *plugin)
{
	seen.plugin++;
	seen.api = api;
	seen.plugin_handle = plugin;
	return true;
}

__declspec(dllexport) const struct test_ffx_seen *test_ffx_original_seen(void)
{
	return &seen;
}
