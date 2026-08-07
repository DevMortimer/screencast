#include "audsrc.h"

#include <libavutil/channel_layout.h>
#include <stdio.h>
#include <time.h>

void audsrc_run(const AudSrcWorker *w)
{
    struct timespec error_ts = { .tv_nsec = w->error_backoff_ns };
    struct timespec empty_ts = { .tv_nsec = w->empty_poll_ns };
    int fails = 0;

    while (w->running(w->user)) {
        AudSrcRead r = w->read(w->user);

        switch (r.result) {
        case AUDSRC_STOP:
            return;

        case AUDSRC_ERROR:
            if (++fails >= w->max_fails) {
                fprintf(stderr, "audsrc: %s audio source died — dropping it "
                                "from the mix\n", w->label);
                mixer_drop_source(w->mixer, w->src);
                return;
            }
            nanosleep(&error_ts, NULL);  /* bounded backoff, never busy-spin */
            continue;

        case AUDSRC_EMPTY:
            nanosleep(&empty_ts, NULL);
            continue;

        case AUDSRC_FRAME: {
            fails = 0;
            /* The mixer reads only the channel count from the layout (it
               stamps the frame with a native layout of that size itself). */
            AVChannelLayout layout;
            av_channel_layout_default(&layout,
                                      r.in_channels > 0 ? r.in_channels : 2);
            mixer_feed(w->mixer, w->src, r.frame, r.in_sample_rate,
                       &layout, r.in_fmt, r.pts_us);
            av_channel_layout_uninit(&layout);
            if (r.owned) av_frame_free(&r.frame);
            continue;
        }

        default:
            /* An adapter result we do not know: end the loop rather than
               spin on a value nothing will ever consume. */
            return;
        }
    }
}
