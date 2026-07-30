// macOS compatibility shim for Linux `SOCK_CLOEXEC` socket flag.
//
// macOS does not support the `SOCK_CLOEXEC` flag in `socket()` — passing any
// extra bit returns ENOTSUP.  This header provides a socket() wrapper that
// strips the unsupported flag and sets `FD_CLOEXEC` via fcntl after creation,
// matching the Linux semantics.
//
// Force-included on macOS targets via `-include` in CFLAGS (see Makefile).

#ifdef __APPLE__
#ifndef SCREENCAST_COMPAT_MACOS_H
#define SCREENCAST_COMPAT_MACOS_H

#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

/* CFRunLoopRunInMode (used for AppKit event pumping from C code). */
#include <CoreFoundation/CFRunLoop.h>

// Undefine any prior macro so the real socket() is available.
#ifdef socket
#undef socket
#endif

static inline int __screencast_socket(int domain, int type, int protocol) {
    // Strip the SOCK_CLOEXEC bit (0x80000 = O_CLOEXEC on Linux) — unsupported
    // on macOS and would cause ENOTSUP.
    int fd = socket(domain, type & ~0x80000, protocol);
    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return fd;
}

#define socket(domain, type, protocol) __screencast_socket(domain, type, protocol)

// Define SOCK_CLOEXEC to the Linux value so `SOCK_STREAM | SOCK_CLOEXEC`
// expressions compile.  The bit is stripped inside __screencast_socket().
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0x80000
#endif

#endif // SCREENCAST_COMPAT_MACOS_H
#endif // __APPLE__
