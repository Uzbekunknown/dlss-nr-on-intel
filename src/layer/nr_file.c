/* Reading a file whole, stamping it, and replacing it atomically. See nr_file.h. */
#include "nr_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#endif

int nr_read_file(const char *path, char **out, size_t *size)
{
    if (!path || !*path) return 0;

    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long length = ftell(f);
    if (length < 0) { fclose(f); return 0; }
    rewind(f);

    char *buffer = (char *)malloc((size_t)length + 1);
    if (!buffer) { fclose(f); return 0; }
    size_t got = fread(buffer, 1, (size_t)length, f);
    fclose(f);
    buffer[got] = '\0';

    *out = buffer;
    if (size) *size = got;
    return 1;
}

long long nr_file_stamp(const char *path)
{
    if (!path || !*path) return 0;
#ifdef _WIN32
    /* The write time, which changes whenever anybody writes the file - not the access time, which
     * changes when it is read and would make a poll look like an edit. */
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info)) return 0;
    return ((long long)info.ftLastWriteTime.dwHighDateTime << 32)
         | (unsigned)info.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return ((long long)st.st_mtime << 32) ^ (long long)st.st_size;
#endif
}

int nr_write_file_atomic(const char *path, const void *data, size_t size)
{
    if (!path || !*path) return 0;

    char temporary[1088];
    snprintf(temporary, sizeof temporary, "%s.new", path);

    FILE *f = fopen(temporary, "wb");
    if (!f) return 0;
    size_t wrote = fwrite(data, 1, size, f);
    int closed = (fclose(f) == 0);
    if (wrote != size || !closed) {
#ifdef _WIN32
        DeleteFileA(temporary);
#else
        remove(temporary);
#endif
        return 0;
    }

#ifdef _WIN32
    /* MOVEFILE_REPLACE_EXISTING as well as WRITE_THROUGH: without the first, a rename onto a file
     * that exists fails; without the second, the rename can be visible before the bytes are. */
    return MoveFileExA(temporary, path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temporary, path) == 0;
#endif
}
