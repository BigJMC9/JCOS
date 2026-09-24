# JCOS — Experimental x86-64 Microkernel Operating System

**JCOS (JA OS)** is a from-scratch x86-64 operating system developed by **Jacob Crosbie**. It explores a capability-oriented microkernel design with a deliberately small privileged mechanism layer and increasingly capable Ring 3 services.

The project boots through UEFI, takes ownership of the machine after `ExitBootServices()`, establishes its own memory-management and interrupt environment, launches isolated userspace programs and services, and includes native storage, USB, console, IPC, scheduling, and diagnostic infrastructure.

> **Status:** active development. R7 established the current userspace service boundary and native USB host path. R8 work is expanding the system toward a more persistent graphical/application environment.

## Highlights

- x86-64 UEFI boot with a custom EFI loader
- Position-independent ELF kernel loaded at a firmware-selected physical address
- Custom physical and virtual memory management
- NX, CR0.WP and kernel W^X protections
- Ring 3 userspace execution and isolated address spaces
- Capability-based authority model
- Bounded IPC and versioned userspace service protocols
- Preemptive scheduling, waits, cancellation and process/thread lifecycle handling
- Managed userspace services with restart/recovery semantics
- Boot-default Ring 3 console shell with an emergency kernel monitor
- Foreground application input/output with explicit focus/session ownership
- ELF program launching with transactional rollback and capability grants
- ACPI discovery plus APIC/IOAPIC interrupt infrastructure
- AHCI storage and block-device support
- GPT partition discovery
- Native PCI-discovered xHCI USB host controller support
- USB core and HID Boot Protocol keyboard support
- Framebuffer terminal plus COM1 serial diagnostics
- Root filesystem / boot archive infrastructure
- Extensive kernel, userspace, service-lifetime and race-sensitive test suites
- QEMU/OVMF and real-hardware testing

## Architecture

JCOS is moving policy out of the kernel and into userspace.

The privileged kernel is responsible for protection and mechanism:

- address spaces and memory protection
- threads, scheduling and waits
- capabilities and object lifetime
- bounded IPC
- process outcomes and fault containment
- minimal hardware-facing mechanisms

Higher-level policy is intended to live in Ring 3 services:

- shell and console behavior
- service protocols
- launch/restart policy
- formatting
- retry/replay decisions
- application-facing I/O
- future filesystem and device-service policy

Capability handles are local authority rather than global object names. Rights are explicitly granted and can be attenuated when transferred to another process.

Service protocols are versioned, bounded and designed so that a crashed service can be replaced without silently retargeting stale client authority.

For more detail, see [docs/SERVICE_BOUNDARY.md](docs/SERVICE_BOUNDARY.md).

## Boot flow

1. UEFI firmware loads `EFI/BOOT/BOOTX64.EFI` from the EFI System Partition.
2. The loader opens and validates `KERNEL.ELF`.
3. ELF program segments are loaded into firmware-allocated memory.
4. GOP framebuffer information and the final UEFI memory map are collected.
5. A dedicated kernel stack and boot handoff structure are prepared.
6. The loader calls `ExitBootServices()`.
7. The x86-64 entry stub establishes the kernel execution environment.
8. JCOS initializes memory protection, interrupts, platform discovery and core kernel subsystems.
9. Userspace services and the normal Ring 3 shell are brought online.

After `ExitBootServices()`, JCOS no longer relies on UEFI Boot Services, BIOS interrupts, a host operating system or libc.

## Userspace and services

JCOS includes a growing userspace environment rather than keeping ordinary policy in the kernel.

Current infrastructure includes:

- independently linked Ring 3 ELF programs
- capability-scoped startup grants
- versioned service request/reply protocols
- managed service launch and replacement
- service incarnation tracking
- fault containment and reaping
- console output portals
- foreground application input and output
- userspace shell policy
- program/service brokers
- supervisor/worker service experiments

The kernel monitor remains as a development and emergency surface while the normal interactive path runs in userspace.

## Storage

The storage path currently includes:

- PCI discovery
- AHCI controller support
- DMA-backed block I/O
- block-device abstraction
- GPT partition discovery
- boot/root filesystem infrastructure

The system image itself uses GPT and a FAT32 EFI System Partition.

## USB

JCOS includes a native post-UEFI USB host stack.

USB Host v1 supports:

- PCI-discovered xHCI controllers
- BIOS/OS ownership handoff
- hardware-derived slot and root-port counts
- 32-byte and 64-byte xHCI context layouts
- scratchpad buffers
- USB 2.x / USB 3.x root-port discovery
- port reset and recovery
- xHCI command, event and transfer rings
- device slot enable/address flow
- endpoint-zero control transfers
- configuration/interface/endpoint parsing
- directly attached HID Boot Protocol keyboards
- polling-based keyboard input
- safe keyboard-disconnect detection

The first bare-metal acceptance platform is a **Gigabyte Z390 Gaming X** system.

See [docs/USB_HOST_V1.md](docs/USB_HOST_V1.md) for the exact supported/deferred scope.

## Diagnostics and testing

JCOS has a registry-backed diagnostic harness intended for both QEMU and bare-metal validation.

From the emergency monitor:

~~~text
test list
test NAME
test all
test all deep
~~~

`test all` runs the full registered suite once.

`test all deep` repeats stress-sensitive scheduler, ordering, lifetime and service-recovery tests to increase the chance of exposing race and cleanup failures.

The harness records:

- distinct tests passed/failed
- total iterations
- captured failing stages
- cleanup failures
- retained state requiring a reboot
- tests skipped after an unsafe baseline

Long-running suites retain logical scrollback while avoiding extremely slow full-framebuffer line shifting.

See [docs/TESTING.md](docs/TESTING.md).

## Build requirements

### Debian / Ubuntu / WSL

~~~sh
sudo apt update
sudo apt install clang lld nasm binutils python3 qemu-system-x86 ovmf
~~~

### MSYS2 MINGW64

~~~sh
pacman -Syu
pacman -S --needed make mingw-w64-x86_64-clang mingw-w64-x86_64-lld mingw-w64-x86_64-llvm-tools mingw-w64-x86_64-nasm mingw-w64-x86_64-python
~~~

Optional QEMU package:

~~~sh
pacman -S --needed mingw-w64-x86_64-qemu
~~~

## Build

On MSYS2 MINGW64:

~~~sh
make -f Makefile.msys2
~~~

Build the bootable GPT/FAT32 image:

~~~sh
make -f Makefile.msys2 image
~~~

Validate the generated image:

~~~sh
make -f Makefile.msys2 validate
~~~

The principal outputs are:

~~~text
build/BOOTX64.EFI
build/KERNEL.ELF
build/JA-os.img
~~~

A Visual Studio Developer Command Prompt can use:

~~~cmd
nmake /f Mkfile
nmake /f Mkfile image
~~~

## Run in QEMU

With QEMU/OVMF installed:

~~~sh
make -f Makefile.msys2 run
~~~

The firmware initially owns the machine, then hands control to the JCOS loader and kernel. Once boot services are exited, the kernel uses its own framebuffer/serial, interrupt, memory and device infrastructure.

## Boot on real hardware

The generated `build/JA-os.img` is a raw disk image and must be written to the **whole USB device**, not copied as a normal file onto an existing filesystem.

Example on Linux:

~~~sh
lsblk
sudo dd if=build/JA-os.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
~~~

Example from an MSYS2 environment where the USB device is exposed as `/dev/sdX`:

~~~sh
dd if=build/JA-os.img of=/dev/sdX bs=4M status=progress conv=fsync
~~~

**Warning:** writing the image destroys the previous contents of the selected target device. Verify the target disk first.

Boot the device in **UEFI x86-64 mode**. Secure Boot currently needs to be disabled because the EFI loader is unsigned.

## Repository layout

~~~text
loader/      UEFI loader, ELF loading and firmware handoff
kernel/      microkernel, architecture code, memory, IPC, scheduling,
             storage, USB, console mechanisms and diagnostics
include/     shared kernel/userspace ABIs and versioned service protocols
user/        Ring 3 services, applications, test programs and userspace libraries
rootfs/      files included in the boot/root filesystem
docs/        architecture, testing and hardware-support documentation
tools/       image/build support tooling
~~~

## Development principles

JCOS is intentionally built around a few recurring rules:

1. **Mechanism in the kernel, policy in userspace.**
2. **Explicit authority.** Processes receive only the capabilities they need.
3. **Bounded interfaces.** IPC and service protocols use fixed, inspectable contracts.
4. **No silent replay.** A service restart does not implicitly replay ambiguous application operations.
5. **Failure containment.** A userspace fault should terminate the responsible process rather than destabilize the kernel.
6. **Test the real machine.** QEMU is useful, but hardware behavior is part of the acceptance gate.

## Current scope and limitations

JCOS is still an experimental operating system and is not intended for production use.

Notable current limitations include:

- x86-64 UEFI only
- unsigned EFI loader / no Secure Boot support
- single-logical-CPU development profile; SMP is not yet a completed production path
- USB hubs, general HID, USB mass storage and hotplug/re-enumeration remain deferred
- the xHCI path is currently polling-based rather than interrupt-driven
- many device drivers and higher-level policies are still kernel-resident or transitional
- filesystem/device policy is still being migrated toward the intended userspace architecture
- hardware coverage is intentionally limited while the architecture is still changing rapidly

## Project status

The early project began as a small standalone UEFI kernel. It has since grown into a capability-oriented operating-system environment with protected Ring 3 execution, services, IPC, storage, native USB, program launching and extensive diagnostics.

Current work is focused on the next stage of the userspace/application environment while preserving the protection and recovery guarantees established by the earlier milestones.

## License

Copyright (C) 2026 Jacob Crosbie.

JCOS is free software licensed under the **GNU General Public License v3.0 only** (`GPL-3.0-only`). You may redistribute and/or modify it under the terms of version 3 of the GNU General Public License as published by the Free Software Foundation.

See the full [GNU GPL v3.0 license text](LICENSE) for details.
