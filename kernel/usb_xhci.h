#ifndef JA_OS_USB_XHCI_H
#define JA_OS_USB_XHCI_H

#include "types.h"
#include "key_event.h"
#include "vmm.h"

typedef struct {
	char stage[40];
	u32 usbcmd;
	u32 usbsts;
	u32 detail;
	u32 port;
	u32 protocol_major;
	u32 speed_id;
	u32 completion_code;
	u32 event_slot;
	u32 event_endpoint;
} XhciFailureRecord;

bool xhci_init(VmPageMap *kernel_map);
void xhci_poll(void);

/* Controller readiness is distinct from finding a supported HID keyboard. */
bool xhci_controller_ready(void);
u32 xhci_root_port_count(void);
u32 xhci_scratchpad_count(void);

bool xhci_present(void);
bool xhci_get_event(KeyEvent *event);
u32 xhci_failure_count(void);
const XhciFailureRecord *xhci_failure(u32 index);

#endif