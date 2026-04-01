// protocol.c - wrappers for reading and writing full messages over pipes
// since read() and write() can return partial results, these functions
// loop until the full message is transferred (or an error occurs)

#include "montecarlo.h"

// writes exactly len bytes to fd, returns 0 on success, -1 on error
int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

// reads exactly len bytes from fd
// returns 0 on success, 1 on clean EOF (pipe closed before any bytes),
// -1 on error (including mid-message EOF which sets errno=ECONNRESET)
int read_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t remaining = len;
    int first = 1;

    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            if (first) return 1; // EOF, pipe was closed
            // got some bytes then EOF... bad
            errno = ECONNRESET;
            return -1;
        }
        first = 0;
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}
