#ifndef JA_OS_USB_XHCI_MODEL_H
#define JA_OS_USB_XHCI_MODEL_H

#include "types.h"
#include "usb_core.h"

u32 xhci_hcs1_max_slots(u32 hcs1);
u32 xhci_hcs1_max_ports(u32 hcs1);
u32 xhci_hcs2_scratchpad_count(u32 hcs2);
u32 xhci_context_offset(u32 context_size, u32 context_index);
UsbSpeed xhci_port_usb_speed(u8 speed_id, u8 protocol_major);
bool xhci_completion_code_ok(u32 completion_code, bool allow_short_packet);

#endif
