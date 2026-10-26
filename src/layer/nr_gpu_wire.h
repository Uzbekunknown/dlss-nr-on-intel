#ifndef NR_GPU_WIRE_H
#define NR_GPU_WIRE_H
/* The GPU transport's control messages, with no graphics API in them, so that a Vulkan
 * client (nr_gpu_bridge.h, the layer) and a D3D12 one (nr_gpu_d3d12.h, for the XeSS
 * proxy) speak one format to the daemon (nr_gpu_transport.py). A request is the 16-byte
 * header and the 128-byte packet, and the answer is the 16-byte reply; no pixels travel
 * this way. nr_gpu_rpc.h makes the exchange. */
#include <stdint.h>

#define NR_GPU_MAGIC 0x3147524eu
#define NR_GPU_VERSION 1u          /* a Vulkan client, with binary semaphores */
#define NR_GPU_FENCED_VERSION 2u   /* a Vulkan client, with private fences and ordered replies */
#define NR_GPU_D3D12_VERSION 3u    /* a D3D12 client, with one shared fence: below */
#define NR_GPU_HELLO 0u
#define NR_GPU_FRAME 1u
#define NR_GPU_RELEASE 2u
#define NR_GPU_SUCCESS 0u
#define NR_GPU_UNSUPPORTED 1u
#define NR_GPU_FAILURE 2u
#define NR_GPU_UUID_BYTES 16u

struct nr_gpu_header { uint32_t magic, width, height, format; };
struct nr_gpu_packet {
    /* Versions 1 and 2 carry opaque Win32 handles, and Vulkan import requires these
     * exact exporter memory type indices and allocation sizes. */
    uint32_t action, pid, width, height, format, input_memory_type, output_memory_type, version;
    uint64_t session, input_alloc, output_alloc, bytes;
    uint64_t input_memory, output_memory, ready_sem, done_sem;
    uint8_t device_uuid[NR_GPU_UUID_BYTES], driver_uuid[NR_GPU_UUID_BYTES];
};
struct nr_gpu_reply { uint32_t status, detail, session_low, session_high; };

/* Version 3, a D3D12 client on the network's adapter, reads the packet this way:
 * - input_memory and output_memory are NT handles, in the client's process, of committed
 *   buffers made with D3D12_HEAP_FLAG_SHARED, and input_alloc and output_alloc their
 *   widths. The output holds `bytes` of answer and a struct nr_gpu_stamp after them. The
 *   memory types are 0: the daemon asks its own driver.
 * - ready_sem is the NT handle of a fence made with D3D12_FENCE_FLAG_SHARED.
 * - done_sem is 0 in HELLO and RELEASE. In FRAME it is the value V that the client's
 *   queue signals once the input is written. The daemon waits for V and signals V + 1 once
 *   it is done with both buffers, whether it answered or not; it answered when the stamp
 *   names V. Nothing else signals V + 1, and V rises by at least 2 from frame to frame.
 *   The daemon never signals before V: D3D12 would take the fence back to V when the
 *   client's queue got there, and a wait for V + 1 would never end.
 * - device_uuid holds the adapter's LUID in its first 8 bytes; the rest of it and
 *   driver_uuid are zero. */
struct nr_gpu_stamp { uint32_t magic, frame_low, frame_high, reserved; };
#define NR_GPU_STAMP_MAGIC 0x5453524eu   /* "NRST" */

typedef char nr_gpu_packet_size_is_128[(sizeof(struct nr_gpu_packet) == 128) ? 1 : -1];
typedef char nr_gpu_header_size_is_16[(sizeof(struct nr_gpu_header) == 16) ? 1 : -1];
typedef char nr_gpu_reply_size_is_16[(sizeof(struct nr_gpu_reply) == 16) ? 1 : -1];
typedef char nr_gpu_stamp_size_is_16[(sizeof(struct nr_gpu_stamp) == 16) ? 1 : -1];
#endif /* NR_GPU_WIRE_H */
