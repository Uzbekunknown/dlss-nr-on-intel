/* Reading a whole file, and asking whether it changed. Shared by the menu's bridge and anything
 * else that watches the settings file.
 *
 * Two functions with no state, in a header so both callers get the same behaviour: a read that
 * returns the bytes and their length, and a stamp that answers "has anybody written this since I
 * last looked" without opening it. The stamp is what makes an edit in Notepad take effect while
 * the game runs.
 */
#ifndef NR_FILE_H
#define NR_FILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the whole file into a malloc'd buffer, NUL-terminated, and report its length.
 *
 * Returns 1 on success, 0 when the file is not there or cannot be read - a missing settings file
 * is a normal state, not an error. The caller frees `*out` on success. */
int nr_read_file(const char *path, char **out, size_t *size);

/* A stamp that changes when the file changes. Returns 0 when the file cannot be stat'd, which
 * callers treat as "no change" rather than "changed" - so a file that is deleted does not look
 * like an edit. */
long long nr_file_stamp(const char *path);

/* Write `size` bytes to `path` atomically: to a temporary beside it, then rename over it.
 *
 * The settings file is polled by the daemon, and a reader that caught a half-written object would
 * log a parse error and keep its old values - so the rename is not a nicety. Returns 1 on
 * success. */
int nr_write_file_atomic(const char *path, const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* NR_FILE_H */
