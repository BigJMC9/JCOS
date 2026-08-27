#!/usr/bin/env python3
"""Build a 64 MiB GPT/FAT32 UEFI USB image containing BOOTX64.EFI + KERNEL.ELF."""
from __future__ import annotations

import binascii
import math
import struct
import sys
import uuid
from pathlib import Path

SECTOR = 512
IMAGE_MIB = 64
TOTAL_SECTORS = IMAGE_MIB * 1024 * 1024 // SECTOR
LAST_LBA = TOTAL_SECTORS - 1
GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128
GPT_ARRAY_SECTORS = GPT_ENTRIES * GPT_ENTRY_SIZE // SECTOR
FIRST_USABLE = 2 + GPT_ARRAY_SECTORS
LAST_USABLE = LAST_LBA - GPT_ARRAY_SECTORS - 1
PART_START = 2048
PART_END = LAST_USABLE
PART_SECTORS = PART_END - PART_START + 1
RESERVED = 32
NUM_FATS = 2
SECTORS_PER_CLUSTER = 1
ROOT_CLUSTER = 2
ESP_TYPE_GUID = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")


def p16(v: int) -> bytes: return struct.pack("<H", v)
def p32(v: int) -> bytes: return struct.pack("<I", v)
def p64(v: int) -> bytes: return struct.pack("<Q", v)


def fat32_size(total_sectors: int) -> int:
    for fat in range(1, total_sectors):
        data = total_sectors - RESERVED - NUM_FATS * fat
        if data <= 0:
            break
        clusters = data // SECTORS_PER_CLUSTER
        if fat * SECTOR // 4 >= clusters + 2:
            return fat
    raise ValueError("could not size FAT32 allocation table")


def short_entry(name11: bytes, attr: int, cluster: int, size: int = 0) -> bytes:
    if len(name11) != 11:
        raise ValueError(f"8.3 name must be exactly 11 bytes: {name11!r}")
    e = bytearray(32)
    e[0:11] = name11
    e[11] = attr
    e[20:22] = p16((cluster >> 16) & 0xFFFF)
    e[26:28] = p16(cluster & 0xFFFF)
    e[28:32] = p32(size)
    return bytes(e)


def dot_entry(double: bool, cluster: int) -> bytes:
    name = bytearray(b"           ")
    name[0] = ord('.')
    if double:
        name[1] = ord('.')
    return short_entry(bytes(name), 0x10, cluster)


def write_cluster(image: bytearray, cluster: int, data: bytes, data_start_lba: int) -> None:
    lba = data_start_lba + (cluster - 2) * SECTORS_PER_CLUSTER
    off = lba * SECTOR
    capacity = SECTORS_PER_CLUSTER * SECTOR
    if len(data) > capacity:
        raise ValueError("cluster payload too large")
    image[off:off + capacity] = data.ljust(capacity, b"\0")


def gpt_header(current_lba: int, backup_lba: int, array_lba: int,
               disk_guid: uuid.UUID, entries_crc: int) -> bytes:
    h = bytearray(SECTOR)
    h[0:8] = b"EFI PART"
    h[8:12] = p32(0x00010000)
    h[12:16] = p32(92)
    h[24:32] = p64(current_lba)
    h[32:40] = p64(backup_lba)
    h[40:48] = p64(FIRST_USABLE)
    h[48:56] = p64(LAST_USABLE)
    h[56:72] = disk_guid.bytes_le
    h[72:80] = p64(array_lba)
    h[80:84] = p32(GPT_ENTRIES)
    h[84:88] = p32(GPT_ENTRY_SIZE)
    h[88:92] = p32(entries_crc)
    h[16:20] = p32(binascii.crc32(h[:92]) & 0xFFFFFFFF)
    return bytes(h)


def build(efi_path: Path, kernel_path: Path, output_path: Path) -> None:
    efi = efi_path.read_bytes()
    kernel = kernel_path.read_bytes()
    if not efi.startswith(b"MZ"):
        raise SystemExit(f"{efi_path} is not a PE/COFF EFI executable")
    if not kernel.startswith(b"\x7fELF"):
        raise SystemExit(f"{kernel_path} is not an ELF executable")

    image = bytearray(TOTAL_SECTORS * SECTOR)

    # Protective MBR.
    mbr = bytearray(SECTOR)
    pe = 446
    mbr[pe + 4] = 0xEE
    mbr[pe + 5:pe + 8] = b"\xFF\xFF\xFF"
    mbr[pe + 8:pe + 12] = p32(1)
    mbr[pe + 12:pe + 16] = p32(min(LAST_LBA, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xAA"
    image[:SECTOR] = mbr

    # GPT with one EFI System Partition.
    entries = bytearray(GPT_ARRAY_SECTORS * SECTOR)
    entry = bytearray(GPT_ENTRY_SIZE)
    entry[0:16] = ESP_TYPE_GUID.bytes_le
    entry[16:32] = uuid.uuid4().bytes_le
    entry[32:40] = p64(PART_START)
    entry[40:48] = p64(PART_END)
    name = "JC_OS_V4".encode("utf-16le")
    entry[56:56 + len(name)] = name
    entries[:GPT_ENTRY_SIZE] = entry
    entries_crc = binascii.crc32(entries) & 0xFFFFFFFF
    disk_guid = uuid.uuid4()

    image[2 * SECTOR:(2 + GPT_ARRAY_SECTORS) * SECTOR] = entries
    image[SECTOR:2 * SECTOR] = gpt_header(1, LAST_LBA, 2, disk_guid, entries_crc)
    backup_array_lba = LAST_LBA - GPT_ARRAY_SECTORS
    image[backup_array_lba * SECTOR:LAST_LBA * SECTOR] = entries
    image[LAST_LBA * SECTOR:(LAST_LBA + 1) * SECTOR] = gpt_header(
        LAST_LBA, 1, backup_array_lba, disk_guid, entries_crc)

    fat_sectors = fat32_size(PART_SECTORS)
    data_start = PART_START + RESERVED + NUM_FATS * fat_sectors
    data_sectors = PART_SECTORS - RESERVED - NUM_FATS * fat_sectors
    cluster_count = data_sectors // SECTORS_PER_CLUSTER
    if cluster_count < 65525:
        raise SystemExit("image is too small to be a standards-compliant FAT32 volume")

    # FAT32 boot sector + backup.
    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = b"MSWIN4.1"
    boot[11:13] = p16(SECTOR)
    boot[13] = SECTORS_PER_CLUSTER
    boot[14:16] = p16(RESERVED)
    boot[16] = NUM_FATS
    boot[21] = 0xF8
    boot[24:26] = p16(63)
    boot[26:28] = p16(255)
    boot[28:32] = p32(PART_START)
    boot[32:36] = p32(PART_SECTORS)
    boot[36:40] = p32(fat_sectors)
    boot[44:48] = p32(ROOT_CLUSTER)
    boot[48:50] = p16(1)
    boot[50:52] = p16(6)
    boot[64] = 0x80
    boot[66] = 0x29
    boot[67:71] = p32(0x5634484F)
    boot[71:82] = b"HELLOOSV4  "
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xAA"
    image[PART_START * SECTOR:(PART_START + 1) * SECTOR] = boot
    image[(PART_START + 6) * SECTOR:(PART_START + 7) * SECTOR] = boot

    fsinfo = bytearray(SECTOR)
    fsinfo[0:4] = p32(0x41615252)
    fsinfo[484:488] = p32(0x61417272)
    fsinfo[488:492] = p32(0xFFFFFFFF)
    fsinfo[492:496] = p32(5)
    fsinfo[508:512] = p32(0xAA550000)
    image[(PART_START + 1) * SECTOR:(PART_START + 2) * SECTOR] = fsinfo
    image[(PART_START + 7) * SECTOR:(PART_START + 8) * SECTOR] = fsinfo

    # Directory clusters: 2=/, 3=/EFI, 4=/EFI/BOOT. Files begin at cluster 5.
    cluster_bytes = SECTOR * SECTORS_PER_CLUSTER
    next_cluster = 5

    def reserve(payload: bytes) -> tuple[int, int]:
        nonlocal next_cluster
        count = max(1, math.ceil(len(payload) / cluster_bytes))
        first = next_cluster
        next_cluster += count
        return first, count

    efi_first, efi_count = reserve(efi)
    kernel_first, kernel_count = reserve(kernel)
    if next_cluster >= cluster_count + 2:
        raise SystemExit("files do not fit in the FAT32 image")

    fat = bytearray(fat_sectors * SECTOR)
    def set_fat(cluster: int, value: int) -> None:
        fat[cluster * 4:cluster * 4 + 4] = p32(value)
    def chain(first: int, count: int) -> None:
        for c in range(first, first + count - 1):
            set_fat(c, c + 1)
        set_fat(first + count - 1, 0x0FFFFFFF)

    set_fat(0, 0x0FFFFFF8)
    set_fat(1, 0x0FFFFFFF)
    set_fat(2, 0x0FFFFFFF)
    set_fat(3, 0x0FFFFFFF)
    set_fat(4, 0x0FFFFFFF)
    chain(efi_first, efi_count)
    chain(kernel_first, kernel_count)

    fat1 = PART_START + RESERVED
    fat2 = fat1 + fat_sectors
    image[fat1 * SECTOR:(fat1 + fat_sectors) * SECTOR] = fat
    image[fat2 * SECTOR:(fat2 + fat_sectors) * SECTOR] = fat

    root = (short_entry(b"EFI        ", 0x10, 3) +
            short_entry(b"KERNEL  ELF", 0x20, kernel_first, len(kernel)))
    efi_dir = dot_entry(False, 3) + dot_entry(True, 2) + short_entry(b"BOOT       ", 0x10, 4)
    boot_dir = (dot_entry(False, 4) + dot_entry(True, 3) +
                short_entry(b"BOOTX64 EFI", 0x20, efi_first, len(efi)))
    write_cluster(image, 2, root, data_start)
    write_cluster(image, 3, efi_dir, data_start)
    write_cluster(image, 4, boot_dir, data_start)

    def write_file(first: int, count: int, payload: bytes) -> None:
        pos = 0
        for c in range(first, first + count):
            chunk = payload[pos:pos + cluster_bytes]
            write_cluster(image, c, chunk, data_start)
            pos += len(chunk)

    write_file(efi_first, efi_count, efi)
    write_file(kernel_first, kernel_count, kernel)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(image)
    print(f"Wrote {output_path} ({IMAGE_MIB} MiB)")
    print(f"ESP: LBA {PART_START}..{PART_END}, FAT32")
    print(f"EFI/BOOT/BOOTX64.EFI: {len(efi)} bytes")
    print(f"KERNEL.ELF: {len(kernel)} bytes")


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(f"usage: {sys.argv[0]} BOOTX64.EFI KERNEL.ELF output.img")
    build(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))

if __name__ == "__main__":
    main()
