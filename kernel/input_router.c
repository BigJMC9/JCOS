#include "input_router.h"

#include "display_service.h"
#include "input.h"
#include "lib.h"
#include "../include/display_service_protocol.h"

static void clear_event(KeyEvent *event) {
    if (event) k_memset(event, 0, sizeof(*event));
}

InputRouteResult input_route_event(const KeyEvent *event, KeyEvent *fallback) {
    if (!event) {
        clear_event(fallback);
        return INPUT_ROUTE_INVALID;
    }

    KeyEvent routed = *event;
    clear_event(fallback);

    /* A crashed/stopped compositor cannot retain keyboard focus. Preserve the
     * kernel/serial recovery path even if stale focus metadata still exists. */
    if (!display_service_running() ||
        display_service_focus_slot() == JCOS_DISPLAY_SURFACE_FOCUS_NONE) {
        if (fallback) *fallback = routed;
        return INPUT_ROUTE_FALLBACK;
    }

    /* Focus is an authority boundary. If the focused endpoint cannot accept
     * this event, do not replay it into the shell with different authority. */
    return display_service_input_event(&routed) ?
        INPUT_ROUTE_GRAPHICS : INPUT_ROUTE_DROPPED;
}

bool input_poll_routed(KeyEvent *fallback) {
    if (!fallback) return false;
    clear_event(fallback);

    KeyEvent event;
    k_memset(&event, 0, sizeof(event));
    if (!input_poll(&event)) return false;

    return input_route_event(&event, fallback) == INPUT_ROUTE_FALLBACK;
}