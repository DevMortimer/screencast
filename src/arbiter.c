#include "arbiter.h"
#include <string.h>

void arbiter_init(ArbiterState *s)
{
    memset(s, 0, sizeof(*s));
    s->requested = MODE_IDLE;
    s->effective = MODE_IDLE;
}

int arbiter_wants_webcam(RecordMode requested)
{
    return requested == MODE_WEBCAM || requested == MODE_BOTH;
}

ArbiterPlan arbiter_step(ArbiterState *s, RecordMode requested,
                         CaptureAvail avail)
{
    ArbiterPlan p;
    memset(&p, 0, sizeof(p));

    int want_webcam   = arbiter_wants_webcam(requested);
    int webcam_active = want_webcam && avail.webcam == CAP_AVAILABLE;

    /*
     * Effective mode is driven by real availability.  display always records
     * the display; a webcam/both request whose camera is unavailable falls
     * back to display rather than aborting or emitting a black frame.
     */
    RecordMode effective;
    if (requested == MODE_DISPLAY || requested == MODE_IDLE)
        effective = MODE_DISPLAY;
    else if (webcam_active)
        effective = requested;          /* webcam or both */
    else
        effective = MODE_DISPLAY;        /* cooperative decline */

    p.effective            = effective;
    p.active.webcam        = webcam_active;
    p.active.mic           = avail.mic     == CAP_AVAILABLE;
    p.active.desktop       = avail.desktop == CAP_AVAILABLE;
    p.webcam_retry_pending = want_webcam && !webcam_active;

    /*
     * Notifications fire only on a real webcam-state transition.
     *
     *  - "unavailable": a webcam is wanted but not active, and this is a fresh
     *    decline — either the first step of the recording, or the camera was
     *    active until now.  A request that has been pending across earlier
     *    ticks does not re-notify.
     *  - "engaged": the webcam becomes active after a spell of being wanted but
     *    unavailable (retry was pending).  A clean display<->both toggle, where
     *    nothing was ever declined, engages silently.
     */
    unsigned notes = 0;
    if (want_webcam && !webcam_active) {
        int fresh_decline = !s->initialized ||
                            s->active.webcam ||
                            !s->webcam_retry_pending;
        if (fresh_decline)
            notes |= ARB_NOTE_WEBCAM_UNAVAILABLE;
    } else if (webcam_active && s->initialized &&
               !s->active.webcam && s->webcam_retry_pending) {
        notes |= ARB_NOTE_WEBCAM_ENGAGED;
    }
    p.notes = notes;

    s->requested            = requested;
    s->effective            = effective;
    s->active               = p.active;
    s->webcam_retry_pending = p.webcam_retry_pending;
    s->initialized          = 1;
    return p;
}
