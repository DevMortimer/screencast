#define _POSIX_C_SOURCE 200809L
#include "hotkeys.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

atomic_int g_mode      = MODE_IDLE;
atomic_int g_recording = 0;
atomic_int g_running   = 1;

static Display *s_dpy = NULL;
static Window s_root = 0;
static unsigned int s_num_lock_mask = 0;
static int s_grab_errors = 0;

static int grab_error_handler(Display *dpy, XErrorEvent *err)
{
    (void)dpy;
    if (err->error_code == BadAccess)
        s_grab_errors++;
    return 0;
}

static const char *mode_label(RecordMode mode)
{
    switch (mode) {
    case MODE_DISPLAY: return "Display mode";
    case MODE_WEBCAM:  return "Webcam mode";
    case MODE_BOTH:    return "Both mode";
    default:           return NULL;
    }
}

static void send_mode_notification(RecordMode mode)
{
    const char *label = mode_label(mode);
    if (!label) return;

    pid_t pid = fork();
    if (pid < 0) return;

    if (pid == 0) {
        execlp("notify-send", "notify-send",
               "-a", "screencast",
               "-t", "3000",
               "Screencast mode changed",
               label,
               (char *)NULL);
        _exit(127);
    }

    waitpid(pid, NULL, 0);
}

static unsigned int get_num_lock_mask(Display *dpy)
{
    unsigned int mask = 0;
    XModifierKeymap *modmap = XGetModifierMapping(dpy);
    if (!modmap) return 0;

    for (int mod = 0; mod < 8; mod++) {
        for (int i = 0; i < modmap->max_keypermod; i++) {
            KeyCode kc = modmap->modifiermap[mod * modmap->max_keypermod + i];
            if (!kc) continue;

            KeySym sym = XkbKeycodeToKeysym(dpy, kc, 0, 0);
            if (sym == XK_Num_Lock) {
                mask = (unsigned int)(1U << mod);
                break;
            }
        }
        if (mask) break;
    }

    XFreeModifiermap(modmap);
    return mask;
}

static void grab_key(KeySym sym, unsigned int base_mods)
{
    KeyCode code = XKeysymToKeycode(s_dpy, sym);
    if (!code) return;

    unsigned int ignored[] = {
        0,
        LockMask,
        s_num_lock_mask,
        LockMask | s_num_lock_mask,
    };

    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        XGrabKey(s_dpy, (int)code, base_mods | ignored[i],
                 s_root, False, GrabModeAsync, GrabModeAsync);
    }
}

static void ungrab_key(KeySym sym, unsigned int base_mods)
{
    KeyCode code = XKeysymToKeycode(s_dpy, sym);
    if (!code) return;

    unsigned int ignored[] = {
        0,
        LockMask,
        s_num_lock_mask,
        LockMask | s_num_lock_mask,
    };

    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++)
        XUngrabKey(s_dpy, (int)code, base_mods | ignored[i], s_root);
}

static void set_mode(RecordMode new_mode)
{
    int cur_mode = atomic_load(&g_mode);
    if (cur_mode == (int)new_mode) {
        if (!atomic_load(&g_recording))
            atomic_store(&g_recording, 1);
        return;
    }

    atomic_store(&g_mode,      (int)new_mode);
    atomic_store(&g_recording, 1);
    send_mode_notification(new_mode);
}

int hotkeys_init(void)
{
    s_dpy = XOpenDisplay(NULL);
    if (!s_dpy) {
        fprintf(stderr, "hotkeys: cannot open X display\n");
        return -1;
    }

    s_root = DefaultRootWindow(s_dpy);
    s_num_lock_mask = get_num_lock_mask(s_dpy);

    int (*old_handler)(Display *, XErrorEvent *) =
        XSetErrorHandler(grab_error_handler);
    grab_key(XK_D,      Mod4Mask | ShiftMask);
    grab_key(XK_W,      Mod4Mask | ShiftMask);
    grab_key(XK_B,      Mod4Mask | ShiftMask);
    grab_key(XK_Escape, Mod4Mask);
    XSync(s_dpy, False);
    XSetErrorHandler(old_handler);
    if (s_grab_errors > 0) {
        fprintf(stderr,
                "hotkeys: warning: %d key grab%s failed; "
                "another X11 client may already own a recorder shortcut\n",
                s_grab_errors, s_grab_errors == 1 ? "" : "s");
    }

    printf("[INFO] Hotkeys registered (X11 exact grabs):\n");
    printf("  Win+Shift+D  ->  record display + mic\n");
    printf("  Win+Shift+W  ->  record webcam + mic\n");
    printf("  Win+Shift+B  ->  record both + mic\n");
    printf("  Win+Esc      ->  stop and exit\n\n");
    return 0;
}

static void handle_keypress(XKeyEvent *key)
{
    KeySym sym = XkbKeycodeToKeysym(s_dpy, (KeyCode)key->keycode, 0,
                                    (key->state & ShiftMask) ? 1 : 0);
    unsigned int state = key->state & ~(LockMask | s_num_lock_mask);

    if ((state & Mod4Mask) && sym == XK_Escape) {
        atomic_store(&g_recording, 0);
        atomic_store(&g_running,   0);
        return;
    }

    if ((state & (Mod4Mask | ShiftMask)) != (Mod4Mask | ShiftMask))
        return;

    if      (sym == XK_D) set_mode(MODE_DISPLAY);
    else if (sym == XK_W) set_mode(MODE_WEBCAM);
    else if (sym == XK_B) set_mode(MODE_BOTH);
}

void hotkeys_run(void)
{
    int xfd = ConnectionNumber(s_dpy);
    struct pollfd pfd = { .fd = xfd, .events = POLLIN };

    while (atomic_load(&g_running)) {
        int n = poll(&pfd, 1, 20);
        if (n <= 0) continue;

        while (XPending(s_dpy) > 0) {
            XEvent ev;
            XNextEvent(s_dpy, &ev);
            if (ev.type == KeyPress)
                handle_keypress(&ev.xkey);
        }
    }
}

void hotkeys_cleanup(void)
{
    if (!s_dpy) return;

    ungrab_key(XK_D,      Mod4Mask | ShiftMask);
    ungrab_key(XK_W,      Mod4Mask | ShiftMask);
    ungrab_key(XK_B,      Mod4Mask | ShiftMask);
    ungrab_key(XK_Escape, Mod4Mask);
    XSync(s_dpy, False);
    XCloseDisplay(s_dpy);
    s_dpy = NULL;
    s_root = 0;
}
