/* nr_present_probe.exe - where DXGI keeps a swap chain's Present, for the panel in a DirectX 12
 * game (nr_xess_panel.cpp).
 *
 * The panel is drawn in IDXGISwapChain::Present, which NR's libxess.dll hooks by writing into the
 * swap chain class's vtable in dxgi.dll - the way nr_xess_split.cpp hooks D3D12's command list and
 * queue classes. The vtable's address comes from a swap chain, and one made inside the game is seen
 * by everything else hooked there: OptiScaler takes a device made beside the game's for the game's
 * own and keeps the swap chain after it is gone. DLSSNR-AMD found that and moved its probe into a
 * process of its own (its nr_present_probe.cpp, MIT); this is the same idea, written for this tree.
 * A system DLL sits at the same base in every process of a session (checked on this laptop,
 * 2026-10-31), so the game finds the same vtable at the same offset in its own dxgi.dll.
 *
 * It also answers the question DXGI does not: which queue a swap chain presents on. GetDevice hands
 * out the ID3D12Device and refuses ID3D12CommandQueue (E_NOINTERFACE, Windows 11 26300), and the
 * panel's work has to go on that queue, after the game's frame and before the Present. The swap
 * chain keeps the queue in a field of its own; this finds the field's offset with two swap chains
 * on two queues, and writes it only when both agree. The proxy still checks what it reads there
 * against the queues the game executes on before it trusts it.
 *
 *   nr_present_probe.exe <output file>
 *
 * Writes key=value lines: the module, its PE timestamp and image size (so a stale answer is not
 * used after an update replaces dxgi.dll), the vtable's offset in it, the three methods' offsets
 * (Present 8, Present1 22, SetColorSpace1 38), and `queue_at`, the queue field's offset in the swap
 * chain. Exit 0 when the vtable was found.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const struct { const char *name; int slot; } methods[] = {
	{ "present", 8 }, { "present1", 22 }, { "colour_space", 38 },
};

/* The offset, in the swap chain object, of a pointer-sized field holding `queue`; -1 when none. The
 * object is read a word at a time and only while its pages are committed. */
static long queue_field(const void *swapchain, const void *queue, long after)
{
	const uintptr_t *words = (const uintptr_t *)swapchain;
	for (long i = after / 8 + (after >= 0 ? 1 : 0); i < 256; ++i) {
		MEMORY_BASIC_INFORMATION mbi;
		if (!VirtualQuery(&words[i], &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return -1;
		if (words[i] == (uintptr_t)queue) return i * 8;
	}
	return -1;
}

static HWND make_window(void)
{
	static int registered;
	WNDCLASSEXW wc;
	memset(&wc, 0, sizeof wc);
	wc.cbSize = sizeof wc;
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"nr_present_probe";
	if (!registered && RegisterClassExW(&wc)) registered = 1;
	return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL,
			       wc.hInstance, NULL);
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 2) return 2;
	FILE *out = _wfopen(argv[1], L"w");
	if (!out) return 2;
	ID3D12Device *device = NULL;
	ID3D12CommandQueue *queues[2] = { NULL, NULL };
	IDXGIFactory2 *factory = NULL;
	IDXGISwapChain1 *chains[2] = { NULL, NULL };
	HWND windows[2] = { make_window(), make_window() };
	int found = 0;
	D3D12_COMMAND_QUEUE_DESC qd;
	memset(&qd, 0, sizeof qd);
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	DXGI_SWAP_CHAIN_DESC1 sd;
	memset(&sd, 0, sizeof sd);
	sd.Width = 64;
	sd.Height = 64;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 2;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (!windows[0] || !windows[1]
	    || FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device))
	    || FAILED(CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&factory)))
		goto done;
	for (int i = 0; i < 2; ++i)
		if (FAILED(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queues[i]))
		    || FAILED(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)queues[i], windows[i], &sd,
								   NULL, NULL, &chains[i])))
			goto done;

	void **vtable = *(void ***)chains[0];
	HMODULE module = NULL;
	wchar_t path[MAX_PATH];
	char utf8[MAX_PATH * 3];
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)(void *)vtable, &module)
	    || !GetModuleFileNameW(module, path, MAX_PATH)
	    || !WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof utf8, NULL, NULL))
		goto done;
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
	const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)module + dos->e_lfanew);
	const uintptr_t base = (uintptr_t)module;
	fprintf(out, "module=%s\n", utf8);
	fprintf(out, "stamp=%lx\nsize=%lx\n", (unsigned long)nt->FileHeader.TimeDateStamp,
		(unsigned long)nt->OptionalHeader.SizeOfImage);
	fprintf(out, "vtable=%llx\n", (unsigned long long)((uintptr_t)vtable - base));
	for (size_t m = 0; m < sizeof methods / sizeof methods[0]; ++m) {
		uintptr_t at = (uintptr_t)vtable[methods[m].slot];
		if (at > base && at - base < nt->OptionalHeader.SizeOfImage)
			fprintf(out, "%s=%llx\n", methods[m].name, (unsigned long long)(at - base));
	}
	if (*(void ***)chains[1] != vtable)
		fprintf(out, "# a second swap chain has another vtable; only the first is hooked\n");
	/* The queue's field: the first offset at which the first swap chain holds its queue and the
	 * second holds its own. */
	for (long at = queue_field(chains[0], queues[0], -1); at >= 0; at = queue_field(chains[0], queues[0], at)) {
		const uintptr_t *other = (const uintptr_t *)chains[1];
		if (other[at / 8] == (uintptr_t)queues[1]) {
			fprintf(out, "queue_at=%lx\n", (unsigned long)at);
			break;
		}
	}
	found = 1;
done:
	for (int i = 0; i < 2; ++i) {
		if (chains[i]) IDXGISwapChain1_Release(chains[i]);
		if (queues[i]) ID3D12CommandQueue_Release(queues[i]);
		if (windows[i]) DestroyWindow(windows[i]);
	}
	if (factory) IDXGIFactory2_Release(factory);
	if (device) ID3D12Device_Release(device);
	fclose(out);
	return found ? 0 : 1;
}
