#define _POSIX_C_SOURCE 200809L
#include "hotkeys.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

atomic_int g_mode      = MODE_IDLE;
atomic_int g_recording = 0;
atomic_int g_running   = 1;

#define MAX_KBD 16

static int s_fds[MAX_KBD];
static int s_nfds = 0;

/* modifier state tracked across events */
static int s_meta  = 0;
static int s_shift = 0;
static int s_grabbed = 0;
static Display *s_dpy = NULL;

static int bit_set(const unsigned long *bits, int bit)
{
    return !!(bits[bit / (8 * (int)sizeof(long))] &
              (1UL << (bit % (8 * (int)sizeof(long)))));
}

static int is_keyboard(int fd)
{
    unsigned long evbits[(EV_MAX  + 8 * (int)sizeof(long)) / (8 * (int)sizeof(long))] = {0};
    unsigned long keybits[(KEY_MAX + 8 * (int)sizeof(long)) / (8 * (int)sizeof(long))] = {0};
    ioctl(fd, EVIOCGBIT(0,      sizeof(evbits)),  evbits);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
    return bit_set(evbits, EV_KEY) && bit_set(keybits, KEY_D);
}

static void set_device_grab(int grab)
{
    if (s_grabbed == grab) return;

    for (int i = 0; i < s_nfds; i++)
        ioctl(s_fds[i], EVIOCGRAB, grab ? 1 : 0);
    s_grabbed = grab;
}

static void fake_key_release(KeySym sym)
{
    if (!s_dpy) return;

    KeyCode key = XKeysymToKeycode(s_dpy, sym);
    if (!key) return;

    XTestFakeKeyEvent(s_dpy, key, False, CurrentTime);
    XFlush(s_dpy);
}

static void fake_modifier_release(int code)
{
    switch (code) {
    case KEY_LEFTMETA:   fake_key_release(XK_Super_L); return;
    case KEY_RIGHTMETA:  fake_key_release(XK_Super_R); return;
    case KEY_LEFTSHIFT:  fake_key_release(XK_Shift_L); return;
    case KEY_RIGHTSHIFT: fake_key_release(XK_Shift_R); return;
    }
}

static void set_mode(RecordMode new_mode)
{
    int cur_mode = atomic_load(&g_mode);
    if (atomic_load(&g_recording) && cur_mode == (int)new_mode)
        return;

    atomic_store(&g_mode,      (int)new_mode);
    atomic_store(&g_recording, 1);
}

int hotkeys_init(void)
{
    s_dpy = XOpenDisplay(NULL);

    DIR *dir = opendir("/dev/input");
    if (!dir) { perror("hotkeys: opendir /dev/input"); return -1; }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_nfds < MAX_KBD) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[280];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (is_keyboard(fd))
            s_fds[s_nfds++] = fd;
        else
            close(fd);
    }
    closedir(dir);

    if (s_nfds == 0) {
        fprintf(stderr, "hotkeys: no keyboard found under /dev/input "
                "(are you in the 'input' group?)\n");
        return -1;
    }

    printf("[INFO] Hotkeys registered (evdev, %d keyboard%s):\n",
           s_nfds, s_nfds > 1 ? "s" : "");
    printf("  Win+Shift+D  ->  record display + mic\n");
    printf("  Win+Shift+W  ->  record webcam + mic\n");
    printf("  Win+Shift+B  ->  record both + mic\n");
    printf("  Win+Esc      ->  stop and exit\n\n");
    return 0;
}

static void handle_ev(int code, int value)
{
    /* value: 0=release, 1=press, 2=repeat */
    switch (code) {
    case KEY_LEFTMETA:  case KEY_RIGHTMETA:
    {
        int was_grabbed = s_grabbed;
        s_meta = (value != 0);
        if (s_meta && s_shift)
            set_device_grab(1);
        else {
            set_device_grab(0);
            if (was_grabbed && value == 0)
                fake_modifier_release(code);
        }
        return;
    }
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
    {
        int was_grabbed = s_grabbed;
        s_shift = (value != 0);
        if (s_meta && s_shift)
            set_device_grab(1);
        else {
            set_device_grab(0);
            if (was_grabbed && value == 0)
                fake_modifier_release(code);
        }
        return;
    }
    }

    if (value != 1 || !s_meta) return;

    if (code == KEY_ESC) {
        atomic_store(&g_recording, 0);
        atomic_store(&g_running,   0);
        set_device_grab(0);
        return;
    }

    if (!s_shift) return;

    RecordMode new_mode;
    if      (code == KEY_D) new_mode = MODE_DISPLAY;
    else if (code == KEY_W) new_mode = MODE_WEBCAM;
    else if (code == KEY_B) new_mode = MODE_BOTH;
    else {
        set_device_grab(0);
        return;
    }

    set_mode(new_mode);
    set_device_grab(0);
}

void hotkeys_run(void)
{
    struct pollfd pfds[MAX_KBD];
    for (int i = 0; i < s_nfds; i++) {
        pfds[i].fd     = s_fds[i];
        pfds[i].events = POLLIN;
    }

    while (atomic_load(&g_running)) {
        int n = poll(pfds, (nfds_t)s_nfds, 20);
        if (n <= 0) continue;

        for (int i = 0; i < s_nfds; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            struct input_event ev;
            while (read(s_fds[i], &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
                if (ev.type == EV_KEY)
                    handle_ev(ev.code, ev.value);
        }
    }
}

void hotkeys_cleanup(void)
{
    set_device_grab(0);
    for (int i = 0; i < s_nfds; i++) {
        close(s_fds[i]);
        s_fds[i] = -1;
    }
    s_nfds = 0;
    if (s_dpy) {
        XCloseDisplay(s_dpy);
        s_dpy = NULL;
    }
}
