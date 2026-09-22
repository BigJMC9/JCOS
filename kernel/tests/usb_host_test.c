#include "usb_host_test.h"

#include "terminal.h"
#include "usb_core.h"
#include "usb_xhci_model.h"

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

void usb_host_test_run(void) {
    terminal_writeln("USB HOST V1 MODEL / DESCRIPTOR TEST:");

    u32 hcs1 = 0x1A000040U;
    bool limits =
        xhci_hcs1_max_slots(hcs1) == 64U &&
        xhci_hcs1_max_ports(hcs1) == 26U;
    report("HARDWARE SLOT / PORT LIMIT DECODE", limits);

    u32 hcs2 = 0x10200000U;
    bool scratchpads = xhci_hcs2_scratchpad_count(hcs2) == 34U;
    report("SCRATCHPAD COUNT DECODE", scratchpads);

    bool context_layout =
        xhci_context_offset(32U, 1U) == 32U &&
        xhci_context_offset(32U, 2U) == 64U &&
        xhci_context_offset(64U, 1U) == 64U &&
        xhci_context_offset(64U, 2U) == 128U &&
        xhci_context_offset(48U, 1U) == 0U;
    report("32 / 64 BYTE CONTEXT OFFSETS", context_layout);

    bool speed_model =
        xhci_port_usb_speed(1U, 2U) == USB_SPEED_FULL &&
        xhci_port_usb_speed(2U, 2U) == USB_SPEED_LOW &&
        xhci_port_usb_speed(3U, 2U) == USB_SPEED_HIGH &&
        xhci_port_usb_speed(4U, 3U) == USB_SPEED_SUPER &&
        usb_initial_ep0_max_packet(USB_SPEED_LOW) == 8U &&
        usb_initial_ep0_max_packet(USB_SPEED_FULL) == 64U &&
        usb_initial_ep0_max_packet(USB_SPEED_HIGH) == 64U &&
        usb_initial_ep0_max_packet(USB_SPEED_SUPER) == 512U;
    report("USB SPEED / EP0 INITIAL MODEL", speed_model);

    bool completions =
        xhci_completion_code_ok(1U, false) &&
        !xhci_completion_code_ok(13U, false) &&
        xhci_completion_code_ok(13U, true) &&
        !xhci_completion_code_ok(4U, true);
    report("XHCI COMPLETION CLASSIFICATION", completions);

    static const u8 keyboard_configuration[] = {
        9U, 2U, 25U, 0U, 1U, 1U, 0U, 0x80U, 50U,
        9U, 4U, 2U, 0U, 1U, 3U, 1U, 1U, 0U,
        7U, 5U, 0x81U, 0x03U, 8U, 0U, 10U
    };

    UsbBootKeyboardInfo keyboard;
    bool keyboard_parse =
        usb_find_boot_keyboard(keyboard_configuration,
            (u16)sizeof(keyboard_configuration), &keyboard) &&
        keyboard.configuration_value == 1U &&
        keyboard.interface.number == 2U &&
        keyboard.interface.class_code == 3U &&
        keyboard.interface.subclass == 1U &&
        keyboard.interface.protocol == 1U &&
        keyboard.endpoint.address == 0x81U &&
        keyboard.endpoint.max_packet_size == 8U &&
        keyboard.endpoint.transactions == 1U &&
        keyboard.endpoint.interval == 10U;
    report("HID BOOT KEYBOARD DESCRIPTOR PARSE", keyboard_parse);

    static const u8 mouse_configuration[] = {
        9U, 2U, 25U, 0U, 1U, 1U, 0U, 0x80U, 50U,
        9U, 4U, 0U, 0U, 1U, 3U, 1U, 2U, 0U,
        7U, 5U, 0x81U, 0x03U, 8U, 0U, 10U
    };

    UsbBootKeyboardInfo not_keyboard;
    bool reject_non_keyboard =
        !usb_find_boot_keyboard(mouse_configuration,
            (u16)sizeof(mouse_configuration), &not_keyboard);
    report("NON-KEYBOARD HID REJECTED", reject_non_keyboard);

    bool pass = limits && scratchpads && context_layout &&
        speed_model && completions && keyboard_parse &&
        reject_non_keyboard;

    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write("USB HOST V1 MODEL / DESCRIPTOR TEST: ");
    terminal_writeln(pass ? "PASS" : "FAILED");
    terminal_set_color(terminal_default_color());
}
