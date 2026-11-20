/* nr_transport.h — the frame transport, in one place.
 *
 * Upstream this project talks to the daemon over a Unix socket, which is what the
 * Linux build does (NR_TRANSPORT_UNIX below, the default there). On Windows the same
 * conversation has to happen over a named pipe: Winsock's AF_UNIX exists but cannot
 * bind here (measured: bind() fails WSAENETDOWN 10050), and CPython on Windows does
 * not expose socket.AF_UNIX at all, so the daemon side could not answer even if the
 * layer could ask. A named pipe is the transport both ends can reach.
 *
 * Only the transport is different. The bytes on the wire — 16-byte header, then the
 * colour, then the interface mask for a masked frame — are unchanged, so a daemon on
 * either platform speaks the same protocol.
 *
 * The pipe is opened in byte mode and read with PeekNamedPipe + a deadline, because a
 * named pipe has no SO_RCVTIMEO: a blocking ReadFile waits forever when the daemon has
 * nothing to say, which is what the Unix build's 60-second timeout exists to prevent.
 */
#ifndef NR_TRANSPORT_H
#define NR_TRANSPORT_H

/* A static function that some includer will not call. `-Wunused-function` is right about almost
 * every static in this tree and wrong about the four helpers in this header, because each
 * translation unit takes the ones it needs. See `nr_link_read_nowait`. */
#if defined(__GNUC__) || defined(__clang__)
#define NR_UNUSED __attribute__((unused))
#elif defined(_MSC_VER)
#define NR_UNUSED
#else
#define NR_UNUSED
#endif

#ifdef _WIN32

#include <windows.h>
#include <stdio.h>   /* snprintf, used by both arms */

/* How long the layer gives a daemon it has never met to follow its pixels with a trailer. See
 * `nr_link_read_nowait`, which is where it is used and where the reasoning is. */
#define NR_LINK_TRAILER_BUDGET_US 2000

/* One connected endpoint. `handle` is a pipe handle; `deadline` is the same
 * whole-exchange budget the Unix build gives SO_SNDTIMEO/SO_RCVTIMEO. */
typedef struct {
	HANDLE handle;
	DWORD deadline_ms;
} nr_link;

static int nr_link_connect(nr_link *link, const char *path)
{
	link->handle = INVALID_HANDLE_VALUE;
	link->deadline_ms = 60000;

	/* The daemon creates the pipe before it listens, so the first attempt can
	 * catch ERROR_FILE_NOT_FOUND while it is still starting. WaitNamedPipe does
	 * not distinguish "not yet" from "never", so this polls instead. */
	for (int attempt = 0; attempt < 100; attempt++) {
		HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
				       OPEN_EXISTING, 0, NULL);
		if (h != INVALID_HANDLE_VALUE) {
			DWORD mode = PIPE_READMODE_BYTE;
			SetNamedPipeHandleState(h, &mode, NULL, NULL);
			link->handle = h;
			return 0;
		}
		DWORD e = GetLastError();
		if (e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND) return -1;
		Sleep(50);
	}
	return -1;
}

static void nr_link_close(nr_link *link)
{
	if (link->handle != INVALID_HANDLE_VALUE) CloseHandle(link->handle);
	link->handle = INVALID_HANDLE_VALUE;
}

/* A connection carried as an int, so callers that keep one across calls do not have to
 * name the platform type. Only ever round-tripped through the pair below. */
static int nr_link_handle(nr_link link)
{
	return (int)(intptr_t)link.handle;
}

static nr_link nr_link_from_handle(int fd)
{
	nr_link link;
	link.handle = (HANDLE)(intptr_t)fd;
	link.deadline_ms = 60000;
	return link;
}

/* Send exactly `count` bytes, or fail. The layer's three writes are small
 * (16-byte header, then the colour, then nothing) but the colour is a whole
 * frame, so this loops the way the Unix build's send() loop does. */
static int nr_link_write(nr_link *link, const void *data, size_t count)
{
	const unsigned char *p = data;
	DWORD budget = link->deadline_ms;
	while (count > 0) {
		DWORD chunk = (DWORD)(count > (1u << 20) ? (1u << 20) : count);
		DWORD written = 0;
		if (!WriteFile(link->handle, p, chunk, &written, NULL)) return -1;
		if (written == 0) return -1;
		p += written;
		count -= written;
		(void)budget;
	}
	return 0;
}

/* Read exactly `count` bytes within the deadline, or fail.
 *
 * PeekNamedPipe first: ReadFile on a byte-mode pipe returns whatever is there,
 * including zero bytes, so a single blocking ReadFile cannot express "wait until
 * count bytes exist, but not past the deadline" the way read() on a socket can. */
static int nr_link_read(nr_link *link, void *data, size_t count)
{
	unsigned char *p = data;
	DWORD waited = 0;                  /* milliseconds spent waiting */
	const DWORD started = GetTickCount();
	while (count > 0) {
		DWORD available = 0;
		if (!PeekNamedPipe(link->handle, NULL, 0, NULL, &available, NULL))
			return -1;
		if (available == 0) {
			waited = GetTickCount() - started;
			if (waited >= link->deadline_ms) return -1;
			Sleep(10);
			waited += 10;
			continue;
		}
		DWORD want = (DWORD)(count < available ? count : available);
		DWORD got = 0;
		if (!ReadFile(link->handle, p, want, &got, NULL)) return -1;
		if (got == 0) return -1;
		p += got;
		count -= got;
	}
	return 0;
}

/* **Read what is already there, and give up at once if it is not.**
 *
 * The legacy answer's optional trailer (the network's time, appended by a daemon that
 * has it) arrives a moment after the pixels, and whether it arrives at all says which
 * daemon is answering. `nr_link_read` cannot tell those apart: its deadline is the
 * exchange's whole 60-second budget, so waiting in it for bytes an old daemon will
 * never send would hang every frame. This waits a few milliseconds at most — enough
 * for a trailer that is in flight, nothing for one that does not exist.
 *
 * Returns 0 and fills `data` when `count` bytes were read; returns 1 when the peer
 * has nothing (or not enough yet) — not an error, just no trailer this frame.
 *
 * **`NR_UNUSED` because a unit that includes this header need not read a trailer.** Every
 * function here is `static` and every translation unit that wants one gets its own copy, so a
 * file that wants `nr_link_read` and not this used to warn under MinGW: the same
 * `-Wunused-function` the other statics give there. Marking it is the honest answer; the
 * alternative is a per-file define that lies about how many callers there are. */
NR_UNUSED static int nr_link_read_nowait(nr_link *link, void *data, size_t count)
{
	unsigned char *p = data;
	/* **A wall-clock budget, measured on a clock that can resolve it.** Two mistakes are
	 * possible here and this function has now made both:
	 *
	 *   - counting Sleep(1) calls and calling the count milliseconds. Sleep(1) rounds up to
	 *     the scheduler tick - 15.6 ms by default, and nothing in this tree calls
	 *     timeBeginPeriod - so "30 sleeps" was anywhere from 30 ms to nearly half a second.
	 *   - measuring with GetTickCount, which fixes the counting but not the resolution: the
	 *     tick is the same 15.6 ms, so "2 ms" was really "somewhere in the next tick", and
	 *     `Sleep(0)` spinning against it burned a tick's worth of presents before giving up.
	 *     Measured by the reviewer against a stand-in that closes 10 ms after the pixels: six
	 *     milliseconds a frame more than the client before this PR.
	 *
	 * QueryPerformanceCounter is the machine's high-resolution counter and costs about what
	 * GetTickCount does. The budget below is really two milliseconds.
	 *
	 * **And nothing inside the budget sleeps a tick.** A `Sleep(1)` here, after a few dozen
	 * `Sleep(0)`, was meant to let a starved writer run; but `Sleep(1)` sleeps to the next
	 * scheduler tick, 15.6 ms unless the process has raised its timer resolution (the layer
	 * never does, a game may or may not), and the clock above is read only when it wakes.
	 * The few dozen yields end well inside two milliseconds, so every frame without a trailer
	 * reached it. Measured on 11-19 (Arc 140V): this function on a connected pipe that holds
	 * nothing and stays open, 200 calls back to back, median: 15.7 ms with the `Sleep(1)` (2.9
	 * once the process has set a 1 ms timer, as some games do), 16.0 with GetTickCount, 2.00
	 * with `Sleep(0)` alone, with either timer. A writer starved for the whole budget costs one
	 * frame's trailer, which is all the budget is for. */
	LARGE_INTEGER freq, started;
	if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) {
		/* No clock to bound the wait by: take what is there and do not wait at all. */
		freq.QuadPart = 0;
	}
	QueryPerformanceCounter(&started);
	while (count > 0) {
		DWORD available = 0;
		if (!PeekNamedPipe(link->handle, NULL, 0, NULL, &available, NULL))
			return -1;
		if (available == 0) {
			/* Nothing YET is not nothing EVER: a large answer streams through a
			 * 1 MB pipe buffer while the layer reads it, so the trailer - the tail
			 * of the daemon's one `sendall` - can still be queued when the pixels
			 * finish arriving. Measured on a 1280x720 legacy frame, the daemon's
			 * write of the last bytes landed within a millisecond of the pixels.
			 *
			 * TWO milliseconds, once, for a daemon that will never send a trailer
			 * at all: that is the price of asking a version we cannot identify,
			 * against the seven-plus milliseconds of network work before it. */
			if (!freq.QuadPart) return 1;
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			const LONGLONG elapsed_us =
				(now.QuadPart - started.QuadPart) * 1000000 / freq.QuadPart;
			if (elapsed_us >= NR_LINK_TRAILER_BUDGET_US) return 1;
			Sleep(0);
			continue;
		}
		DWORD want = (DWORD)(count < available ? count : available);
		DWORD got = 0;
		if (!ReadFile(link->handle, p, want, &got, NULL)) return -1;
		if (got == 0) return -1;
		p += got;
		count -= got;
		/* A partial read means the trailer straddles a pipe boundary; give the
		 * rest a moment rather than dropping half a magic word on the floor -
		 * still inside the same two-millisecond budget. */
	}
	return 0;
}
#else  /* the Linux build, unchanged: a Unix socket */

#include <sys/socket.h>
#include <sys/types.h>   /* ssize_t, which <sys/socket.h> does not promise */
#include <sys/un.h>
#include <sys/time.h>
/* read(), write() and close() come from <unistd.h>; errno and EINTR from <errno.h>.
 * <stdio.h> is for snprintf below — this header is included before the includer's own
 * stdio in one of its two callers, so it cannot rely on theirs. */
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

typedef struct {
	int fd;
} nr_link;

static int nr_link_connect(nr_link *link, const char *path)
{
	link->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (link->fd < 0) return -1;
	struct timeval timeout = { .tv_sec = 60 };
	if (setsockopt(link->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout) ||
	    setsockopt(link->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout)) {
		close(link->fd);
		link->fd = -1;
		return -1;
	}
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
	if (connect(link->fd, (struct sockaddr *)&address, sizeof address) < 0) {
		close(link->fd);
		link->fd = -1;
		return -1;
	}
	return 0;
}

static void nr_link_close(nr_link *link)
{
	if (link->fd >= 0) close(link->fd);
	link->fd = -1;
}

/* A connection carried as an int, so callers that keep one across calls do not have to
 * name the platform type. Only ever round-tripped through the pair below. */
static int nr_link_handle(nr_link link)
{
	return link.fd;
}

static nr_link nr_link_from_handle(int fd)
{
	nr_link link;
	link.fd = fd;
	return link;
}

static int nr_link_write(nr_link *link, const void *data, size_t count)
{
	const unsigned char *p = data;
	while (count > 0) {
		ssize_t n = send(link->fd, p, count, MSG_NOSIGNAL);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n;
		count -= (size_t)n;
	}
	return 0;
}

static int nr_link_read(nr_link *link, void *data, size_t count)
{
	unsigned char *p = data;
	while (count > 0) {
		ssize_t n = read(link->fd, p, count);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n;
		count -= (size_t)n;
	}
	return 0;
}

/* The Unix twin of the Windows `nr_link_read_nowait` above: take what has arrived,
 * never wait. `MSG_DONTWAIT` fails with EAGAIN when the socket has nothing, which is
 * the "no trailer this frame" case, not an error. A partial read waits a moment for
 * the rest, as the Windows one does — the trailer is 8 bytes and straddles nothing
 * bigger than itself, but a stream has no message boundaries to trust. */
static int nr_link_read_nowait(nr_link *link, void *data, size_t count)
{
	unsigned char *p = data;
	int waited = 0;
	while (count > 0) {
		ssize_t n = recv(link->fd, p, count, MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) return -1;
		p += n;
		count -= (size_t)n;
		if (count > 0) {
			if (waited >= 20) return 1;
			usleep(1000);
			waited += 1;
		}
	}
	return 0;
}

#endif

#endif
