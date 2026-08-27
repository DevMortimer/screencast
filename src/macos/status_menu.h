// status_menu.h — macOS menu-bar controls for Screencast
#ifndef STATUS_MENU_H
#define STATUS_MENU_H

#include <stddef.h>

/* Create the status item and its native menus.  Main-thread only. */
int status_menu_start(void);
void status_menu_stop(void);

/* Keep the status item accurate while the recorder starts/stops. */
void status_menu_set_recording(int recording, int presenter);

/* Preferred mode used by a plain `screencast` start with no CLI mode. */
int status_menu_preferred_presenter(void);

/* Preferences selected in the menu.  Empty output means system default. */
void status_menu_camera_target(char *buf, size_t size);
void status_menu_microphone_target(char *buf, size_t size);
int status_menu_desktop_audio_enabled(void);

#endif /* STATUS_MENU_H */
