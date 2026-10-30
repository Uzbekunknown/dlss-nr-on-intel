"""nr_pipe — the Windows named-pipe transport for the layer/daemon pair.

The Linux build speaks over a Unix socket: the daemon `bind()`s, the layer
`connect()`s, and both use the byte stream unchanged. On Windows the two ends
meet on a named pipe instead — Winsock's `AF_UNIX` cannot bind here and CPython
has no `socket.AF_UNIX` at all, so a Win32 named pipe is the only transport
both sides can reach (see the t15 build notes, section A.7).

The bytes are unchanged: 16-byte header, then the colour, then the mask for a
masked frame. Only the endpoint changes, so this module reproduces just enough
of the `socket` interface for `nr_daemon` — `.accept()` on the server, and
`.recv()` / `.recv_into()` / `.sendall()` / `.settimeout()` / `.close()` on a
connection — so the daemon's frame loop stays as it is.

`CreateNamedPipe` is reached through `ctypes` on kernel32: the machine has no
`pywin32`, and the standard library is enough (measured in section A.7.9).
"""
import ctypes
import errno
import time
from ctypes import wintypes

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)

PIPE_ACCESS_DUPLEX = 0x00000003
PIPE_TYPE_BYTE = 0x00000000
PIPE_READMODE_BYTE = 0x00000000
PIPE_WAIT = 0x00000000

ERROR_PIPE_CONNECTED = 535
ERROR_PIPE_BUSY = 231        # every instance is in use; retry, it is not a failure
ERROR_BROKEN_PIPE = 109
ERROR_NO_DATA = 232

INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value

_k32.CreateNamedPipeW.restype = wintypes.HANDLE
_k32.CreateNamedPipeW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
    wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p]
_k32.ConnectNamedPipe.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
_k32.ConnectNamedPipe.restype = wintypes.BOOL
_k32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                          ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_k32.ReadFile.restype = wintypes.BOOL
_k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                           ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
_k32.WriteFile.restype = wintypes.BOOL
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_k32.CloseHandle.restype = wintypes.BOOL
_k32.DisconnectNamedPipe.argtypes = [wintypes.HANDLE]
_k32.DisconnectNamedPipe.restype = wintypes.BOOL

# The client end, for the tests: the layer is C and opens its own handle.
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3

_k32.CreateFileW.restype = wintypes.HANDLE
_k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                             wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]


class NamedPipeConnection:
    """One connected byte-mode pipe endpoint, shaped like a socket."""

    def __init__(self, handle, timeout=None):
        self._handle = int(handle)
        self._timeout = timeout
        self._open = True

    def settimeout(self, seconds):
        self._timeout = seconds

    def recv(self, count):
        """Read up to `count` bytes; b'' when the peer closed cleanly."""
        buf = bytearray(count)
        return bytes(buf[:self.recv_into(buf, count)])

    def recv_into(self, buffer, count=0):
        """Read up to `count` bytes (all `buffer` holds when 0) straight into `buffer`, as a
        socket's `recv_into` does; 0 when the peer closed cleanly. `nr_daemon.receive` takes
        this path: a frame then lands in its buffer with no copy on the way, where reading
        each megabyte into a fresh buffer of its own, copying it out and joining the pieces
        took 3.5 ms of a 1280x720 frame against Linux's 0.9."""
        if not self._open:
            return 0
        view = memoryview(buffer).cast("B")
        count = min(count or len(view), len(view))
        if not count:
            return 0
        target = (ctypes.c_char * count).from_buffer(view)
        got = wintypes.DWORD(0)
        if _k32.ReadFile(self._handle, target, count, ctypes.byref(got), None):
            return got.value
        err = ctypes.get_last_error()
        if err in (ERROR_BROKEN_PIPE, ERROR_NO_DATA):
            # The layer closed its end: clean EOF, not a failure.
            self._open = False
            return 0
        # A byte-mode pipe on a blocking ReadFile returns only when data
        # arrives or the peer closes; this path is unexpected rather than a
        # timeout, so it surfaces as an error the daemon's frame loop can
        # already swallow ("frame rejected/failed").
        raise OSError(err, "ReadFile")

    def sendall(self, data):
        """Write all of `data` from where it lies. A writable buffer — the answer the daemon
        composes into the request's own bytes — is handed to WriteFile as it is, and so is
        `bytes`; copying the frame twice on the way out took 2.7 ms of a 1280x720 frame
        against Linux's 0.75. WriteFile on this blocking pipe returns once the bytes are the
        pipe's, so the caller may reuse its buffer straight after."""
        view = memoryview(data).cast("B")
        total = len(view)
        if not total:
            return
        if view.readonly:
            whole = data if isinstance(data, bytes) else view.tobytes()
            holder = ctypes.c_char_p(whole)
            base = ctypes.cast(holder, ctypes.c_void_p).value
        else:
            holder = (ctypes.c_char * total).from_buffer(view)
            base = ctypes.addressof(holder)
        sent = 0
        while sent < total:
            written = wintypes.DWORD(0)
            ok = _k32.WriteFile(self._handle, ctypes.c_void_p(base + sent), total - sent,
                                ctypes.byref(written), None)
            if not ok:
                err = ctypes.get_last_error()
                if err == ERROR_BROKEN_PIPE:
                    self._open = False
                    raise BrokenPipeError(err, "WriteFile")
                raise OSError(err, "WriteFile")
            if written.value == 0:
                raise OSError(errno.EPIPE, "WriteFile wrote nothing")
            sent += written.value
        del holder

    def close(self):
        # EOF/broken-pipe marks the stream unusable but does not release its handle.
        # Status probes close without sending a header, so this distinction matters
        # even when every real frame succeeds. Release once, independent of _open.
        if self._handle is not None:
            _k32.CloseHandle(self._handle)
            self._handle = None
        self._open = False

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class NamedPipeServer:
    """A byte-mode named pipe the layer connects to, shaped like a listening socket.

    `instances` pre-creates that many endpoints. One is enough for the daemon and the layer - a
    client connects while the daemon is inside `accept()` on that one - but a test that queues
    several clients before reading any of them needs them all to exist already: a pipe created
    with `MaxInstances` = 4 still refuses the second `CreateFile` with ERROR_PIPE_BUSY until a
    second instance has actually been made, which `accept()` would otherwise only do after the
    first hand-over.
    """

    def __init__(self, path, backlog=4, buffer_size=1 << 20, timeout=None, instances=1):
        self.path = path
        self.backlog = backlog
        self.buffer_size = buffer_size
        self._timeout = timeout
        self._handles = [self._make() for _ in range(max(1, instances))]

    def settimeout(self, seconds):
        """Accepted connections carry this; kept so a test can use one shape on both platforms."""
        self._timeout = seconds

    def _make(self, attempts=50):
        """Create one pipe instance.

        ERROR_PIPE_BUSY means the instance count is momentarily at `backlog` — the
        normal state while accepted connections are still open. It is not fatal: wait
        and retry, and only give up when the caller really cannot get an endpoint.
        """
        for attempt in range(attempts):
            handle = _k32.CreateNamedPipeW(
                self.path, PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                self.backlog, self.buffer_size, self.buffer_size, 0, None)
            value = int(handle)
            if value != INVALID_HANDLE_VALUE:
                return value
            err = ctypes.get_last_error()
            if err != ERROR_PIPE_BUSY:
                raise OSError(err, "CreateNamedPipeW")
            time.sleep(0.02 * (attempt + 1))
        raise OSError(ctypes.get_last_error(), "CreateNamedPipeW")

    def accept(self):
        """Wait for one client, then return a connected `NamedPipeConnection`."""
        if not self._handles:
            # Nothing is listening: every instance is in use, or the last one was handed over and
            # the top-up below could not make one. Wait for it, because the caller has no other
            # endpoint.
            self._handles = [self._make()]
        handle = self._handles.pop(0)
        ok = _k32.ConnectNamedPipe(handle, None)
        if not ok:
            err = ctypes.get_last_error()
            if err != ERROR_PIPE_CONNECTED:
                raise OSError(err, "ConnectNamedPipe")
        connection = NamedPipeConnection(handle, self._timeout)
        # A fresh pipe instance replaces the one just handed over, so the next `accept` waits on a
        # new endpoint rather than the one in use. ONE attempt on purpose: with `MaxInstances` at its
        # limit - which is the normal state for a backlog of 1 while the connection just handed over
        # is still open - every further attempt gets ERROR_PIPE_BUSY, and `_make`'s retry budget is
        # 50 back-offs, about 25 seconds. That stall was inside `accept`, so it delayed handling of
        # the connection it had ALREADY accepted: a client that talked and waited looked like a dead
        # lock. Letting the top-up fail costs the next accept one `_make`, not this frame a minute.
        if len(self._handles) < self.backlog:
            try:
                self._handles.append(self._make(attempts=1))
            except OSError:
                pass
        return connection, None

    def close(self):
        for handle in self._handles:
            _k32.DisconnectNamedPipe(handle)
            _k32.CloseHandle(handle)
        self._handles = []


class NamedPipeClient(NamedPipeConnection):
    """The connecting end, shaped like a socket.

    The daemon and the layer never needed this - the layer is C and opens the pipe itself - but
    the daemon's exchange tests do, and on Windows there is no `socket.AF_UNIX` to stand in for
    it. A pipe is also the truer subject than a socket would be: `NR_LAYER_SOCKET` names a named
    pipe there, and the bytes go through `WriteFile`/`ReadFile` either way.

    ERROR_PIPE_BUSY is retried, not raised: a peer sitting in `accept()` has its instance in use
    while it hands the previous one over, and that is the normal transition, so a client that
    gave up on it would fail on a busy daemon rather than a broken one.
    """

    def __init__(self, path, timeout=None, attempts=250):
        handle = int(INVALID_HANDLE_VALUE)
        error = 0
        for _ in range(attempts):
            raw = _k32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, None,
                                   OPEN_EXISTING, 0, None)
            handle = int(raw)
            if handle != INVALID_HANDLE_VALUE:
                break
            error = ctypes.get_last_error()
            if error != ERROR_PIPE_BUSY:
                raise OSError(error, "CreateFileW")
            time.sleep(0.02)
        if handle == INVALID_HANDLE_VALUE:
            raise OSError(error, "CreateFileW")
        super().__init__(handle, timeout)


def server(path, backlog=4, timeout=None, instances=1):
    return NamedPipeServer(path, backlog=backlog, timeout=timeout, instances=instances)


def client(path, timeout=None):
    return NamedPipeClient(path, timeout=timeout)
