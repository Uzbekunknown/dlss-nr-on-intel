/* The panel in a DirectX 12 swap chain, offline: what nr_xess_panel.cpp does in a game, in a process
 * that is its own game.
 *
 * It starts the panel as the proxy's DllMain does - the probe in a process of its own, DXGI's
 * Present hooked through the vtable it finds - then makes a D3D12 device, a queue and a swap chain
 * on a window that is never shown, clears each back buffer green on that queue and presents it, with
 * the panel open (NR_MENU_OPEN) and the 30th frame it is drawn over written out (NR_MENU_DUMP). What
 * that proves: the probe's answer fits this process's dxgi.dll, the hook is called, the queue is
 * found (read out of the swap chain, or the thread's), the panel is drawn over the frame with the
 * game's pixels kept round it, and the frame still presents.
 *
 * The window is never shown, so it is never in front: the panel reads no keys and takes no pointer,
 * and the fixture runs while somebody uses the machine. It needs a D3D12 device, so it skips with 77
 * where there is none.
 *
 *   test_xess_panel.exe          (beside nr_present_probe.exe, as build_xess.bat leaves them)
 */
#define _CRT_SECURE_NO_WARNINGS
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nr_xess_panel.h"

static int failures = 0;
static char said_all[16384];

static void check(int ok, const char *what, const char *detail)
{
	printf("  %s  %s  (%s)\n", ok ? "PASS" : "FAIL", what, detail ? detail : "");
	if (!ok) ++failures;
}

static void said(const char *line)
{
	printf("  [panel] %s\n", line);
	strncat(said_all, line, sizeof said_all - strlen(said_all) - 2);
	strcat(said_all, "\n");
}

static int trigger = 1;
static int trigger_get(void *user) { (void)user; return trigger; }
static void trigger_set(void *user, int on) { (void)user; trigger = on; }

/* One pixel of a binary PPM, as read back; 0 when the file is not one this wrote. */
static int pixel(const unsigned char *ppm, size_t size, int x, int y, unsigned char rgb[3])
{
	unsigned w = 0, h = 0, max = 0;
	int used = 0;
	if (sscanf((const char *)ppm, "P6 %u %u %u%n", &w, &h, &max, &used) != 3 || max != 255) return 0;
	size_t at = (size_t)used + 1 + ((size_t)y * w + (size_t)x) * 3;
	if (x < 0 || y < 0 || (unsigned)x >= w || (unsigned)y >= h || at + 3 > size) return 0;
	memcpy(rgb, ppm + at, 3);
	return 1;
}

static int green(const unsigned char rgb[3])
{
	return rgb[0] <= 2 && rgb[1] >= 151 && rgb[1] <= 155 && rgb[2] <= 2;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("the panel over a DirectX 12 swap chain\n");

	wchar_t dir[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
	wchar_t *slash = n ? wcsrchr(dir, L'\\') : NULL;
	if (!slash) return 1;
	slash[1] = L'\0';
	static nr_xess_panel_host host;
	memset(&host, 0, sizeof host);
	host.say = said;
	host.trigger_get = trigger_get;
	host.trigger_set = trigger_set;
	_snwprintf(host.probe, 1024, L"%lsnr_present_probe.exe", dir);
	_snwprintf(host.answer, 1024, L"%lstest_xess_panel_present.txt", dir);
	DeleteFileW(host.answer);   /* the probe runs, rather than an earlier answer being read */
	char dir_a[MAX_PATH * 2], dump[MAX_PATH * 2];
	WideCharToMultiByte(CP_ACP, 0, dir, -1, dir_a, sizeof dir_a, NULL, NULL);
	snprintf(host.settings, sizeof host.settings, "%stest_xess_panel_settings.json", dir_a);
	snprintf(dump, sizeof dump, "%stest_xess_panel.ppm", dir_a);
	remove(dump);
	FILE *settings = fopen(host.settings, "wb");
	if (settings) {
		fputs("{\"render_scale\": 0.4}\n", settings);
		fclose(settings);
	}
	_putenv_s("NR_MENU_OPEN", "1");
	_putenv_s("NR_MENU_DUMP", dump);

	nr_xess_panel_start(&host);
	for (int i = 0; i < 300 && !nr_xess_panel_hooked(); ++i) Sleep(100);
	check(nr_xess_panel_hooked(), "DXGI's Present is hooked through the probe's answer",
	      nr_xess_panel_hooked() ? "hooked" : "not within 30 s");
	if (!nr_xess_panel_hooked()) goto out;

	{
		ID3D12Device *device = NULL;
		if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device))) {
			printf("  skipped: no D3D12 device here\n");
			return 77;
		}
		WNDCLASSEXW wc;
		memset(&wc, 0, sizeof wc);
		wc.cbSize = sizeof wc;
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(NULL);
		wc.lpszClassName = L"NrXessPanelTest";
		RegisterClassExW(&wc);
		/* Never shown: never in front, so no keys and no pointer are the panel's. */
		HWND window = CreateWindowExW(0, wc.lpszClassName, L"the game", WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720,
					      NULL, NULL, wc.hInstance, NULL);
		D3D12_COMMAND_QUEUE_DESC qd;
		memset(&qd, 0, sizeof qd);
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		ID3D12CommandQueue *queue = NULL;
		IDXGIFactory2 *factory = NULL;
		IDXGISwapChain1 *chain = NULL;
		IDXGISwapChain3 *chain3 = NULL;
		ID3D12CommandAllocator *allocator = NULL;
		ID3D12GraphicsCommandList *list = NULL;
		ID3D12DescriptorHeap *rtvs = NULL;
		ID3D12Fence *fence = NULL;
		HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
		DXGI_SWAP_CHAIN_DESC1 sd;
		memset(&sd, 0, sizeof sd);
		sd.Width = 1280;
		sd.Height = 720;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 2;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		D3D12_DESCRIPTOR_HEAP_DESC hd;
		memset(&hd, 0, sizeof hd);
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 1;
		int ready = window && event
			&& SUCCEEDED(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue))
			&& SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&factory))
			&& SUCCEEDED(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)queue, window, &sd, NULL, NULL, &chain))
			&& SUCCEEDED(IDXGISwapChain1_QueryInterface(chain, &IID_IDXGISwapChain3, (void **)&chain3))
			&& SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
									 &IID_ID3D12CommandAllocator, (void **)&allocator))
			&& SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
								    &IID_ID3D12GraphicsCommandList, (void **)&list))
			&& SUCCEEDED(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs))
			&& SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
		check(ready, "a D3D12 swap chain on a window of its own", ready ? "made" : "refused");
		if (ready) ID3D12GraphicsCommandList_Close(list);
		UINT64 value = 0;
		int presented = 0;
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = { 0 };
		if (ready) ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs, &rtv);
		for (int frame = 0; ready && frame < 40; ++frame) {
			ID3D12Resource *back = NULL;
			UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(chain3);
			if (FAILED(IDXGISwapChain3_GetBuffer(chain3, index, &IID_ID3D12Resource, (void **)&back))) break;
			ID3D12CommandAllocator_Reset(allocator);
			ID3D12GraphicsCommandList_Reset(list, allocator, NULL);
			ID3D12Device_CreateRenderTargetView(device, back, NULL, rtv);
			D3D12_RESOURCE_BARRIER b;
			memset(&b, 0, sizeof b);
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = back;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
			const float colour[4] = { 0.0f, 0.6f, 0.0f, 1.0f };
			ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, colour, 0, NULL);
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
			ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
			ID3D12GraphicsCommandList_Close(list);
			ID3D12CommandList *lists[1] = { (ID3D12CommandList *)list };
			ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
			ID3D12Resource_Release(back);
			/* The game's frame is on the queue; the panel goes in front of the Present, after it. */
			if (SUCCEEDED(IDXGISwapChain3_Present(chain3, 0, 0))) ++presented;
			ID3D12CommandQueue_Signal(queue, fence, ++value);
			if (ID3D12Fence_GetCompletedValue(fence) < value) {
				ID3D12Fence_SetEventOnCompletion(fence, value, event);
				WaitForSingleObject(event, 5000);
			}
		}
		check(presented == 40, "every frame presents with the hook in front of it",
		      presented == 40 ? "40 of 40" : "a Present failed");

		FILE *f = fopen(dump, "rb");
		static unsigned char ppm[1280 * 720 * 3 + 64];
		size_t size = f ? fread(ppm, 1, sizeof ppm, f) : 0;
		if (f) fclose(f);
		check(size > 1280 * 720 * 3, "the panel's frame was written out", size ? dump : "no file");
		unsigned char rgb[3] = { 0, 0, 0 };
		char detail[96];
		int got = pixel(ppm, size, 1200, 400, rgb);
		snprintf(detail, sizeof detail, "(%u, %u, %u) at 1200,400", rgb[0], rgb[1], rgb[2]);
		check(got && green(rgb), "the game's frame is kept beside the panel", detail);
		got = pixel(ppm, size, 4, 4, rgb);
		snprintf(detail, sizeof detail, "(%u, %u, %u) at 4,4", rgb[0], rgb[1], rgb[2]);
		check(got && green(rgb), "and above it", detail);
		/* The panel: dark, slightly blue, over most of its 640x672 at (24,24). */
		int panel = 0, total = 0;
		for (int y = 60; y < 680; y += 8)
			for (int x = 40; x < 650; x += 8) {
				++total;
				if (pixel(ppm, size, x, y, rgb) && !green(rgb)) ++panel;
			}
		snprintf(detail, sizeof detail, "%d of %d samples inside it are not the game's", panel, total);
		check(total && panel * 10 >= total * 9, "the panel is drawn over the frame", detail);
		check(strstr(said_all, "the panel over the back buffer is in") != NULL,
		      "the panel said where it wrote the frame", "NR_MENU_DUMP");

		if (fence) ID3D12Fence_Release(fence);
		if (rtvs) ID3D12DescriptorHeap_Release(rtvs);
		if (list) ID3D12GraphicsCommandList_Release(list);
		if (allocator) ID3D12CommandAllocator_Release(allocator);
		if (chain3) IDXGISwapChain3_Release(chain3);
		if (chain) IDXGISwapChain1_Release(chain);
		if (factory) IDXGIFactory2_Release(factory);
		if (queue) ID3D12CommandQueue_Release(queue);
		if (event) CloseHandle(event);
		if (window) DestroyWindow(window);
		ID3D12Device_Release(device);
	}
out:
	DeleteFileW(host.answer);
	remove(host.settings);
	{
		char live[1200];
		snprintf(live, sizeof live, "%s.live", host.settings);
		remove(live);
	}
	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
	return failures ? 1 : 0;
}
