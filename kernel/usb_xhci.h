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
} XhciFailureRecord;

bool xhci_init(VmPageMap *kernel_map);
void xhci_poll(void);
bool xhci_present(void);
bool xhci_get_event(KeyEvent *event);
u32 xhci_failure_count(void);
const XhciFailureRecord *xhci_failure(u32 index);

#endif