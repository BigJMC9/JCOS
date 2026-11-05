# JCOS USB Host v1

USB Host v1 names the current bounded native USB support profile. It is not an OS release number. The stack runs after UEFI Boot Services exit and currently remains kernel-resident.

## Supported profile

| Area | Current behavior |
| --- | --- |
| Discovery | One PCI-discovered xHCI controller with class/subclass/programming interface `0C:03:30` |
| Ownership | BIOS/OS xHCI handoff where the controller exposes the legacy capability |
| Hardware layout | Hardware-derived slot/root-port counts, 32-byte or 64-byte contexts and controller-requested scratchpad buffers |
| Root ports | USB 2.x/3.x protocol discovery, power, connection detection, reset and recovery |
| Transfers | Command/event/transfer rings, slot enable/address, endpoint-zero control requests and configuration selection |
| Class support | One directly attached HID Boot Protocol keyboard, identified by configuration/interface/endpoint descriptors |
| Input | Polling the xHCI event ring and decoding Boot Protocol keyboard reports |
| Disconnect | Detect the selected keyboard's disconnection, withdraw readiness and stop delivering its queued input |

The first recorded bare-metal bring-up platform is the **Gigabyte Z390 Gaming X**. Controller discovery is PCI class-based, rather than restricted to that motherboard name; the support record remains limited to hardware actually tested.

## Current exclusions

- USB hubs and devices reached through hubs.
- Multiple simultaneously managed USB devices or xHCI controllers.
- Automatic hotplug/re-enumeration after disconnect.
- General HID report-descriptor parsing, mice and gamepads.
- USB mass storage (BOT/SCSI or UAS).
- USB audio and isochronous class support.
- Interrupt-driven xHCI event delivery.
- Suspend/resume and USB power management.
- Userspace xHCI execution or IOMMU-backed isolation of its DMA.

A USB keyboard functioning before `ExitBootServices()` is not evidence that this native stack works. A keyboard also needs the supported Boot Protocol interface; a generic USB label is insufficient. Reconnecting a disconnected keyboard is not an accepted re-enumeration path in this profile.

## Layering

PCI discovery identifies the controller. [usb_xhci.c](../kernel/usb_xhci.c) owns its registers, DMA structures, command/event/transfer rings, slots, root ports and transfer submission. [usb_core.c](../kernel/usb_core.c) handles controller-independent USB descriptor interpretation and Boot Keyboard endpoint selection. [usb_xhci_model.c](../kernel/usb_xhci_model.c) contains the bounded capability/layout helpers used by model tests.

Hardware polling feeds the existing input mechanism. The normal shell and foreground applications receive public key events through the [console/input boundary](SERVICE_BOUNDARY.md#console-input-and-normal-shell); they do not receive xHCI MMIO or DMA authority. The separate PCI resource framework does not automatically migrate or isolate the existing xHCI driver.

## Automated diagnostic

Run from the kernel monitor:

```text
test usb-host
```

This is a **model/descriptor test**. It checks:

- slot/port and scratchpad count decoding;
- 32-byte and 64-byte context offsets;
- USB speed and initial EP0 packet-size models;
- completion-code classification;
- Boot Keyboard descriptor parsing and rejection of a non-keyboard HID descriptor.

It does not enumerate a real controller/keyboard, exercise physical transfer DMA or prove safe recovery from every controller error. Pair it with the hardware procedure below. General suite and cleanup behavior is in [Testing](TESTING.md).

## Hardware verification

Build the image with the current warnings-as-errors workflow and validate it using the [README](../README.md#build). Record the source commit, image hash, motherboard/firmware, controller identity and keyboard/port used.

1. Boot with the keyboard connected directly to the selected controller.
2. Inspect the boot report for `XHCI: READY`, the `ROOT PORTS:` count and `USB HID: DETECTED`.
3. Confirm normal typing, modifiers, editing/history, Page Up/Page Down and Ctrl+Page Up/Ctrl+Page Down in the Ring 3 shell.
4. Enter `monitor`, run `test usb-host`, and inspect `pci` for the expected controller. Preserve any `XHCI: FAIL` diagnostics from COM1/boot output.
5. Run the relevant input tests and aggregate suites using [Testing](TESTING.md); retain the full serial log.
6. With another diagnostic input/output path available, unplug the keyboard. Confirm native readiness is withdrawn without a kernel crash and capture `XHCI: USB HID keyboard disconnected` when emitted. Reboot with the keyboard reattached before resuming keyboard-dependent checks.

`XHCI: READY` describes controller initialization; `USB HID: DETECTED` describes the selected keyboard. Record both. A decoded root-port count or a model-test PASS alone does not establish keyboard enumeration.
