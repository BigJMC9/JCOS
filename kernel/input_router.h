#ifndef JA_OS_INPUT_ROUTER_H
#define JA_OS_INPUT_ROUTER_H

#include "key_event.h"

typedef enum {
    INPUT_ROUTE_INVALID = 0,
    INPUT_ROUTE_FALLBACK,
    INPUT_ROUTE_GRAPHICS,
    INPUT_ROUTE_DROPPED
} InputRouteResult;

/* Route one already-decoded key event according to the current graphical
 * focus. With no graphical focus, copy it to fallback unchanged. A focused
 * graphical client owns the event exclusively: delivery failure is DROPPED,
 * never leaked through to the shell underneath. */
InputRouteResult input_route_event(const KeyEvent *event, KeyEvent *fallback);

/* Poll the ordinary PS/2/xHCI/serial input sources, apply the same focus
 * policy, and return true only when the caller should consume a fallback
 * (currently shell) event. Focused graphical events are consumed here. */
bool input_poll_routed(KeyEvent *fallback);

#endif