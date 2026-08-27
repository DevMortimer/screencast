#import <AppKit/AppKit.h>
#import "sck_capture.h"
#include <stdio.h>
#include <unistd.h>

/* Camera-free regression test for the main-thread AppKit event boundary. */
@interface PumpProbeApplication : NSApplication {
    BOOL _receivedEvent;
}
@property (nonatomic, readonly) BOOL receivedEvent;
@end

@implementation PumpProbeApplication
@synthesize receivedEvent = _receivedEvent;

- (void)sendEvent:(NSEvent *)event
{
    if (event.type == NSEventTypeApplicationDefined)
        _receivedEvent = YES;
    [super sendEvent:event];
}
@end

int main(void)
{
    @autoreleasepool {
        /* Install the probe class before sck_bootstrap_app obtains NSApp. */
        PumpProbeApplication *app = [PumpProbeApplication sharedApplication];
        sck_bootstrap_app();

        /* Application-defined events use the same queued NSEvent boundary as
           mouseDown/mouseDragged, without requiring camera or screen input. */
        NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                            location:NSZeroPoint
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:0
                                               data1:0
                                               data2:0];
        [app postEvent:event atStart:YES];
        sck_pump_run_loop();

        if (!app.receivedEvent) {
            fprintf(stderr, "appkit event pump did not dispatch posted event\n");
            fflush(stderr);
            _exit(1);
        }
        puts("appkit event pump dispatched posted event");
        fflush(stdout);
        _exit(0);
    }
}
