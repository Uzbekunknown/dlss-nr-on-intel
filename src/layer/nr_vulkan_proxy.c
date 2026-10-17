/* nr_vulkan_proxy.c - vulkan-1.dll beside a game: NR's environment, whatever starts the game.
 *
 * The layer is found and configured through environment variables, and only setup's own
 * Launch game, or Steam launch options pointing at its wrapper, used to give them. A game
 * started from a shortcut, from Epic or GOG, or from Steam's Play button ran without NR.
 *
 * Placed beside the game's executable as vulkan-1.dll, this is what the game loads: Windows
 * looks in the executable's folder before System32, for the game's own imports and for DXVK's
 * LoadLibrary inside it alike. When it is loaded it reads dlss-nr\nr-env.txt, sets those
 * variables in the game's process before any Vulkan instance exists, and records the launch
 * for setup's status. Every function the loader exports is passed on unchanged to the real
 * loader: System32's, or the game's own copy if it brought one (setup sets that aside).
 *
 * Only in the process of the game named in nr-env.txt: a launcher or a crash reporter in the
 * same folder gets the plain loader. DISABLE_NR_PROXY=1 turns all of it off. In the game's
 * process an inherited NR_LAYER_SPAWNED, the mark of a daemon NR started, is taken away.
 *
 * nr-env.txt is UTF-8, one NAME=value a line. Three names are the proxy's own and are not set:
 *   NR_GAME_EXE      the executable whose process this is for
 *   NR_REAL_VULKAN   the loader to pass calls to, when not System32's
 *   NR_LAUNCH_STATE  where to record the launch (setup's launch-state.json)
 *
 * The folder the game was started in goes into dlss-nr\start-folders.txt, for Remove NR: DXVK
 * opens its first log there before it loads this DLL, so before DXVK_LOG_PATH is set.
 *
 * The launch record carries the process's integrity level. Windows' Vulkan loader ignores
 * VK_LAYER_PATH in a process at High integrity or above, as administrator or with UAC off
 * (Vulkan-Loader's is_high_integrity): such a game gets these variables and never NR's layer,
 * and setup's status says so.
 */
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <vulkan/vulkan.h> /* with its prototypes: the wrappers must match them */
#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib") /* the token: integrity_level */
#endif

static wchar_t real_path[MAX_PATH * 2];
static HMODULE real_loader;
static INIT_ONCE real_once = INIT_ONCE_STATIC_INIT;

/* Not in DllMain: loading a DLL while the loader lock is held is how processes deadlock. */
static BOOL CALLBACK load_real(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	wchar_t path[MAX_PATH * 2];
	(void)once; (void)parameter; (void)context;
	if (real_path[0]) {
		lstrcpynW(path, real_path, MAX_PATH * 2);
	} else {
		/* System32 is SysWOW64 for a 32-bit game, by the file system's own redirection. */
		UINT length = GetSystemDirectoryW(path, MAX_PATH);
		if (!length || length >= MAX_PATH) return TRUE;
		lstrcatW(path, L"\\vulkan-1.dll");
	}
	real_loader = LoadLibraryW(path);
	return TRUE;
}

static PFN_vkVoidFunction real(const char *name)
{
	InitOnceExecuteOnce(&real_once, load_real, NULL, NULL);
	return real_loader ? (PFN_vkVoidFunction)(void *)GetProcAddress(real_loader, name) : NULL;
}

#define NR_PROXY(type, name, params, args, failure) \
	VKAPI_ATTR type VKAPI_CALL name params \
	{ \
		static PFN_##name next; \
		if (!next) next = (PFN_##name)real(#name); \
		if (!next) return failure; \
		return next args; \
	}
#define NR_PROXY_VOID(name, params, args) \
	VKAPI_ATTR void VKAPI_CALL name params \
	{ \
		static PFN_##name next; \
		if (!next) next = (PFN_##name)real(#name); \
		if (next) next args; \
	}
#include "nr_vulkan_proxy_exports.h"

/* A JSON string of `text`, quotes included, appended to `out`. */
static void json_string(wchar_t *out, size_t size, const wchar_t *text)
{
	size_t used = wcslen(out);
	if (used + 2 >= size) return;
	out[used++] = L'"';
	for (; *text && used + 7 < size; text++) {
		if (*text == L'"' || *text == L'\\') { out[used++] = L'\\'; out[used++] = *text; }
		else if (*text < 0x20) used += swprintf(out + used, size - used, L"\\u%04x", (unsigned)*text);
		else out[used++] = *text;
	}
	out[used++] = L'"';
	out[used] = 0;
}

/* The same file by identity, not by spelling: a junction, an 8.3 name or a drive's case gives
 * another path to the one executable. */
static int same_file(const wchar_t *a, const wchar_t *b)
{
	BY_HANDLE_FILE_INFORMATION info[2];
	const wchar_t *paths[2] = { a, b };
	for (int i = 0; i < 2; i++) {
		HANDLE file = CreateFileW(paths[i], 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE) return 0;
		BOOL ok = GetFileInformationByHandle(file, &info[i]);
		CloseHandle(file);
		if (!ok) return 0;
	}
	return info[0].dwVolumeSerialNumber == info[1].dwVolumeSerialNumber
	    && info[0].nFileIndexHigh == info[1].nFileIndexHigh
	    && info[0].nFileIndexLow == info[1].nFileIndexLow;
}

static const wchar_t *lookup(wchar_t **names, wchar_t **values, int count, const wchar_t *name)
{
	for (int i = 0; i < count; i++)
		if (CompareStringOrdinal(names[i], -1, name, -1, TRUE) == CSTR_EQUAL) return values[i];
	return NULL;
}

/* This process's mandatory integrity level, the last part of its label's SID: 0x2000 medium,
 * 0x3000 high, the loader's limit. -1 when the token cannot be read. */
static long integrity_level(void)
{
	HANDLE token;
	union {
		TOKEN_MANDATORY_LABEL label;
		BYTE bytes[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
	} buffer;
	DWORD size = 0;
	long level = -1;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return -1;
	if (GetTokenInformation(token, TokenIntegrityLevel, &buffer, sizeof buffer, &size)) {
		UCHAR count = *GetSidSubAuthorityCount(buffer.label.Label.Sid);
		if (count) level = (long)*GetSidSubAuthority(buffer.label.Label.Sid, count - 1u);
	}
	CloseHandle(token);
	return level;
}

/* What setup's status reads to tell a running game from an old log: this process, its game,
 * its integrity level, and how much of the daemon's log was there before it. Written beside,
 * then moved over. */
static void record_launch(const wchar_t *path, const wchar_t *root, const wchar_t *game,
			  const wchar_t *log)
{
	static wchar_t text[8192];
	wchar_t temporary[MAX_PATH * 2 + 16], integrity[16] = L"null";
	char bytes[24576];
	unsigned long long offset = 0;
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	SYSTEMTIME now;
	long level = integrity_level();
	if (level >= 0) swprintf(integrity, 16, L"%ld", level);
	if (log && GetFileAttributesExW(log, GetFileExInfoStandard, &attributes))
		offset = ((unsigned long long)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
	GetSystemTime(&now);
	text[0] = 0;
	wcscat_s(text, 8192, L"{\n  \"ok\": true,\n  \"mode\": \"vulkan-proxy\",\n  \"root\": ");
	json_string(text, 8192, root ? root : L"");
	wcscat_s(text, 8192, L",\n  \"game_exe\": ");
	json_string(text, 8192, game);
	swprintf(text + wcslen(text), 8192 - wcslen(text),
		 L",\n  \"pid\": %lu,\n  \"integrity\": %ls,\n  \"daemon_start_bytes\": %llu,\n"
		 L"  \"started_utc\": \"%04u-%02u-%02uT%02u:%02u:%02u.%03u+00:00\",\n"
		 L"  \"exit_code\": null\n}\n",
		 GetCurrentProcessId(), integrity, offset, now.wYear, now.wMonth, now.wDay, now.wHour,
		 now.wMinute, now.wSecond, now.wMilliseconds);
	int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, sizeof bytes, NULL, NULL);
	if (length <= 1) return;
	swprintf(temporary, MAX_PATH * 2 + 16, L"%ls.proxy", path);
	HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
				  FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return;
	DWORD written = 0;
	BOOL ok = WriteFile(file, bytes, (DWORD)(length - 1), &written, NULL);
	CloseHandle(file);
	if (ok) MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING);
	else DeleteFileW(temporary);
}

/* The folder this process was started in, added to dlss-nr\start-folders.txt (UTF-8, a line a
 * folder) unless it is there already. */
static void record_start_folder(const wchar_t *folder)
{
	static char bytes[16384];
	static wchar_t text[16384];
	wchar_t current[MAX_PATH * 2], path[MAX_PATH * 2 + 40];
	DWORD length = GetCurrentDirectoryW(MAX_PATH * 2, current);
	if (!length || length >= MAX_PATH * 2) return;
	swprintf(path, MAX_PATH * 2 + 40, L"%ls\\dlss-nr\\start-folders.txt", folder);
	/* Without FILE_WRITE_DATA every write lands at the end. */
	HANDLE file = CreateFileW(path, GENERIC_READ | FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
				  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return;
	DWORD read = 0;
	int known = !ReadFile(file, bytes, sizeof bytes - 1, &read, NULL) || read == sizeof bytes - 1;
	int count = read ? MultiByteToWideChar(CP_UTF8, 0, bytes, (int)read, text, 16383) : 0;
	text[count > 0 ? count : 0] = 0;
	for (wchar_t *line = text, *next; !known && line && *line; line = next) {
		next = wcschr(line, L'\n');
		if (next) *next++ = 0;
		known = CompareStringOrdinal(line, -1, current, -1, TRUE) == CSTR_EQUAL;
	}
	char line[MAX_PATH * 6 + 1];
	int size = known ? 0 : WideCharToMultiByte(CP_UTF8, 0, current, -1, line, (int)sizeof line - 1,
						   NULL, NULL);
	if (size > 1) {
		DWORD written;
		line[size - 1] = '\n';
		WriteFile(file, line, (DWORD)size, &written, NULL);
	}
	CloseHandle(file);
}

/* A launch trace, beside the game as dlss-nr\proxy-log.txt.
 *
 * The proxy is the one component that runs before anything else and the one whose failure is
 * invisible: if the game is started in a way that never loads this DLL, or loads it into a process
 * that is not the game, nothing anywhere says so - the game simply runs without NR. A launch from
 * Steam and a launch from a script are different enough that guessing which branch was taken is
 * not a method; this records the branch. Appended, so a run of launches can be compared. */
static void trace_launch(const wchar_t *folder, const wchar_t *what)
{
	wchar_t path[MAX_PATH * 2 + 40];
	swprintf(path, MAX_PATH * 2 + 40, L"%ls\\dlss-nr\\proxy-log.txt", folder);
	HANDLE file = CreateFileW(path, GENERIC_WRITE | FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
				  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return;
	char line[16384];
	wchar_t module[MAX_PATH * 2] = L"?";
	GetModuleFileNameW(NULL, module, MAX_PATH * 2);
	wchar_t cwd[MAX_PATH * 2] = L"?";
	GetCurrentDirectoryW(MAX_PATH * 2, cwd);
	wchar_t wide[16384];
	_snwprintf_s(wide, 16384, _TRUNCATE,
		     L"pid=%lu module=%ls cwd=%ls %ls\r\n",
		     GetCurrentProcessId(), module, cwd, what);
	/* UTF-8, and one WriteFile per line.
	 *
	 * The first version wrote `_snprintf_s` text straight out, which is bytes for a wide string and
	 * came out as interleaved fragments - it read as file corruption rather than as a format bug.
	 * A single write also keeps two processes from interleaving inside one line. */
	int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, line, (int)sizeof line, NULL, NULL);
	if (n > 1) {
		DWORD written = 0;
		WriteFile(file, line, (DWORD)(n - 1), &written, NULL);
		FlushFileBuffers(file);
	}
	CloseHandle(file);

	/* The same line on stderr, which the caller can redirect and read without racing the file.
	 * A game launched by Steam has no visible stderr, but a game launched by a script does - and
	 * the comparison between the two launches is the whole point of this trace. */
	{
		static char narrow[16384];
		int m = WideCharToMultiByte(CP_UTF8, 0, wide, -1, narrow, (int)sizeof narrow, NULL, NULL);
		if (m > 1) fwrite(narrow, 1, (size_t)(m - 1), stderr);
	}
}

static void apply_environment(HINSTANCE self)
{
	wchar_t folder[MAX_PATH * 2], file[MAX_PATH * 2 + 32], game[MAX_PATH * 2];
	DWORD length = GetModuleFileNameW(self, folder, MAX_PATH * 2);
	if (!length || length >= MAX_PATH * 2) return;
	wchar_t *slash = wcsrchr(folder, L'\\');
	if (!slash) return;
	*slash = 0;
	swprintf(file, MAX_PATH * 2 + 32, L"%ls\\dlss-nr\\nr-env.txt", folder);
	trace_launch(folder, L"proxy loaded");

	HANDLE handle = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
				    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) { trace_launch(folder, L"no nr-env.txt"); return; }
	static char bytes[65536];
	static wchar_t text[65536];
	DWORD read = 0;
	BOOL ok = ReadFile(handle, bytes, sizeof bytes - 1, &read, NULL);
	CloseHandle(handle);
	if (!ok || !read) return;
	int count = MultiByteToWideChar(CP_UTF8, 0, bytes, (int)read, text, 65535);
	if (count <= 0) return;
	text[count] = 0;

	/* Every line first, so nothing is set unless this is the game the file names. */
	wchar_t *names[128], *values[128];
	int lines = 0;
	for (wchar_t *line = text, *next; line && *line && lines < 128; line = next) {
		next = wcschr(line, L'\n');
		if (next) *next++ = 0;
		size_t end = wcslen(line);
		while (end && (line[end - 1] == L'\r' || line[end - 1] == L' ')) line[--end] = 0;
		if (line[0] == 0xFEFF) line++;
		wchar_t *equals = wcschr(line, L'=');
		if (!equals || equals == line || line[0] == L'#') continue;
		*equals = 0;
		names[lines] = line;
		values[lines++] = equals + 1;
	}
	const wchar_t *wanted = lookup(names, values, lines, L"NR_GAME_EXE");
	length = GetModuleFileNameW(NULL, game, MAX_PATH * 2);
	if (!wanted || !length || length >= MAX_PATH * 2) {
		trace_launch(folder, L"NR_GAME_EXE missing, or no module path");
		return;
	}
	if (!same_file(game, wanted)) {
		/* Another executable in the game's folder - a crash reporter, a launcher. */
		trace_launch(folder, L"not the game process; left alone");
		return;
	}
	trace_launch(folder, L"matched: setting the environment");

	const wchar_t *loader = lookup(names, values, lines, L"NR_REAL_VULKAN");
	if (loader && *loader) lstrcpynW(real_path, loader, MAX_PATH * 2);
	/* The game is never a daemon NR started: NR_LAYER_SPAWNED marks one, and inherited from
	 * wherever, it would keep the layer from starting the game's daemon. */
	SetEnvironmentVariableW(L"NR_LAYER_SPAWNED", NULL);
	for (int i = 0; i < lines; i++) {
		if (CompareStringOrdinal(names[i], -1, L"NR_GAME_EXE", -1, TRUE) == CSTR_EQUAL
		    || CompareStringOrdinal(names[i], -1, L"NR_REAL_VULKAN", -1, TRUE) == CSTR_EQUAL
		    || CompareStringOrdinal(names[i], -1, L"NR_LAUNCH_STATE", -1, TRUE) == CSTR_EQUAL)
			continue;
		SetEnvironmentVariableW(names[i], values[i]);
	}
	record_start_folder(folder);
	/* What was actually set, and what the process ended up with. A Steam launch arrives with a
	 * great deal of its own environment - the overlay's, the compatibility tool's - and a variable
	 * that the proxy sets and something later overrides looks identical, from the game's side, to
	 * one that was never set. */
	{
		/* One line per variable, each written on its own. The buffer is the caller's largest
		 * possible value plus room for the label - a bound smaller than the string is how the
		 * first version of this wrote a truncated, interleaved log that read as corruption. */
		static const wchar_t *names_to_log[] = {
			L"VK_LAYER_PATH", L"VK_INSTANCE_LAYERS", L"ENABLE_NR_LAYER", L"NR_ROOT",
			L"NR_LAYER_SPAWN", L"NR_LAYER_LIVE", L"NR_SETTINGS",
		};
		wchar_t value[MAX_PATH * 4 + 64];
		wchar_t msg[MAX_PATH * 4 + 128];
		for (int i = 0; i < (int)(sizeof names_to_log / sizeof names_to_log[0]); ++i) {
			DWORD got = GetEnvironmentVariableW(names_to_log[i], value,
							    (DWORD)(sizeof value / sizeof value[0]));
			_snwprintf_s(msg, sizeof msg / sizeof msg[0], _TRUNCATE, L"  %ls=%ls",
				     names_to_log[i], got ? value : L"<unset>");
			trace_launch(folder, msg);
		}
	}
	/* The game as setup's profile spells it, so its status matches the record. */
	const wchar_t *state = lookup(names, values, lines, L"NR_LAUNCH_STATE");
	if (state && *state)
		record_launch(state, lookup(names, values, lines, L"NR_ROOT"), wanted,
			      lookup(names, values, lines, L"NR_LAYER_LOG"));
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		wchar_t off[8];
		DisableThreadLibraryCalls(self);
		DWORD set = GetEnvironmentVariableW(L"DISABLE_NR_PROXY", off, 8);
		if (!(set && set < 8 && off[0] == L'1')) apply_environment(self);
	}
	return TRUE;
}
