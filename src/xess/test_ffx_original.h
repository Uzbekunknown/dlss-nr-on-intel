/* test_ffx_original.h — what test_ffx_original.c, a stand-in for a custom upscaler of OptiScaler
 * v10's put behind NR's FidelityFX DLL, was handed: test_nr_xess reads it through the stand-in's
 * test_ffx_original_seen. */
#ifndef TEST_FFX_ORIGINAL_H
#define TEST_FFX_ORIGINAL_H

#include <stdint.h>
#include <wchar.h>

/* The one version the stand-in lists when asked (FFX_API_QUERY_DESC_TYPE_GET_VERSIONS), as a custom
 * upscaler lists its own, and the context its create makes. */
#define TEST_FFX_VERSION_ID 0x5eed0001ull
#define TEST_FFX_VERSION_NAME "4.1.2X stand-in"
#define TEST_FFX_CONTEXT ((void *)(uintptr_t)0xc0de)

struct test_ffx_seen {
	int create, destroy, configure, query, dispatch, plugin;   /* the calls */
	const void *create_desc, *configure_desc, *query_desc, *dispatch_desc;
	void *dispatch_context, *destroy_context;   /* the context each was handed */
	const void *api, *plugin_handle;            /* what OptiPluginInit was handed */
	int own_file;               /* test_ffx_own.txt found beside this DLL, by the DLL's own path */
	int beside_standard_name;   /* the DLL of the standard name is in the same folder */
	wchar_t self[260];          /* this DLL's path */
};

typedef const struct test_ffx_seen *(*test_ffx_seen_fn)(void);

#endif
