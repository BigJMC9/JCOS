#!/usr/bin/env python3
"""Structural validator for the JA OS v5 UEFI disk image."""
from __future__ import annotations

import argparse
import binascii
import hashlib
import struct
import sys
import uuid
from dataclasses import dataclass
from pathlib import Path

SECTOR = 512
ESP_GUID = uuid.UUID("c12a7328-f81f-11d2-ba4b-00a0c93ec93b")


class ValidationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValidationError(message)


def u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def crc32(data: bytes) -> int:
    return binascii.crc32(data) & 0xFFFFFFFF


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True)
class GptHeader:
    current_lba: int
    backup_lba: int
    first_usable: int
    last_usable: int
    disk_guid: uuid.UUID
    entries_lba: int
    entry_count: int
    entry_size: int
    entries_crc: int


def parse_gpt_header(image: bytes, lba: int) -> GptHeader:
    block = image[lba * SECTOR:(lba + 1) * SECTOR]
    require(len(block) == SECTOR, f"GPT header LBA {lba} lies outside image")
    require(block[:8] == b"EFI PART", f"missing GPT signature at LBA {lba}")
    header_size = u32(block, 12)
    require(92 <= header_size <= SECTOR, f"invalid GPT header size at LBA {lba}")
    expected_crc = u32(block, 16)
    check = bytearray(block[:header_size])
    check[16:20] = b"\0\0\0\0"
    require(crc32(check) == expected_crc, f"GPT header CRC mismatch at LBA {lba}")
    return GptHeader(
        current_lba=u64(block, 24),
        backup_lba=u64(block, 32),
        first_usable=u64(block, 40),
        last_usable=u64(block, 48),
        disk_guid=uuid.UUID(bytes_le=block[56:72]),
        entries_lba=u64(block, 72),
        entry_count=u32(block, 80),
        entry_size=u32(block, 84),
        entries_crc=u32(block, 88),
    )


def read_gpt_entries(image: bytes, header: GptHeader) -> bytes:
    size = header.entry_count * header.entry_size
    start = header.entries_lba * SECTOR
    entries = image[start:start + size]
    require(len(entries) == size, "GPT entry array lies outside image")
    require(crc32(entries) == header.entries_crc, "GPT entry-array CRC mismatch")
    return entries


@dataclass(frozen=True)
class FatEntry:
    name: bytes
    attributes: int
    first_cluster: int
    size: int


class Fat32:
    def __init__(self, image: bytes, partition_lba: int, partition_sectors: int):
        self.image = image
        self.partition_lba = partition_lba
        self.partition_sectors = partition_sectors
        boot = image[partition_lba * SECTOR:(partition_lba + 1) * SECTOR]
        require(len(boot) == SECTOR and boot[510:512] == b"\x55\xaa", "invalid FAT32 boot sector")
        self.bytes_per_sector = u16(boot, 11)
        self.sectors_per_cluster = boot[13]
        self.reserved = u16(boot, 14)
        self.fat_count = boot[16]
        self.total_sectors = u32(boot, 32)
        self.fat_sectors = u32(boot, 36)
        self.root_cluster = u32(boot, 44)
        self.fsinfo_sector = u16(boot, 48)
        self.backup_sector = u16(boot, 50)
        require(self.bytes_per_sector == SECTOR, "unexpected FAT bytes-per-sector")
        require(self.sectors_per_cluster > 0 and self.sectors_per_cluster & (self.sectors_per_cluster - 1) == 0,
                "invalid FAT sectors-per-cluster")
        require(self.reserved >= 1 and self.fat_count in (1, 2), "invalid FAT layout")
        require(self.total_sectors == partition_sectors, "FAT size does not match GPT partition")
        require(self.fat_sectors > 0 and self.root_cluster >= 2, "invalid FAT32 metadata")
        require(boot[82:90] == b"FAT32   ", "volume does not identify itself as FAT32")

        fsinfo = image[(partition_lba + self.fsinfo_sector) * SECTOR:
                       (partition_lba + self.fsinfo_sector + 1) * SECTOR]
        require(u32(fsinfo, 0) == 0x41615252 and u32(fsinfo, 484) == 0x61417272 and
                u32(fsinfo, 508) == 0xAA550000, "invalid FAT32 FSInfo signatures")
        backup = image[(partition_lba + self.backup_sector) * SECTOR:
                       (partition_lba + self.backup_sector + 1) * SECTOR]
        require(backup == boot, "FAT32 backup boot sector differs from primary")

        self.fat_lba = partition_lba + self.reserved
        self.data_lba = self.fat_lba + self.fat_count * self.fat_sectors
        data_sectors = partition_sectors - self.reserved - self.fat_count * self.fat_sectors
        self.cluster_count = data_sectors // self.sectors_per_cluster
        require(self.cluster_count >= 65525, "volume is not large enough to be FAT32")
        fat_bytes = self.fat_sectors * SECTOR
        self.fat = image[self.fat_lba * SECTOR:self.fat_lba * SECTOR + fat_bytes]
        require(len(self.fat) == fat_bytes, "FAT lies outside image")
        if self.fat_count == 2:
            second_lba = self.fat_lba + self.fat_sectors
            second = image[second_lba * SECTOR:second_lba * SECTOR + fat_bytes]
            require(second == self.fat, "FAT copies differ")

    @property
    def cluster_bytes(self) -> int:
        return self.sectors_per_cluster * SECTOR

    def fat_value(self, cluster: int) -> int:
        require(0 <= cluster < len(self.fat) // 4, "FAT cluster index out of range")
        return u32(self.fat, cluster * 4) & 0x0FFFFFFF

    def cluster(self, cluster: int) -> bytes:
        require(2 <= cluster < self.cluster_count + 2, f"invalid cluster {cluster}")
        lba = self.data_lba + (cluster - 2) * self.sectors_per_cluster
        data = self.image[lba * SECTOR:(lba + self.sectors_per_cluster) * SECTOR]
        require(len(data) == self.cluster_bytes, "cluster lies outside image")
        return data

    def chain(self, first: int) -> list[int]:
        require(first >= 2, "invalid first cluster")
        result: list[int] = []
        seen: set[int] = set()
        current = first
        while True:
            require(current not in seen, "cycle in FAT chain")
            require(len(result) <= self.cluster_count, "FAT chain is unreasonably long")
            seen.add(current)
            result.append(current)
            value = self.fat_value(current)
            if value >= 0x0FFFFFF8:
                return result
            require(value >= 2 and value != 0x0FFFFFF7, f"bad FAT chain value 0x{value:x}")
            current = value

    def data(self, first: int, size: int | None = None) -> bytes:
        payload = b"".join(self.cluster(c) for c in self.chain(first))
        if size is None:
            return payload
        require(size <= len(payload), "file size exceeds FAT chain capacity")
        return payload[:size]

    def directory(self, first: int) -> dict[bytes, FatEntry]:
        entries: dict[bytes, FatEntry] = {}
        raw = self.data(first)
        for offset in range(0, len(raw), 32):
            item = raw[offset:offset + 32]
            if len(item) < 32 or item[0] == 0x00:
                break
            if item[0] == 0xE5 or item[11] == 0x0F or item[11] & 0x08:
                continue
            name = bytes(item[:11])
            cluster = u16(item, 26) | (u16(item, 20) << 16)
            entries[name] = FatEntry(name, item[11], cluster, u32(item, 28))
        return entries


def validate_pe(efi: bytes) -> None:
    require(efi[:2] == b"MZ", "EFI loader lacks MZ signature")
    pe_offset = u32(efi, 0x3C)
    require(pe_offset + 24 <= len(efi) and efi[pe_offset:pe_offset + 4] == b"PE\0\0", "invalid PE header")
    coff = pe_offset + 4
    require(u16(efi, coff) == 0x8664, "EFI loader is not x86-64")
    optional_size = u16(efi, coff + 16)
    optional = coff + 20
    require(optional + optional_size <= len(efi), "truncated PE optional header")
    require(u16(efi, optional) == 0x20B, "EFI loader is not PE32+")
    require(u16(efi, optional + 68) == 10, "PE subsystem is not EFI application")
    directory_count = u32(efi, optional + 108)
    require(directory_count > 5 and optional_size >= 112 + directory_count * 8,
            "PE data-directory table is invalid")
    reloc_rva = u32(efi, optional + 112 + 5 * 8)
    reloc_size = u32(efi, optional + 112 + 5 * 8 + 4)
    require(reloc_rva != 0 and reloc_size != 0, "EFI loader has no base relocation directory")


def validate_elf(kernel: bytes) -> None:
    require(kernel[:4] == b"\x7fELF" and kernel[4] == 2 and kernel[5] == 1,
            "kernel is not little-endian ELF64")
    require(u16(kernel, 16) == 3, "kernel is not ET_DYN/PIE")
    require(u16(kernel, 18) == 62, "kernel is not x86-64")
    entry = u64(kernel, 24)
    phoff = u64(kernel, 32)
    shoff = u64(kernel, 40)
    phentsize = u16(kernel, 54)
    phnum = u16(kernel, 56)
    shentsize = u16(kernel, 58)
    shnum = u16(kernel, 60)
    require(phentsize == 56 and phnum > 0, "invalid kernel program-header table")
    require(phoff + phentsize * phnum <= len(kernel), "kernel program headers are truncated")

    loads: list[tuple[int, int, int]] = []
    entry_executable = False
    for index in range(phnum):
        off = phoff + index * phentsize
        p_type = u32(kernel, off)
        p_flags = u32(kernel, off + 4)
        p_offset = u64(kernel, off + 8)
        p_vaddr = u64(kernel, off + 16)
        p_filesz = u64(kernel, off + 32)
        p_memsz = u64(kernel, off + 40)
        if p_type != 1:
            continue
        require(p_filesz <= p_memsz and p_offset + p_filesz <= len(kernel), "invalid PT_LOAD segment")
        loads.append((p_vaddr, p_vaddr + p_memsz, p_flags))
        if p_flags & 1 and p_vaddr <= entry < p_vaddr + p_memsz:
            entry_executable = True
    require(loads and entry_executable, "kernel entry is not inside an executable PT_LOAD segment")
    for i, a in enumerate(loads):
        for b in loads[i + 1:]:
            a_low, a_high = a[0] & ~0xFFF, (a[1] + 0xFFF) & ~0xFFF
            b_low, b_high = b[0] & ~0xFFF, (b[1] + 0xFFF) & ~0xFFF
            if a_low < b_high and b_low < a_high:
                require(bool(a[2] & 2) == bool(b[2] & 2), "writable and non-writable PT_LOAD segments share a page")

    if shnum:
        require(shentsize == 64 and shoff + shentsize * shnum <= len(kernel), "invalid section table")
        for index in range(shnum):
            off = shoff + index * shentsize
            section_type = u32(kernel, off + 4)
            section_size = u64(kernel, off + 32)
            if section_type in (4, 9):
                require(section_size == 0, "kernel contains runtime relocation records")


def validate(image_path: Path, efi_path: Path, kernel_path: Path, rootfs_path: Path) -> None:
    image = image_path.read_bytes()
    efi = efi_path.read_bytes()
    kernel = kernel_path.read_bytes()
    rootfs = rootfs_path.read_bytes()
    require(len(image) >= 34 * SECTOR and len(image) % SECTOR == 0, "image size is invalid")
    last_lba = len(image) // SECTOR - 1

    mbr = image[:SECTOR]
    require(mbr[510:512] == b"\x55\xaa", "protective MBR signature is missing")
    require(mbr[446 + 4] == 0xEE and u32(mbr, 446 + 8) == 1, "protective MBR entry is invalid")

    primary = parse_gpt_header(image, 1)
    backup = parse_gpt_header(image, last_lba)
    require(primary.current_lba == 1 and primary.backup_lba == last_lba, "primary GPT LBA pointers are invalid")
    require(backup.current_lba == last_lba and backup.backup_lba == 1, "backup GPT LBA pointers are invalid")
    require(primary.disk_guid == backup.disk_guid, "primary and backup GPT disk GUIDs differ")
    primary_entries = read_gpt_entries(image, primary)
    backup_entries = read_gpt_entries(image, backup)
    require(primary_entries == backup_entries, "primary and backup GPT entry arrays differ")

    entry = primary_entries[:primary.entry_size]
    require(uuid.UUID(bytes_le=entry[:16]) == ESP_GUID, "first GPT partition is not an EFI System Partition")
    first_lba = u64(entry, 32)
    final_lba = u64(entry, 40)
    require(primary.first_usable <= first_lba <= final_lba <= primary.last_usable,
            "EFI System Partition lies outside GPT usable range")

    fat = Fat32(image, first_lba, final_lba - first_lba + 1)
    root = fat.directory(fat.root_cluster)
    require(b"EFI        " in root and root[b"EFI        "].attributes & 0x10, "missing /EFI directory")
    require(b"KERNEL  ELF" in root and not root[b"KERNEL  ELF"].attributes & 0x10, "missing /KERNEL.ELF")
    require(b"ROOTFS  TAR" in root and not root[b"ROOTFS  TAR"].attributes & 0x10, "missing /ROOTFS.TAR")
    efi_dir = fat.directory(root[b"EFI        "].first_cluster)
    require(b"BOOT       " in efi_dir and efi_dir[b"BOOT       "].attributes & 0x10, "missing /EFI/BOOT directory")
    boot_dir = fat.directory(efi_dir[b"BOOT       "].first_cluster)
    require(b"BOOTX64 EFI" in boot_dir and not boot_dir[b"BOOTX64 EFI"].attributes & 0x10,
            "missing /EFI/BOOT/BOOTX64.EFI")

    embedded_kernel = fat.data(root[b"KERNEL  ELF"].first_cluster, root[b"KERNEL  ELF"].size)
    embedded_efi = fat.data(boot_dir[b"BOOTX64 EFI"].first_cluster, boot_dir[b"BOOTX64 EFI"].size)
    embedded_rootfs = fat.data(root[b"ROOTFS  TAR"].first_cluster, root[b"ROOTFS  TAR"].size)
    require(embedded_kernel == kernel, "embedded KERNEL.ELF differs from build artifact")
    require(embedded_efi == efi, "embedded BOOTX64.EFI differs from build artifact")
    require(embedded_rootfs == rootfs, "embedded ROOTFS.TAR differs from build artifact")
    validate_pe(efi)
    validate_elf(kernel)

    print("JA OS v5 image validation: PASS")
    print(f"  image:  {len(image):,} bytes  sha256={sha256(image)}")
    print(f"  EFI:    {len(efi):,} bytes  sha256={sha256(efi)}")
    print(f"  kernel: {len(kernel):,} bytes  sha256={sha256(kernel)}")
    print(f"  rootfs: {len(rootfs):,} bytes  "f"sha256={sha256(rootfs)}")
    print(f"  ESP:    LBA {first_lba}..{final_lba} ({final_lba - first_lba + 1:,} sectors)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("efi", type=Path)
    parser.add_argument("kernel", type=Path)
    parser.add_argument("rootfs", type=Path)
    args = parser.parse_args()
    try:
        validate(args.image, args.efi, args.kernel, args.rootfs)
    except (OSError, ValidationError, struct.error, ValueError) as exc:
        print(f"validation failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
