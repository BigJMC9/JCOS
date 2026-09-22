#include "usb_core.h"

#include "lib.h"

#define USB_DESC_CONFIGURATION 2U
#define USB_DESC_INTERFACE 4U
#define USB_DESC_ENDPOINT 5U
#define USB_CLASS_HID 3U
#define USB_HID_BOOT_SUBCLASS 1U
#define USB_HID_KEYBOARD_PROTOCOL 1U
#define USB_ENDPOINT_DIR_IN 0x80U
#define USB_ENDPOINT_TRANSFER_MASK 0x03U
#define USB_ENDPOINT_INTERRUPT 0x03U

u16 usb_initial_ep0_max_packet(UsbSpeed speed) {
    switch (speed) {
        case USB_SPEED_LOW: return 8U;
        case USB_SPEED_FULL: return 64U;
        case USB_SPEED_HIGH: return 64U;
        case USB_SPEED_SUPER: return 512U;
        default: return 8U;
    }
}

bool usb_find_boot_keyboard(const u8 *descriptor, u16 length,
                            UsbBootKeyboardInfo *keyboard) {
    if (!descriptor || !keyboard || length < 2U) return false;

    k_memset(keyboard, 0, sizeof(*keyboard));

    bool hid_boot_keyboard = false;
    UsbInterfaceInfo current;
    k_memset(&current, 0, sizeof(current));

    for (u16 offset = 0; offset + 2U <= length;) {
        u8 size = descriptor[offset];
        u8 type = descriptor[offset + 1U];

        if (size < 2U || offset + size > length) return false;

        if (type == USB_DESC_CONFIGURATION && size >= 9U) {
            keyboard->configuration_value = descriptor[offset + 5U];
        } else if (type == USB_DESC_INTERFACE) {
            hid_boot_keyboard = false;
            if (size >= 9U) {
                current.number = descriptor[offset + 2U];
                current.alternate_setting = descriptor[offset + 3U];
                current.class_code = descriptor[offset + 5U];
                current.subclass = descriptor[offset + 6U];
                current.protocol = descriptor[offset + 7U];
                hid_boot_keyboard =
                    current.class_code == USB_CLASS_HID &&
                    current.subclass == USB_HID_BOOT_SUBCLASS &&
                    current.protocol == USB_HID_KEYBOARD_PROTOCOL;
            }
        } else if (hid_boot_keyboard &&
                   type == USB_DESC_ENDPOINT && size >= 7U) {
            u8 address = descriptor[offset + 2U];
            u8 attributes = descriptor[offset + 3U];
            if ((address & USB_ENDPOINT_DIR_IN) &&
                (attributes & USB_ENDPOINT_TRANSFER_MASK) ==
                    USB_ENDPOINT_INTERRUPT) {
                u16 packet_encoding =
                    (u16)descriptor[offset + 4U] |
                    ((u16)descriptor[offset + 5U] << 8);
                keyboard->interface = current;
                keyboard->endpoint.address = address;
                keyboard->endpoint.attributes = attributes;
                keyboard->endpoint.max_packet_size =
                    packet_encoding & 0x07FFU;
                keyboard->endpoint.transactions =
                    (u8)(((packet_encoding >> 11) & 0x03U) + 1U);
                keyboard->endpoint.interval = descriptor[offset + 6U];
                return keyboard->configuration_value != 0U &&
                    keyboard->endpoint.max_packet_size != 0U;
            }
        }

        offset = (u16)(offset + size);
    }

    return false;
}
