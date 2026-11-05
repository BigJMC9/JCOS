# JCOS — Experimental x86-64 Operating System

**JCOS (JA OS)** is a from-scratch operating system developed by **Jacob Crosbie**. It uses a capability-oriented microkernel architecture with protected Ring 3 programs and services. The current implementation combines that mechanism layer with explicitly bounded, transitional kernel services and drivers.

JCOS boots through UEFI, leaves Boot Services, installs its own paging and interrupt environment, and starts a userspace console shell. It includes service replacement, a fixed-mode graphical session, native USB keyboard input, storage diagnostics, and a protected PCI resource and recovery framework.

> **Status:** active development; the documented source baseline is the untagged commit [`fcfe85714089`](https://github.com/BigJMC9/JCOS/commit/fcfe857140891d7c25a44814e1738970d72a852a). See [Versioning](docs/VERSIONING.md) for public version references and [Documentation](docs/README.md) for the current document set.

## Current capabilities

| Area | Implemented scope |
| --- | --- |
| Boot and protection | Custom x86-64 UEFI loader; ELF kernel loaded at a firmware-selected physical address without runtime relocations; kernel-owned paging, physical direct map, NX, CR0.WP and W^X protection |
| Execution | Isolated address spaces, Process/Thread ownership, single-CPU preemptive scheduling, idle handling, waits, cancellation and process fault outcomes |
| Authority and IPC | Local capability handles, explicit rights, bounded register-message IPC, tracked teardown/rollback and versioned service protocols |
| Programs and services | Static ELF64 programs, transactional launch/rollback, versioned startup grants, bounded service shutdown, explicit reconnect and executable replacement |
| Interactive environment | Boot-default Ring 3 shell, userspace archive namespace and executable selection, focused foreground input/output, and a separate kernel diagnostic monitor |
| Graphics | Separate Ring 3 display and compositor services, validated GOP ownership handoff, bounded shared RGB332 surfaces and focused graphical input |
| Devices | AHCI/block/GPT support; native polling xHCI HID Boot Protocol keyboard support; bounded PCI assignment, BAR access, IRQ, DMA-lifetime and recovery mechanisms |
| Diagnostics | Individual tests, aggregate and repeated stress suites, resource-baseline checks, framebuffer scrollback and COM1 logging |

### Architecture and present boundaries

The kernel enforces address-space protection, scheduling, capabilities, IPC, object lifetime and hardware-resource access. Ring 3 code owns normal shell editing, command parsing, archive pathname resolution, foreground executable selection, ordinary-service naming/replacement, and compositor policy.

Several components remain privileged: the emergency monitor, bootstrap ELF/VFS helpers, service and graphical-session orchestration, raw console/archive portals, and the AHCI, xHCI and audio drivers. The bounded PCI framework does not mean these existing drivers have moved into userspace.

The [service boundary](docs/SERVICE_BOUNDARY.md) describes authority, cancellation, restart and device-lifetime contracts. The [runtime profile](docs/RUNTIME_PROFILE.md) describes the supported execution environment.

### Graphics

The display service owns the validated firmware GOP framebuffer while the graphical session is active. A separate compositor service chooses placement, z-order and focus and sends bounded presentation requests. Applications receive their own shared surface and input authority; they do not receive direct framebuffer ownership.

The current shared-surface profile has four 64 × 64 RGB332 slots. The interactive `gui` command runs one graphical client and returns on Escape. Recovery tests exercise client, compositor and display-service faults; the interactive loop tears down the session if a component unexpectedly stops. This is fixed-mode, CPU-rendered graphics, without native GPU mode setting or acceleration.

### PCI driver framework

The source includes exclusive PCI function assignment, validated BAR resources, page-bounded MMIO, mediated port I/O, per-assignment IRQ delivery/acknowledgement, legacy INTx routing through the IOAPIC, DMA buffer/submission lifetime tracking, quarantine and a PCIe Function Level Reset backend.

The userspace driver demonstration is a constrained IPC-mediated probe. Its automated DMA/IRQ fault and hang cases use a virtual device path with an enumerated PCI function as identity. They do not establish arbitrary physical-device driver support, IOMMU isolation or hardware DMA/FLR qualification. Generic DMA submissions do not enable bus mastering. See [Device authority and recovery](docs/SERVICE_BOUNDARY.md#device-authority-and-recovery) for the exact boundary.

### Storage and USB

The image contains a GPT partition table and FAT32 EFI System Partition. The loader supplies an immutable boot archive; the normal shell reads its namespace in Ring 3. AHCI block-device and filesystem diagnostics remain available in the kernel monitor.

The native USB profile supports one PCI-discovered xHCI controller and one directly attached HID Boot Protocol keyboard. It uses hardware-reported port/context capabilities and polling after UEFI exits. The recorded bare-metal bring-up platform includes the **Gigabyte Z390 Gaming X**; that is a limited support record, not a claim about all xHCI controllers. See [USB Host v1](docs/USB_HOST_V1.md) for limits and hardware checks.

## Build

[Makefile.msys2](Makefile.msys2) is the current complete build entry point. Despite its name, it can also be used with GNU make and the required tools on Linux/WSL. The repository's `Mkfile` is a legacy NMake description and is not equivalent to the current build.

### MSYS2 MINGW64

Install the toolchain in the **MINGW64** shell:

```sh
pacman -Syu
pacman -S --needed make tar mingw-w64-x86_64-clang mingw-w64-x86_64-lld mingw-w64-x86_64-llvm-tools mingw-w64-x86_64-nasm mingw-w64-x86_64-python
```

QEMU is optional for building and required for the emulator run targets:

```sh
pacman -S --needed mingw-w64-x86_64-qemu
```

Build and validate the complete boot image:

```sh
make -f Makefile.msys2 image
make -f Makefile.msys2 validate
```

### Debian / Ubuntu / WSL

```sh
sudo apt update
sudo apt install make clang lld llvm nasm python3 tar qemu-system-x86 ovmf
make -f Makefile.msys2 image PYTHON=python3
make -f Makefile.msys2 validate PYTHON=python3
```

The build uses freestanding Clang/LLVM, treats compiler warnings as errors, and checks that the kernel and userspace ELF outputs need no runtime relocations. NASM is preferred for kernel architecture assembly; the makefile selects the Clang/GAS source when NASM is unavailable. This selection does not by itself certify both paths on a given checkout.

The principal generated artifacts are:

| Artifact | Purpose |
| --- | --- |
| `build/BOOTX64.EFI` | UEFI loader |
| `build/KERNEL.ELF` | Kernel |
| `rootfs/bin/*.elf` | Independently linked userspace programs |
| `build/ROOTFS.TAR` | Immutable boot archive |
| `build/JA-os.img` | Raw GPT/FAT32 boot image |

`make -f Makefile.msys2` also builds the complete image. Optional media encoding uses FFmpeg only when media inputs are present; it is not required for the base image.

## Run in QEMU

On MSYS2, the makefile supplies its default OVMF firmware paths:

```sh
make -f Makefile.msys2 run
```

On Linux/WSL, clear those MSYS2 defaults so the run script can locate a matching installed OVMF pair:

```sh
make -f Makefile.msys2 run PYTHON=python3 OVMF_CODE= OVMF_VARS=
```

Alternatively, set both `OVMF_CODE` and `OVMF_VARS` to a matching firmware pair. `run-headless` uses the same image and serial console without a graphical window. The run script starts a Q35 machine with 256 MiB RAM and sends COM1 output to the launching terminal; it copies the firmware variable store to `build/OVMF_VARS.runtime.fd` for the run.

The default QEMU configuration does not exercise the physical USB keyboard path. Emulator, model and hardware checks have different coverage; see [Testing](docs/TESTING.md).

## Use the system

The normal boot enters the Ring 3 shell. `help` lists that shell's commands. Useful examples are:

```text
ls
stat /bin/hello.elf
run /bin/hello.elf
gui
service start worker
service status worker
service replace worker
service stop worker
monitor
```

Escape leaves the graphical session. In the normal shell, `monitor` or the kernel-owned Escape route enters the emergency/development monitor. Enter `usershell` there to return to the userspace shell.

Diagnostics run in the monitor:

```text
test list
test all
test all deep
```

The suite reports tests, iterations, failed steps and any safety abort. Its size is discovered from the checked-out source rather than fixed in this README. See [Testing](docs/TESTING.md) for targeted commands, cleanup and evidence capture.

## Boot on real hardware

`build/JA-os.img` is a **raw disk image**. Write it to the **whole USB device**, not a partition and not as a normal file on an existing filesystem.

**Writing the image replaces the target device's contents. Identify the USB disk by size and device information before choosing the output path.** Linux users can inspect `lsblk`; MSYS2 users can compare `cat /proc/partitions` with Windows `Get-Disk` output. Device letters can change between boots.

After identifying the whole device, an example command is:

```sh
dd if=build/JA-os.img of=/dev/sdX bs=4M status=progress conv=fsync
```

Replace `/dev/sdX` with the verified device; Linux normally requires `sudo`, and Windows/MSYS2 requires appropriate raw-disk access. Boot in **UEFI x86-64 mode** with Secure Boot disabled because the loader is unsigned. Record the image hash and machine configuration when reporting results.

## Current limitations

- The supported execution profile is single logical CPU and general-purpose registers only. General FP/SIMD/TLS context support and SMP are not established by this build.
- Userspace programs use static ELF64 and the project's narrow runtime; this is not a general POSIX or Linux binary environment.
- The ordinary background-service broker has one slot, foreground console focus is single-client, and the graphical surface/profile sizes are bounded.
- The boot archive is immutable and read-only; a general writable userspace filesystem stack is not provided by it.
- Physical drivers still include privileged implementations. Unrestricted userspace bus mastering and IOMMU-backed device assignment are not available.
- The PCI IRQ backend supports firmware-programmed legacy INTx/IOAPIC routing; MSI/MSI-X and general ACPI interrupt-routing discovery are outside its current scope.
- USB hubs, general HID, USB mass storage, multiple active controllers/devices and automatic re-enumeration are outside the current USB profile.
- Hardware support and fault recovery are limited to the paths actually qualified. A model test or virtual-device recovery test is not a hardware acceptance result.

## Repository layout

| Directory | Contents |
| --- | --- |
| `loader/` | UEFI loader, ELF loading and firmware handoff |
| `kernel/` | Kernel mechanisms, transitional drivers/services, architecture code and diagnostics |
| `include/` | Shared kernel/userspace ABI and protocol definitions |
| `user/` | Ring 3 programs, services, probes and freestanding libraries |
| `rootfs/` | Files packed into the immutable boot archive |
| `docs/` | Current architecture, runtime, testing and support contracts |
| `tools/` | Build, image, emulator and optional media tooling |

## License

Copyright (C) 2026 Jacob Crosbie.

JCOS is licensed under the **GNU General Public License v3.0 only** (`GPL-3.0-only`). See [LICENSE](LICENSE) for the license text.
