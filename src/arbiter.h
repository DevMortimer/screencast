#pragma once
#include "control.h"   /* RecordMode */

/*
 * The capture arbiter — the cooperative-degradation decision unit.
 *
 * Pure: no PipeWire, no libav, no I/O, no devices.  Given the requested
 * RecordMode and an availability verdict for each optional input (webcam,
 * microphone audio, desktop audio), it returns the *effective plan* — the mode
 * that can actually be recorded, which inputs feed the recording, any
 * state-change notification(s) to emit, and whether a declined webcam request
 * should stay pending for a later retry.
 *
 * All cooperative-decline and auto-recover behaviour lives here as a small
 * state transition over (requested mode × availability), so it can be tested
 * without a camera or a running PipeWire.  The real opens sit behind a thin
 * acquisition wrapper (in the daemon) that reports only available/unavailable;
 * the arbiter never calls them.
 */

/*
 * Availability of an optional input.  "Busy" and "absent" both collapse to
 * UNAVAILABLE: with try-open detection they are indistinguishable and resolve
 * to the same behaviour (decline + retry), so the arbiter does not model them
 * apart.
 */
typedef enum {
    CAP_UNAVAILABLE = 0,
    CAP_AVAILABLE   = 1,
} Availability;

/* Per-source availability verdicts fed into one arbiter step. */
typedef struct {
    Availability webcam;
    Availability mic;
    Availability desktop;
} CaptureAvail;

/* Which optional inputs the plan marks active (feeding the recording). */
typedef struct {
    int webcam;
    int mic;
    int desktop;
} ActiveInputs;

/*
 * State-change notifications the arbiter asks the caller to emit, as a bitmask.
 * The caller maps these onto the existing control_notify() path — the arbiter
 * itself stays free of the notification I/O.
 */
typedef enum {
    ARB_NOTE_WEBCAM_UNAVAILABLE = 1 << 0, /* declined; recording the display instead */
    ARB_NOTE_WEBCAM_ENGAGED     = 1 << 1, /* webcam engaged after being unavailable */
} ArbiterNote;

/*
 * Retained arbiter state: the last plan, so notifications fire only on real
 * transitions and a declined webcam request is remembered as pending.
 */
typedef struct {
    RecordMode   requested;            /* last requested mode */
    RecordMode   effective;            /* last effective mode */
    ActiveInputs active;               /* last active inputs */
    int          webcam_retry_pending; /* want webcam, but it was unavailable */
    int          initialized;          /* a step has run since arbiter_init */
} ArbiterState;

/* Result of one arbiter step (also folded back into the ArbiterState). */
typedef struct {
    RecordMode   effective;
    ActiveInputs active;
    int          webcam_retry_pending;
    unsigned     notes;                /* bitmask of ArbiterNote */
} ArbiterPlan;

/* Reset to a clean pre-recording state: no inputs active, nothing pending. */
void arbiter_init(ArbiterState *s);

/*
 * Advance the arbiter one step: given the requested mode and the latest
 * availability verdicts, compute the effective plan, fold it into *s, and
 * return it (including any transition notifications to emit).
 */
ArbiterPlan arbiter_step(ArbiterState *s, RecordMode requested,
                         CaptureAvail avail);

/* 1 if the requested mode wants the webcam (and thus the camera node). */
int arbiter_wants_webcam(RecordMode requested);
