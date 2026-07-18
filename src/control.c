#define _POSIX_C_SOURCE 200809L
#include "control.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

atomic_int g_mode      = MODE_IDLE;
atomic_int g_recording = 0;
atomic_int g_running   = 1;

static int       s_listen_fd = -1;
static pthread_t s_thread;
static int       s_thread_started = 0;
static char      s_sock_path[256];

/* ── socket path ───────────────────────────────────────────── */

static const char *sock_path(void)
{
    if (s_sock_path[0]) return s_sock_path;

    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir && dir[0])
        snprintf(s_sock_path, sizeof(s_sock_path), "%s/screencast.sock", dir);
    else
        snprintf(s_sock_path, sizeof(s_sock_path), "/tmp/screencast-%u.sock",
                 (unsigned)getuid());
    return s_sock_path;
}

/* ── notifications ─────────────────────────────────────────── */

void control_notify(const char *summary, const char *body)
{
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        execlp("notify-send", "notify-send",
               "-a", "screencast", "-t", "3000",
               summary, body ? body : "", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

/* ── mode helpers ──────────────────────────────────────────── */

const char *control_mode_label(RecordMode mode)
{
    switch (mode) {
    case MODE_DISPLAY: return "Display + mic";
    case MODE_WEBCAM:  return "Webcam + mic";
    case MODE_BOTH:    return "Display + webcam + mic";
    default:           return NULL;
    }
}

int control_parse_mode(const char *cmd)
{
    if (!cmd) return -1;
    if (!strcmp(cmd, "display") || !strcmp(cmd, "d")) return MODE_DISPLAY;
    if (!strcmp(cmd, "webcam")  || !strcmp(cmd, "w")) return MODE_WEBCAM;
    if (!strcmp(cmd, "both")    || !strcmp(cmd, "b")) return MODE_BOTH;
    return -1;
}

int control_is_stop(const char *cmd)
{
    return cmd && (!strcmp(cmd, "stop") || !strcmp(cmd, "quit") ||
                   !strcmp(cmd, "exit"));
}

static void set_mode(RecordMode new_mode)
{
    int cur = atomic_load(&g_mode);
    if (cur == (int)new_mode) {
        atomic_store(&g_recording, 1);
        return;
    }
    atomic_store(&g_mode, (int)new_mode);
    atomic_store(&g_recording, 1);
    const char *label = control_mode_label(new_mode);
    if (label) control_notify("Screencast mode changed", label);
}

static void dispatch_command(const char *cmd)
{
    if (control_is_stop(cmd)) {
        atomic_store(&g_recording, 0);
        atomic_store(&g_running, 0);
        return;
    }
    int mode = control_parse_mode(cmd);
    if (mode >= 0) set_mode((RecordMode)mode);
}

/* ── client ────────────────────────────────────────────────── */

int control_client_send(const char *cmd)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path());

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1; /* no daemon listening (or stale socket) */
    }

    size_t len = strlen(cmd);
    ssize_t wr = write(fd, cmd, len);
    close(fd);
    return (wr == (ssize_t)len) ? 0 : -1;
}

/* ── server ────────────────────────────────────────────────── */

static void *listen_thread(void *arg)
{
    (void)arg;
    struct pollfd pfd = { .fd = s_listen_fd, .events = POLLIN };

    while (atomic_load(&g_running)) {
        int n = poll(&pfd, 1, 200);
        if (n <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        int fd = accept(s_listen_fd, NULL, NULL);
        if (fd < 0) continue;

        char buf[64];
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (r <= 0) continue;

        buf[r] = '\0';
        /* trim trailing whitespace/newline */
        while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ' ||
                         buf[r - 1] == '\r' || buf[r - 1] == '\t'))
            buf[--r] = '\0';

        dispatch_command(buf);
    }
    return NULL;
}

int control_server_start(void)
{
    const char *path = sock_path();
    unlink(path); /* clear any stale socket */

    s_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s_listen_fd < 0) { perror("control: socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("control: bind");
        close(s_listen_fd);
        s_listen_fd = -1;
        return -1;
    }
    if (listen(s_listen_fd, 4) < 0) {
        perror("control: listen");
        close(s_listen_fd);
        unlink(path);
        s_listen_fd = -1;
        return -1;
    }

    if (pthread_create(&s_thread, NULL, listen_thread, NULL) != 0) {
        fprintf(stderr, "control: cannot start listener thread\n");
        close(s_listen_fd);
        unlink(path);
        s_listen_fd = -1;
        return -1;
    }
    s_thread_started = 1;

    printf("[INFO] Control socket: %s\n", path);
    printf("  screencast display | webcam | both  ->  switch mode\n");
    printf("  screencast stop                      ->  stop and exit\n\n");
    return 0;
}

void control_server_stop(void)
{
    atomic_store(&g_running, 0);
    if (s_thread_started) {
        pthread_join(s_thread, NULL);
        s_thread_started = 0;
    }
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    unlink(sock_path());
}
