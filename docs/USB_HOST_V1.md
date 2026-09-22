# JCOS USB Host v1

USB Host v1 is the first native post-UEFI USB host milestone for JCOS.

## Supported

- PCI-discovered xHCI host controllers (`0C:03:30`)
- BIOS/OS xHCI ownership handoff
- hardware-derived slot and root-port counts
- 32-byte and 64-byte xHCI context layouts
- controller-requested scratchpad buffers
- USB 2.x / USB 3.x root-port protocol discovery
- root-port power, connection detection, reset and recovery
- xHCI command, event and transfer rings
- device slot enable/address flow
- endpoint-zero control transfers
- controller-independent configuration/interface/endpoint parsing
- directly attached HID Boot Protocol keyboards
- polling-based keyboard input
- safe keyboard-disconnect detection

The Gigabyte Z390 Gaming X bare-metal path is the first hardware acceptance
platform for this milestone.

## Deliberately deferred

- USB hubs
- automatic hotplug/re-enumeration after disconnect
- multiple simultaneously managed USB devices
- multiple xHCI controllers
- general HID report-descriptor parsing
- HID mice/gamepads
- USB Mass Storage / BOT / SCSI
- UAS
- USB audio / isochronous class support
- interrupt-driven xHCI event delivery
- suspend/resume and USB power management
- userspace xHCI drivers and IOMMU-backed DMA isolation

## Layering

    PCI
      |
     xHCI
      |
    USB core
      |
      +-- HID Boot Keyboard
      |
      +-- future class drivers

The xHCI layer owns controller mechanics: capability registers, DMA structures,
rings, slots, ports and transfer submission. The USB core owns
controller-independent USB concepts such as speed, configurations, interfaces,
endpoints and class-descriptor selection.

## Regression gate

Before merging USB Host v1:

1. build with warnings-as-errors;
2. run `test usb-host`;
3. run the existing kernel regression suite;
4. boot on bare metal and verify:
   - `XHCI: READY`;
   - the hardware root-port count is reported;
   - `USB HID: DETECTED`;
   - normal typing and shell navigation work;
   - unplugging the keyboard removes native HID readiness without crashing.
