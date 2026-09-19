#ifndef JA_OS_KEY_EVENT_H
#define JA_OS_KEY_EVENT_H

#include "types.h"

typedef enum {
    KEY_NONE = 0,
    KEY_CHARACTER,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ESCAPE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN
} KeyCode;

typedef struct {
    KeyCode key;
    char character;
    bool pressed;
    bool shift;
    bool ctrl;
    bool alt;
} KeyEvent;

#endif
