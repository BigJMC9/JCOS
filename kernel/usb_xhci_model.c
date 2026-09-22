#include "usb_xhci_model.h"

u32 xhci_hcs1_max_slots(u32 hcs1) { return hcs1 & 0xFFU; }

u32 xhci_hcs1_max_ports(u32 hcs1) { return (hcs1 >> 24) & 0xFFU; }

u32 xhci_hcs2_scratchpad_count(u32 hcs2) {
    return (((hcs2 >> 21) & 0x1FU) << 5) | ((hcs2 >> 27) & 0x1FU);
}

u32 xhci_context_offset(u32 context_size, u32 context_index) {
    if ((context_size != 32U && context_size != 64U) ||
        context_index > (~0U / context_size)) return 0U;
    return context_size * context_index;
}

UsbSpeed xhci_port_usb_speed(u8 speed_id, u8 protocol_major) {
    if (protocol_major >= 3U || speed_id >= 4U) return USB_SPEED_SUPER;
    switch (speed_id) {
        case 1U: return USB_SPEED_FULL;
        case 2U: return USB_SPEED_LOW;
        case 3U: return USB_SPEED_HIGH;
        default: return USB_SPEED_UNKNOWN;
    }
}

bool xhci_completion_code_ok(u32 completion_code, bool allow_short_packet) {
    if (completion_code == 1U) return true;
    return allow_short_packet && completion_code == 13U;
}
