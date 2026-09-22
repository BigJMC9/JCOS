#ifndef JA_OS_USB_CORE_H
#define JA_OS_USB_CORE_H

#include "types.h"

typedef enum {
    USB_SPEED_UNKNOWN = 0,
    USB_SPEED_LOW,
    USB_SPEED_FULL,
    USB_SPEED_HIGH,
    USB_SPEED_SUPER
} UsbSpeed;

typedef struct {
    u8 number;
    u8 alternate_setting;
    u8 class_code;
    u8 subclass;
    u8 protocol;
} UsbInterfaceInfo;

typedef struct {
    u8 address;
    u8 attributes;
    u16 max_packet_size;
    u8 interval;
    u8 transactions;
} UsbEndpointInfo;

typedef struct {
    u8 configuration_value;
    UsbInterfaceInfo interface;
    UsbEndpointInfo endpoint;
} UsbBootKeyboardInfo;

/* Conservative EP0 size before bMaxPacketSize0 is known. */
u16 usb_initial_ep0_max_packet(UsbSpeed speed);

/* Locate a HID Boot Protocol keyboard interrupt-IN endpoint. */
bool usb_find_boot_keyboard(const u8 *descriptor, u16 length,
    UsbBootKeyboardInfo *keyboard);

#endif
