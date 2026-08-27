// status_menu.m — native menu-bar controls for the macOS recorder
#import "status_menu.h"
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>

static NSString *const PreferredModeKey = @"ScreencastPreferredMode";
static NSString *const CameraDeviceKey  = @"ScreencastCameraDeviceUID";
static NSString *const MicDeviceKey     = @"ScreencastMicrophoneDeviceUID";
static NSString *const DesktopAudioKey  = @"ScreencastDesktopAudioEnabled";

typedef NS_ENUM(NSInteger, StatusMode) {
    StatusModeDisplay = 1,
    StatusModePresenter = 2,
};

@interface ScreencastStatusController : NSObject {
@public
    NSStatusItem *_statusItem;
    NSMenuItem *_stateItem;
    NSMenuItem *_nextItem;
    NSMenuItem *_displayModeItem;
    NSMenuItem *_presenterModeItem;
    NSMenuItem *_desktopAudioItem;
    BOOL _recording;
    BOOL _presenter;
}
- (void)setRecording:(BOOL)recording presenter:(BOOL)presenter;
- (void)cameraChanged:(id)sender;
- (void)microphoneChanged:(id)sender;
- (void)modeChanged:(id)sender;
- (void)desktopAudioChanged:(id)sender;
@end

static NSString *saved_string(NSString *key)
{
    id value = [[NSUserDefaults standardUserDefaults] objectForKey:key];
    return [value isKindOfClass:[NSString class]] ? value : @"";
}

static BOOL selected_device(NSString *uid, AVCaptureDevice *device)
{
    return uid.length > 0 && [uid isEqualToString:device.uniqueID];
}

@implementation ScreencastStatusController

- (void)dealloc
{
    [_statusItem release];
    [super dealloc];
}

- (NSMenuItem *)menuItemWithTitle:(NSString *)title
                            action:(SEL)action
                     representedBy:(id)value
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title
                                                  action:action
                                           keyEquivalent:@""];
    item.target = self;
    if (value) item.representedObject = value;
    return [item autorelease];
}

- (void)rebuildMenus
{
    NSMenu *root = _statusItem.menu;
    [root removeAllItems];

    _stateItem = [self menuItemWithTitle:@"Screencast — Ready"
                                  action:NULL representedBy:nil];
    _stateItem.enabled = NO;
    [root addItem:_stateItem];

    _nextItem = [self menuItemWithTitle:_recording
        ? @"Settings apply to the next recording"
        : @"Selected settings are used by screencast"
                                 action:NULL representedBy:nil];
    _nextItem.enabled = NO;
    _nextItem.hidden = NO;
    [root addItem:_nextItem];
    [root addItem:[NSMenuItem separatorItem]];

    NSMenuItem *modeItem = [self menuItemWithTitle:@"Recording Mode"
                                             action:NULL representedBy:nil];
    NSMenu *modeMenu = [[[NSMenu alloc] initWithTitle:@"Recording Mode"] autorelease];
    _displayModeItem = [self menuItemWithTitle:@"Display"
                                         action:@selector(modeChanged:)
                                  representedBy:@(StatusModeDisplay)];
    _presenterModeItem = [self menuItemWithTitle:@"Presenter"
                                           action:@selector(modeChanged:)
                                    representedBy:@(StatusModePresenter)];
    NSInteger preferred = [[NSUserDefaults standardUserDefaults]
        integerForKey:PreferredModeKey];
    if (preferred != StatusModePresenter) preferred = StatusModeDisplay;
    _displayModeItem.state = preferred == StatusModeDisplay
        ? NSControlStateValueOn : NSControlStateValueOff;
    _presenterModeItem.state = preferred == StatusModePresenter
        ? NSControlStateValueOn : NSControlStateValueOff;
    [modeMenu addItem:_displayModeItem];
    [modeMenu addItem:_presenterModeItem];
    modeItem.submenu = modeMenu;
    [root addItem:modeItem];

    NSMenuItem *cameraItem = [self menuItemWithTitle:@"Camera"
                                               action:NULL representedBy:nil];
    NSMenu *cameraMenu = [[[NSMenu alloc] initWithTitle:@"Camera"] autorelease];
    NSString *cameraUID = saved_string(CameraDeviceKey);
    NSMenuItem *defaultCamera = [self menuItemWithTitle:@"System Default"
                                                  action:@selector(cameraChanged:)
                                           representedBy:nil];
    defaultCamera.state = cameraUID.length == 0
        ? NSControlStateValueOn : NSControlStateValueOff;
    [cameraMenu addItem:defaultCamera];
    for (AVCaptureDevice *device in [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]) {
        NSMenuItem *item = [self menuItemWithTitle:device.localizedName
                                             action:@selector(cameraChanged:)
                                      representedBy:device.uniqueID];
        item.state = selected_device(cameraUID, device)
            ? NSControlStateValueOn : NSControlStateValueOff;
        [cameraMenu addItem:item];
    }
    cameraItem.submenu = cameraMenu;
    [root addItem:cameraItem];

    NSMenuItem *audioItem = [self menuItemWithTitle:@"Audio"
                                              action:NULL representedBy:nil];
    NSMenu *audioMenu = [[[NSMenu alloc] initWithTitle:@"Audio"] autorelease];
    NSMenuItem *micItem = [self menuItemWithTitle:@"Microphone"
                                            action:NULL representedBy:nil];
    NSMenu *micMenu = [[[NSMenu alloc] initWithTitle:@"Microphone"] autorelease];
    NSString *micUID = saved_string(MicDeviceKey);
    NSMenuItem *defaultMic = [self menuItemWithTitle:@"System Default"
                                               action:@selector(microphoneChanged:)
                                        representedBy:nil];
    defaultMic.state = micUID.length == 0
        ? NSControlStateValueOn : NSControlStateValueOff;
    [micMenu addItem:defaultMic];
    for (AVCaptureDevice *device in [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio]) {
        NSMenuItem *item = [self menuItemWithTitle:device.localizedName
                                             action:@selector(microphoneChanged:)
                                      representedBy:device.uniqueID];
        item.state = selected_device(micUID, device)
            ? NSControlStateValueOn : NSControlStateValueOff;
        [micMenu addItem:item];
    }
    micItem.submenu = micMenu;
    [audioMenu addItem:micItem];

    _desktopAudioItem = [self menuItemWithTitle:@"Desktop Audio"
                                          action:@selector(desktopAudioChanged:)
                                   representedBy:nil];
    BOOL desktopAudio = [[NSUserDefaults standardUserDefaults]
        objectForKey:DesktopAudioKey] == nil
        ? YES : [[NSUserDefaults standardUserDefaults] boolForKey:DesktopAudioKey];
    _desktopAudioItem.state = desktopAudio
        ? NSControlStateValueOn : NSControlStateValueOff;
    [audioMenu addItem:_desktopAudioItem];
    audioItem.submenu = audioMenu;
    [root addItem:audioItem];

    [root addItem:[NSMenuItem separatorItem]];
    NSMenuItem *help = [self menuItemWithTitle:@"Drag or resize the presenter window while recording"
                                         action:NULL representedBy:nil];
    help.enabled = NO;
    help.hidden = !_recording || !_presenter;
    [root addItem:help];
}

- (void)setRecording:(BOOL)recording presenter:(BOOL)presenter
{
    _recording = recording;
    _presenter = presenter;
    NSString *label = recording
        ? [NSString stringWithFormat:@"Screencast — Recording: %@",
           presenter ? @"Presenter" : @"Display"]
        : @"Screencast — Ready";
    if (_stateItem) _stateItem.title = label;
    if (_nextItem) _nextItem.hidden = !recording;
    if (_statusItem.button) {
        _statusItem.button.toolTip = label;
        _statusItem.button.accessibilityLabel = label;
    }
    [self rebuildMenus];
}

- (void)modeChanged:(id)sender
{
    NSInteger mode = [[sender representedObject] integerValue];
    [[NSUserDefaults standardUserDefaults] setInteger:mode forKey:PreferredModeKey];
    [self rebuildMenus];
}

- (void)cameraChanged:(id)sender
{
    NSString *uid = [sender representedObject];
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    if (uid.length) [defaults setObject:uid forKey:CameraDeviceKey];
    else [defaults removeObjectForKey:CameraDeviceKey];
    [self rebuildMenus];
}

- (void)microphoneChanged:(id)sender
{
    NSString *uid = [sender representedObject];
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    if (uid.length) [defaults setObject:uid forKey:MicDeviceKey];
    else [defaults removeObjectForKey:MicDeviceKey];
    [self rebuildMenus];
}

- (void)desktopAudioChanged:(id)sender
{
    (void)sender;
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    BOOL enabled = [defaults objectForKey:DesktopAudioKey] == nil
        ? YES : [defaults boolForKey:DesktopAudioKey];
    [defaults setBool:!enabled forKey:DesktopAudioKey];
    [self rebuildMenus];
}

@end

static ScreencastStatusController *s_controller;

int status_menu_start(void)
{
    if (![NSThread isMainThread]) return -1;
    if (s_controller) return 0;

    s_controller = [[ScreencastStatusController alloc] init];
    NSStatusBar *bar = [NSStatusBar systemStatusBar];
    s_controller->_statusItem = [[bar statusItemWithLength:NSVariableStatusItemLength] retain];
    NSButton *button = s_controller->_statusItem.button;
    NSImage *icon = [NSImage imageWithSystemSymbolName:@"video.fill"
                                accessibilityDescription:@"Screencast"];
    if (!icon) icon = [NSImage imageNamed:NSImageNameTouchBarCommunicationVideoTemplate];
    if (!icon) icon = [NSImage imageNamed:NSImageNameApplicationIcon];
    icon.template = YES;
    icon.size = NSMakeSize(18.0, 18.0);
    button.image = icon;
    button.imagePosition = NSImageOnly;
    button.toolTip = @"Screencast — Ready";
    s_controller->_statusItem.menu = [[[NSMenu alloc] initWithTitle:@"Screencast"] autorelease];
    [s_controller rebuildMenus];
    return 0;
}

void status_menu_stop(void)
{
    if (!s_controller) return;
    [[NSStatusBar systemStatusBar] removeStatusItem:s_controller->_statusItem];
    [s_controller release];
    s_controller = nil;
}

void status_menu_set_recording(int recording, int presenter)
{
    if (s_controller) [s_controller setRecording:recording presenter:presenter];
}

int status_menu_preferred_presenter(void)
{
    return [[NSUserDefaults standardUserDefaults]
                integerForKey:PreferredModeKey] == StatusModePresenter;
}

static void copy_preference(NSString *key, char *buf, size_t size)
{
    if (!buf || size == 0) return;
    NSString *value = saved_string(key);
    const char *utf8 = value.UTF8String;
    if (!utf8) utf8 = "";
    snprintf(buf, size, "%s", utf8);
}

void status_menu_camera_target(char *buf, size_t size)
{
    copy_preference(CameraDeviceKey, buf, size);
}

void status_menu_microphone_target(char *buf, size_t size)
{
    copy_preference(MicDeviceKey, buf, size);
}

int status_menu_desktop_audio_enabled(void)
{
    NSUserDefaults *defaults = [NSUserDefaults standardUserDefaults];
    if (![defaults objectForKey:DesktopAudioKey]) return 1;
    return [defaults boolForKey:DesktopAudioKey] ? 1 : 0;
}
