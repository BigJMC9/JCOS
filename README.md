# JA OS v4 - UEFI loader + standalone x86_64 kernel

This version separates firmware-facing boot code from the kernel:

1. UEFI firmware loads `EFI/BOOT/BOOTX64.EFI` from the FAT32 EFI System Partition.
2. The EFI loader opens `/KERNEL.ELF` from the same disk.
3. It parses the ELF64 program headers and loads the position-independent kernel into RAM.
4. It discovers the UEFI Graphics Output Protocol framebuffer.
5. It allocates a dedicated 64 KiB kernel stack.
6. It captures the final UEFI memory map.
7. It calls `ExitBootServices()`.
8. It jumps to the kernel's NASM `_start` stub.
9. NASM switches to the new stack, bridges Microsoft x64 ABI to System V ABI, and calls C `kernel_main()`.
10. The C kernel writes pixels directly into the framebuffer. It no longer calls UEFI or BIOS services.

## What is actually standalone now?

Once `ExitBootServices()` succeeds, the UEFI boot services are gone. The text you see afterward is rendered by the kernel's own framebuffer routines. There is no `OutputString()`, libc, Linux, Windows, BIOS interrupt, or firmware console involved.

UEFI runtime services still *exist conceptually*, but this project does not use them and does not pass the system table to the kernel.

## Build dependencies

Debian/Ubuntu/WSL:

```sh
sudo apt update
sudo apt install clang lld nasm binutils python3 qemu-system-x86 ovmf
```

MSYS2 MINGW64:

```sh
pacman -Syu
pacman -S --needed make mingw-w64-x86_64-clang mingw-w64-x86_64-lld mingw-w64-x86_64-llvm-tools mingw-w64-x86_64-nasm mingw-w64-x86_64-python
```

Optional, for QEMU:

```sh
pacman -S --needed mingw-w64-x86_64-qemu
```

Build the loader and kernel:

```sh
make
```

In MSYS2 MINGW64, use the MSYS2-specific GNU makefile:

```sh
make -f Makefile.msys2
```

In a Visual Studio Developer Command Prompt, use the `nmake` makefile:

```cmd
nmake /f Mkfile
```

Build the 64 MiB GPT/FAT32 USB image:

```sh
make image
```

MSYS2:

```sh
make -f Makefile.msys2 image
```

Visual Studio Developer Command Prompt:

```cmd
nmake /f Mkfile image
```

Outputs:

```text
build/BOOTX64.EFI
build/KERNEL.ELF
build/JA-os-v4.img
```

Inspect the binaries:

```sh
make inspect
```

MSYS2:

```sh
make -f Makefile.msys2 inspect
```

## Test in QEMU + OVMF

```sh
make run
```

MSYS2 uses QEMU's packaged x86-64 EDK2 firmware by default:

```sh
make -f Makefile.msys2 run
```

The EFI loader briefly prints status through the firmware console. Then the screen is cleared and the standalone kernel draws its own message directly to the framebuffer.

## Flash to USB

**Warning: the target device is completely overwritten. Verify the device name.**

Linux:

```sh
lsblk
sudo dd if=build/JA-os-v4.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

Write to the whole device (`/dev/sdX`), not a partition such as `/dev/sdX1`.

macOS:

```sh
diskutil list
diskutil unmountDisk /dev/diskN
sudo dd if=build/JA-os-v4.img of=/dev/rdiskN bs=1m
diskutil eject /dev/diskN
```

Windows: write `build/JA-os-v4.img` as a raw image with a raw disk imaging tool.

Boot the USB in **UEFI x86-64 mode**. Secure Boot must currently be disabled because this hobby EFI loader is unsigned.

## Disk layout

```text
GPT disk
└── EFI System Partition (FAT32)
    ├── EFI/
    │   └── BOOT/
    │       └── BOOTX64.EFI
    └── KERNEL.ELF
```

## Source layout

```text
loader/loader.c       UEFI loader, filesystem, ELF loading, GOP, memory map, ExitBootServices
loader/uefi.h         minimal UEFI declarations used by this project
loader/elf.h          minimal ELF64 declarations
kernel/entry.asm      NASM entry point, stack switch, ABI bridge, CPU halt
kernel/kernel.c       standalone C kernel + framebuffer renderer
kernel/linker.ld      position-independent ELF kernel layout
include/boot_info.h   handoff structure shared by loader and kernel
tools/make_usb_image.py  builds GPT + FAT32 image without root/mtools
```

## Why the kernel is PIE

The previous fixed-address approach might fail if firmware already occupies the requested physical address. This kernel is linked as an ELF `ET_DYN` image with no runtime relocations. The loader can therefore allocate any suitable physical pages and load it there.

The build deliberately fails if `readelf` finds runtime `R_X86_64_*` relocations because this tiny loader does not contain a dynamic relocator.

## Current limitations

- x86-64 UEFI only
- Secure Boot unsigned
- framebuffer pixels, but no full terminal abstraction
- interrupts are disabled
- no IDT/PIC/APIC setup
- no keyboard input
- no physical/page allocator yet
- no paging ownership changes yet
- no ACPI parsing yet
- no SMP/multicore startup
- no filesystem driver in the kernel

A natural v5 is: parse the memory map into a physical page allocator, install an IDT, enable interrupts, and add keyboard input.
