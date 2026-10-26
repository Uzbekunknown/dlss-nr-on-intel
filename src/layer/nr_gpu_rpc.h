#ifndef NR_GPU_RPC_H
#define NR_GPU_RPC_H
/* Small control exchanges for the Win32 GPU transport, for a Vulkan client (the
 * bridge) or a D3D12 one. This never sends pixels and never uses the CPU frame
 * transport's connection state. Every exchange has one deadline, including pipe
 * connection. Cancelled OVERLAPPED storage remains alive until Windows confirms
 * completion. */
#ifdef _WIN32
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "nr_gpu_wire.h"

#define NR_GPU_RPC_TIMEOUT_MS 10000u
#define NR_GPU_RPC_CANCEL_GRACE_MS 100u
#define NR_GPU_RPC_REQUEST_BYTES (sizeof(struct nr_gpu_header) + sizeof(struct nr_gpu_packet))

struct nr_gpu_rpc_io {
    OVERLAPPED overlapped;
    HANDLE pipe;
    unsigned char bytes[NR_GPU_RPC_REQUEST_BYTES];
    struct nr_gpu_rpc_io *next;
    int pending;
};

/* Normally empty: named-pipe cancellation completes immediately. A broken or
 * unusually slow driver must not make the game's present wait forever, nor
 * receive pointers to a returned stack frame. The next exchange or an explicit
 * reap releases these small records only after completion. */
static struct nr_gpu_rpc_io *nr_gpu_rpc_retired;
static SRWLOCK nr_gpu_rpc_retired_lock = SRWLOCK_INIT;

static void nr_gpu_rpc_dispose(struct nr_gpu_rpc_io *io)
{
    if (io->overlapped.hEvent) CloseHandle(io->overlapped.hEvent);
    if (io->pipe != INVALID_HANDLE_VALUE) CloseHandle(io->pipe);
    free(io);
}

static int nr_gpu_rpc_completed(struct nr_gpu_rpc_io *io, DWORD *transferred)
{
    if (GetOverlappedResult(io->pipe, &io->overlapped, transferred, FALSE)) return 1;
    DWORD error = GetLastError();
    return error != ERROR_IO_INCOMPLETE && error != ERROR_IO_PENDING;
}

static void nr_gpu_rpc_reap(void)
{
    DWORD saved = GetLastError();
    AcquireSRWLockExclusive(&nr_gpu_rpc_retired_lock);
    struct nr_gpu_rpc_io **link = &nr_gpu_rpc_retired;
    while (*link) {
        struct nr_gpu_rpc_io *io = *link;
        DWORD transferred = 0;
        if (nr_gpu_rpc_completed(io, &transferred)) {
            *link = io->next;
            nr_gpu_rpc_dispose(io);
        } else link = &io->next;
    }
    ReleaseSRWLockExclusive(&nr_gpu_rpc_retired_lock);
    SetLastError(saved);
}

static void nr_gpu_rpc_close(struct nr_gpu_rpc_io *io)
{
    DWORD saved = GetLastError();
    if (io->pending) {
        CancelIoEx(io->pipe, &io->overlapped);
        WaitForSingleObject(io->overlapped.hEvent, NR_GPU_RPC_CANCEL_GRACE_MS);
        DWORD transferred = 0;
        if (!nr_gpu_rpc_completed(io, &transferred)) {
            AcquireSRWLockExclusive(&nr_gpu_rpc_retired_lock);
            io->next = nr_gpu_rpc_retired;
            nr_gpu_rpc_retired = io;
            ReleaseSRWLockExclusive(&nr_gpu_rpc_retired_lock);
            SetLastError(saved);
            return;
        }
    }
    nr_gpu_rpc_dispose(io);
    SetLastError(saved);
}

static DWORD nr_gpu_rpc_remaining(ULONGLONG deadline)
{
    ULONGLONG now = GetTickCount64();
    return now >= deadline ? 0 : (DWORD)(deadline - now);
}

static int nr_gpu_rpc_transfer(struct nr_gpu_rpc_io *io, int writing,
                               size_t offset, DWORD length, ULONGLONG deadline)
{
    while (length) {
        DWORD remaining = nr_gpu_rpc_remaining(deadline);
        if (!remaining) { SetLastError(ERROR_TIMEOUT); return 0; }
        HANDLE event = io->overlapped.hEvent;
        memset(&io->overlapped, 0, sizeof io->overlapped);
        io->overlapped.hEvent = event;
        if (!ResetEvent(event)) return 0;
        DWORD transferred = 0;
        BOOL result = writing
            ? WriteFile(io->pipe, io->bytes + offset, length, &transferred, &io->overlapped)
            : ReadFile(io->pipe, io->bytes + offset, length, &transferred, &io->overlapped);
        if (!result) {
            if (GetLastError() != ERROR_IO_PENDING) return 0;
            io->pending = 1;
            remaining = nr_gpu_rpc_remaining(deadline);
            DWORD wait = remaining ? WaitForSingleObject(event, remaining) : WAIT_TIMEOUT;
            if (wait != WAIT_OBJECT_0) {
                DWORD error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
                SetLastError(error); return 0;
            }
            if (!GetOverlappedResult(io->pipe, &io->overlapped, &transferred, FALSE)) {
                DWORD error = GetLastError();
                if (error != ERROR_IO_INCOMPLETE && error != ERROR_IO_PENDING) io->pending = 0;
                SetLastError(error); return 0;
            }
            io->pending = 0;
        }
        if (!transferred || transferred > length) { SetLastError(ERROR_BAD_LENGTH); return 0; }
        offset += transferred;
        length -= transferred;
    }
    return 1;
}

/* Returns 1 for a complete, session-matching reply, including UNSUPPORTED or
 * FAILURE status. The caller checks reply.status. HELLO success's detail is the
 * peer PID. A transport failure returns 0, clears reply and preserves the Win32
 * error. Losing a reply does not prove the peer stopped using exported buffers;
 * the caller owns RELEASE acknowledgement / peer-exit policy. */
static int nr_gpu_rpc_peer(const char *socket_path, const struct nr_gpu_packet *packet,
                           struct nr_gpu_reply *reply, DWORD *peer_pid)
{
    nr_gpu_rpc_reap();
    if (reply) memset(reply, 0, sizeof *reply);
    if (peer_pid) *peer_pid = 0;
    if (!socket_path || strncmp(socket_path, "\\\\.\\pipe\\", 9)
        || !packet || !reply || !packet->session
        || (packet->version != NR_GPU_VERSION && packet->version != NR_GPU_FENCED_VERSION
            && packet->version != NR_GPU_D3D12_VERSION)
        || packet->action > NR_GPU_RELEASE) {
        SetLastError(ERROR_INVALID_PARAMETER); return 0;
    }
    struct nr_gpu_rpc_io *io = (struct nr_gpu_rpc_io *)calloc(1, sizeof *io);
    if (!io) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    io->pipe = INVALID_HANDLE_VALUE;
    ULONGLONG deadline = GetTickCount64() + NR_GPU_RPC_TIMEOUT_MS;
    while (nr_gpu_rpc_remaining(deadline)) {
        io->pipe = CreateFileA(socket_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (io->pipe != INVALID_HANDLE_VALUE) break;
        DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
            nr_gpu_rpc_close(io); return 0;
        }
        DWORD remaining = nr_gpu_rpc_remaining(deadline);
        if (remaining) Sleep(remaining < 10u ? remaining : 10u);
    }
    if (io->pipe == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_TIMEOUT); nr_gpu_rpc_close(io); return 0;
    }
    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(io->pipe, &mode, NULL, NULL)) {
        nr_gpu_rpc_close(io); return 0;
    }
    ULONG server_pid = 0;
    if ((packet->action == NR_GPU_HELLO || peer_pid)
        && !GetNamedPipeServerProcessId(io->pipe, &server_pid)) {
        nr_gpu_rpc_close(io); return 0;
    }
    /* Preserve the OS-authenticated peer even when a HELLO response is lost;
     * imports may already exist and the caller must retain them until RELEASE
     * acknowledgement or confirmed peer exit. This is never a wire-provided PID. */
    if (peer_pid) *peer_pid = server_pid;
    io->overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!io->overlapped.hEvent) { nr_gpu_rpc_close(io); return 0; }
    struct nr_gpu_header header = {NR_GPU_MAGIC, packet->width, packet->height, packet->format};
    memcpy(io->bytes, &header, sizeof header);
    memcpy(io->bytes + sizeof header, packet, sizeof *packet);
    if (!nr_gpu_rpc_transfer(io, 1, 0, (DWORD)NR_GPU_RPC_REQUEST_BYTES, deadline)
        || !nr_gpu_rpc_transfer(io, 0, 0, sizeof *reply, deadline)) {
        nr_gpu_rpc_close(io); return 0;
    }
    struct nr_gpu_reply received;
    memcpy(&received, io->bytes, sizeof received);
    uint64_t echoed = ((uint64_t)received.session_high << 32) | received.session_low;
    if (echoed != packet->session || received.status > NR_GPU_FAILURE
        || (packet->action == NR_GPU_HELLO && received.status == NR_GPU_SUCCESS
            && (!server_pid || received.detail != server_pid))) {
        SetLastError(ERROR_INVALID_DATA); nr_gpu_rpc_close(io); return 0;
    }
    *reply = received;
    SetLastError(ERROR_SUCCESS);
    nr_gpu_rpc_close(io);
    return 1;
}

static int nr_gpu_rpc(const char *socket_path, const struct nr_gpu_packet *packet,
                      struct nr_gpu_reply *reply)
{
    return nr_gpu_rpc_peer(socket_path, packet, reply, NULL);
}
#endif /* _WIN32 */
#endif /* NR_GPU_RPC_H */
