/*
 * Standalone behavioural tests for the capture arbiter.
 *
 * Plain C, hand-rolled, no framework — matches the project's dependency-light
 * style.  Exercises only the arbiter's external behaviour: (requested mode,
 * per-source availability) → (effective mode, active inputs, notifications,
 * retry-pending).  Never touches PipeWire, libav, the encoder, or a device.
 *
 * Built and run by `make test`.  Exits 0 iff every check passes.
 */
#include <stdio.h>
#include "../src/arbiter.h"

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg) do {                                            \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_failures++;                                                \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __func__, __LINE__, msg); \
        }                                                               \
    } while (0)

static CaptureAvail avail(Availability cam, Availability mic, Availability desk)
{
    CaptureAvail a = { cam, mic, desk };
    return a;
}

/* All-available verdicts — the ordinary "no meeting running" world. */
static CaptureAvail all_ok(void)
{
    return avail(CAP_AVAILABLE, CAP_AVAILABLE, CAP_AVAILABLE);
}

/* ── display never touches the webcam or the camera node (US1, US2) ── */

static void test_display_never_holds_camera(void)
{
    ArbiterState s;
    ArbiterPlan p;

    /* Even with the camera fully available, display declines it. */
    arbiter_init(&s);
    p = arbiter_step(&s, MODE_DISPLAY, all_ok());
    CHECK(p.effective == MODE_DISPLAY, "display stays display");
    CHECK(p.active.webcam == 0, "display: webcam inactive");
    CHECK(p.webcam_retry_pending == 0, "display: no retry pending");
    CHECK(p.notes == 0, "display: no webcam notification");

    /* And with the camera busy, identical result. */
    arbiter_init(&s);
    p = arbiter_step(&s, MODE_DISPLAY,
                     avail(CAP_UNAVAILABLE, CAP_AVAILABLE, CAP_AVAILABLE));
    CHECK(p.effective == MODE_DISPLAY, "display stays display (cam busy)");
    CHECK(p.active.webcam == 0, "display: webcam inactive (cam busy)");
    CHECK(p.webcam_retry_pending == 0, "display: no retry pending (cam busy)");
    CHECK(p.notes == 0, "display: no notification (cam busy)");
}

/* ── webcam / both with the camera free engage immediately (US11) ── */

static void test_webcam_available_engages(void)
{
    ArbiterState s;
    ArbiterPlan p;

    arbiter_init(&s);
    p = arbiter_step(&s, MODE_WEBCAM, all_ok());
    CHECK(p.effective == MODE_WEBCAM, "webcam available: effective webcam");
    CHECK(p.active.webcam == 1, "webcam available: webcam active");
    CHECK(p.webcam_retry_pending == 0, "webcam available: no retry");
    CHECK(p.notes == 0, "webcam available: no notification (common case)");

    arbiter_init(&s);
    p = arbiter_step(&s, MODE_BOTH, all_ok());
    CHECK(p.effective == MODE_BOTH, "both available: effective both");
    CHECK(p.active.webcam == 1, "both available: webcam active");
    CHECK(p.notes == 0, "both available: no notification (common case)");
}

/* ── webcam / both with the camera busy decline gracefully (US4,5,6,7,12) ── */

static void test_webcam_unavailable_declines(void)
{
    ArbiterState s;
    ArbiterPlan p;

    arbiter_init(&s);
    p = arbiter_step(&s, MODE_BOTH,
                     avail(CAP_UNAVAILABLE, CAP_AVAILABLE, CAP_AVAILABLE));
    CHECK(p.effective == MODE_DISPLAY, "both, cam busy: falls back to display");
    CHECK(p.active.webcam == 0, "both, cam busy: webcam inactive");
    CHECK(p.webcam_retry_pending == 1, "both, cam busy: retry pending");
    CHECK(p.notes & ARB_NOTE_WEBCAM_UNAVAILABLE,
          "both, cam busy: unavailable notification emitted");

    /* webcam-only declines to display too (US12) rather than a black frame. */
    arbiter_init(&s);
    p = arbiter_step(&s, MODE_WEBCAM,
                     avail(CAP_UNAVAILABLE, CAP_AVAILABLE, CAP_AVAILABLE));
    CHECK(p.effective == MODE_DISPLAY, "webcam-only, cam busy: display");
    CHECK(p.active.webcam == 0, "webcam-only, cam busy: webcam inactive");
    CHECK(p.webcam_retry_pending == 1, "webcam-only, cam busy: retry pending");
    CHECK(p.notes & ARB_NOTE_WEBCAM_UNAVAILABLE,
          "webcam-only, cam busy: unavailable notification");
}

/* ── the unavailable notification fires once, not every tick (US6) ── */

static void test_unavailable_notified_once(void)
{
    ArbiterState s;
    ArbiterPlan p;
    CaptureAvail busy = avail(CAP_UNAVAILABLE, CAP_AVAILABLE, CAP_AVAILABLE);

    arbiter_init(&s);
    p = arbiter_step(&s, MODE_BOTH, busy);
    CHECK(p.notes & ARB_NOTE_WEBCAM_UNAVAILABLE, "first decline notifies");

    p = arbiter_step(&s, MODE_BOTH, busy);
    CHECK((p.notes & ARB_NOTE_WEBCAM_UNAVAILABLE) == 0,
          "still-busy on next tick does not re-notify");
    CHECK(p.webcam_retry_pending == 1, "still pending while busy");
    CHECK(p.effective == MODE_DISPLAY, "still recording display while busy");
}

/* ── auto-recovery: unavailable → available while desire persists (US8,9) ── */

static void test_auto_recovery_engages(void)
{
    ArbiterState s;
    ArbiterPlan p;

    arbiter_init(&s);
    arbiter_step(&s, MODE_BOTH,
                 avail(CAP_UNAVAILABLE, CAP_AVAILABLE, CAP_AVAILABLE));
    /* Camera frees while the request still stands. */
    p = arbiter_step(&s, MODE_BOTH, all_ok());
    CHECK(p.effective == MODE_BOTH, "recovery: effective returns to both");
    CHECK(p.active.webcam == 1, "recovery: webcam engages");
    CHECK(p.webcam_retry_pending == 0, "recovery: retry cleared");
    CHECK(p.notes & ARB_NOTE_WEBCAM_ENGAGED, "recovery: engaged notification");
    CHECK((p.notes & ARB_NOTE_WEBCAM_UNAVAILABLE) == 0,
          "recovery: no stale unavailable note");
}

/* ── leaving webcam/both for display releases the camera node (US10) ── */

static void test_switch_to_display_releases(void)
{
    ArbiterState s;
    ArbiterPlan p;

    arbiter_init(&s);
    p = arbiter_step(&s, MODE_BOTH, all_ok());
    CHECK(p.active.webcam == 1, "both: webcam held");

    p = arbiter_step(&s, MODE_DISPLAY, all_ok());
    CHECK(p.active.webcam == 0, "switch to display: webcam released");
    CHECK(p.effective == MODE_DISPLAY, "switch to display: effective display");
    CHECK(p.webcam_retry_pending == 0, "switch to display: nothing pending");
    CHECK(p.notes == 0, "switch to display: no webcam notification");
}

/* ── repeated display <-> both toggling never sticks (US22) ── */

static void test_repeated_toggle(void)
{
    ArbiterState s;
    ArbiterPlan p;
    arbiter_init(&s);

    for (int i = 0; i < 5; i++) {
        p = arbiter_step(&s, MODE_BOTH, all_ok());
        CHECK(p.active.webcam == 1, "toggle: both re-acquires webcam");
        CHECK(p.effective == MODE_BOTH, "toggle: both effective");
        CHECK(p.notes == 0, "toggle: clean re-acquire needs no notification");

        p = arbiter_step(&s, MODE_DISPLAY, all_ok());
        CHECK(p.active.webcam == 0, "toggle: display releases webcam");
        CHECK(p.effective == MODE_DISPLAY, "toggle: display effective");
        CHECK(p.notes == 0, "toggle: clean release needs no notification");
    }
}

/* ── camera grabbed away mid-recording while still wanted re-declines ── */

static void test_lost_while_wanted(void)
{
    ArbiterState s;
    ArbiterPlan p;

    arbiter_init(&s);
    p = arbiter_step(&s, MODE_BOTH, all_ok());
    CHECK(p.active.webcam == 1, "both engaged");

    /* Another app grabs the camera while we are recording it. */
    p = arbiter_step(&s, MODE_BOTH,
                     avail(CAP_UNAVAILABLE, CAP_AVAILABLE, CAP_AVAILABLE));
    CHECK(p.active.webcam == 0, "camera lost: webcam inactive");
    CHECK(p.effective == MODE_DISPLAY, "camera lost: fall back to display");
    CHECK(p.webcam_retry_pending == 1, "camera lost: retry pending");
    CHECK(p.notes & ARB_NOTE_WEBCAM_UNAVAILABLE, "camera lost: notify once");
}

/* ── audio availability sets active flags, unaffected by / of the webcam ── */

static void test_audio_availability(void)
{
    ArbiterState s;
    ArbiterPlan p;

    /* Mic present, desktop absent. */
    arbiter_init(&s);
    p = arbiter_step(&s, MODE_DISPLAY,
                     avail(CAP_AVAILABLE, CAP_AVAILABLE, CAP_UNAVAILABLE));
    CHECK(p.active.mic == 1, "mic available: mic active");
    CHECK(p.active.desktop == 0, "desktop absent: desktop inactive");
    CHECK(p.notes == 0, "audio changes emit no webcam notification");

    /* Mic absent, desktop present — the recording is otherwise unaffected. */
    arbiter_init(&s);
    p = arbiter_step(&s, MODE_BOTH,
                     avail(CAP_AVAILABLE, CAP_UNAVAILABLE, CAP_AVAILABLE));
    CHECK(p.active.mic == 0, "mic absent: mic inactive");
    CHECK(p.active.desktop == 1, "desktop available: desktop active");
    CHECK(p.active.webcam == 1, "audio loss does not disturb the webcam");
    CHECK(p.effective == MODE_BOTH, "audio loss does not disturb effective mode");
}

/* ── arbiter_wants_webcam helper ── */

static void test_wants_webcam(void)
{
    CHECK(arbiter_wants_webcam(MODE_DISPLAY) == 0, "display does not want webcam");
    CHECK(arbiter_wants_webcam(MODE_WEBCAM) == 1, "webcam wants webcam");
    CHECK(arbiter_wants_webcam(MODE_BOTH) == 1, "both wants webcam");
    CHECK(arbiter_wants_webcam(MODE_IDLE) == 0, "idle does not want webcam");
}

int main(void)
{
    test_display_never_holds_camera();
    test_webcam_available_engages();
    test_webcam_unavailable_declines();
    test_unavailable_notified_once();
    test_auto_recovery_engages();
    test_switch_to_display_releases();
    test_repeated_toggle();
    test_lost_while_wanted();
    test_audio_availability();
    test_wants_webcam();

    if (g_failures == 0) {
        printf("arbiter: all %d checks passed\n", g_checks);
        return 0;
    }
    fprintf(stderr, "arbiter: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
