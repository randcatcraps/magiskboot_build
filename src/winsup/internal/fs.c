#include <assert.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "assert.h"
#include "fd.h"

#define fs_min(a, b)  (((a) < (b)) ? (a) : (b))

#define LOG_TAG             "fs_internal"
#define LOG_ERR(...)        log_err(LOG_TAG, __VA_ARGS__);

static bool enforce_case;

void __ensure_case_sensitive(const char *path, bool file) {
    if (!path)
        return;  // invalid call

    if (file) {
        // find out parent directory and re-run

        char *tmp = strdup(path);

        assert(tmp);

        const char *parent = dirname(tmp);

        __ensure_case_sensitive(parent, false);

        free(tmp);

        return;
    }

    struct stat buf;

    if (stat(path, &buf) < 0 || !S_ISDIR(buf.st_mode) || access(path, W_OK) != 0)
        return;  // path is not a valid directory or inaccessible

    HANDLE h = INVALID_HANDLE_VALUE;
    bool success = false;

    int fd;

    if ((fd = __open_dir_fd(path, GENERIC_READ, FILE_SHARE_VALID_FLAGS, 0)) < 0) {
open_dir_fd_failed:
#ifndef NDEBUG
        LOG_ERR("__open_dir_fd failed: %s", win_strerror(GetLastError()))
#endif

        goto quit;
    }

    if ((h = (HANDLE) _get_osfhandle(fd)) == INVALID_HANDLE_VALUE) {
get_handle_failed:
#ifndef NDEBUG
        LOG_ERR("_get_osfhandle failed")
#endif

        goto quit;
    }

    fd = -1;  // ownership transferred

    FILE_CASE_SENSITIVE_INFO fcsi;

    if (!GetFileInformationByHandleEx(h, FileCaseSensitiveInfo, &fcsi, sizeof(fcsi))) {
#ifndef NDEBUG
        LOG_ERR("GetFileInformationByHandleEx failed: %s", win_strerror(GetLastError()))
#endif

        goto quit;
    }

    if (fcsi.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR)
        goto done;  // already case sensitive, skip

    // reopen with write permission
    CloseHandle(h);
    h = NULL;

    if ((fd = __open_dir_fd(path, GENERIC_WRITE, FILE_SHARE_VALID_FLAGS, 0)) < 0)
        goto open_dir_fd_failed;

    if ((h = (HANDLE) _get_osfhandle(fd)) == INVALID_HANDLE_VALUE)
        goto get_handle_failed;

    fd = -1;

    fcsi.Flags |= FILE_CS_FLAG_CASE_SENSITIVE_DIR;

    if (!SetFileInformationByHandle(h, FileCaseSensitiveInfo, &fcsi, sizeof(fcsi))) {
#ifndef NDEBUG
        LOG_ERR("SetFileInformationByHandle failed: %s", win_strerror(GetLastError()))
#endif

        goto quit;
    }

done:
    success = true;

quit:
    if (!(fd < 0))
        close(fd);

    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);

    if (!success && enforce_case) {
        LOG_ERR("An error occurred while ensuring case sensitivity of the directory '%s'\n"
                    "Now the program will stop in order to avoid any potential incorrect file operations.\n"
                    "Please run 'fsutil.exe file setCaseSensitiveInfo <path> enable' manually for this directory and try again.\n"
                    "To disable this check, set the environment variable MAGISKBOOT_WINSUP_NOCASE to 1 before running this program.", path)

        abort();
    }

    return;
}

__attribute__((constructor)) static void __init_enforce_mode(void) {
    enforce_case = !getenv("MAGISKBOOT_WINSUP_NOCASE");

    // FIXME: some of the Rust stuffs don't go through our open() stubs (but mkdir() stub is used)
    //        so need to ensure the CWD is case sensitive on startup
    //        but not if the user makes magiskboot to create files under another directory
    //        a possible workaround is probably to hook CreateFile() and add the checks
    __ensure_case_sensitive(".", false);
}
