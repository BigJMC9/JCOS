#include "uefi.h"
#include "elf.h"

static EFI_GUID LOADED_IMAGE_GUID = {0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static EFI_GUID SIMPLE_FS_GUID    = {0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static EFI_GUID FILE_INFO_GUID    = {0x09576E92,0x6D3F,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static EFI_GUID GOP_GUID          = {0x9042A9DE,0x23DC,0x4A38,{0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};

#ifndef JCOS_FRAMEBUFFER_WIDTH
#define JCOS_FRAMEBUFFER_WIDTH 0U
#endif
#ifndef JCOS_FRAMEBUFFER_HEIGHT
#define JCOS_FRAMEBUFFER_HEIGHT 0U
#endif
#define JCOS_FRAMEBUFFER_AUTO_MAX_WIDTH 1280U
#define JCOS_FRAMEBUFFER_AUTO_MAX_HEIGHT 800U
static EFI_GUID ACPI_20_GUID      = {0x8868E871,0xE4F1,0x11D3,{0xBC,0x22,0x00,0x80,0xC7,0x3C,0x88,0x81}};
static EFI_GUID ACPI_10_GUID      = {0xEB9D2D30,0x2D88,0x11D3,{0x9A,0x16,0x00,0x90,0x27,0x3F,0xC1,0x4D}};

static CHAR16 MSG_START[] = {'H','e','l','l','o',' ','O','S',' ','v','5',' ','U','E','F','I',' ','l','o','a','d','e','r','\r','\n',0};
static CHAR16 MSG_LOAD[]  = {'L','o','a','d','i','n','g',' ','K','E','R','N','E','L','.','E','L','F','.','.','.','\r','\n',0};
static CHAR16 MSG_EXIT[]  = {'E','x','i','t','i','n','g',' ','U','E','F','I',' ','b','o','o','t',' ','s','e','r','v','i','c','e','s','.','.','.','\r','\n',0};
static CHAR16 MSG_NO_ACPI[] = {'W','a','r','n','i','n','g',':',' ','A','C','P','I',' ','R','S','D','P',' ','n','o','t',' ','f','o','u','n','d','.','\r','\n',0};
static CHAR16 MSG_FAIL[]  = {'B','o','o','t',' ','f','a','i','l','e','d',':',' ',0};
static CHAR16 KERNEL_PATH[] = {'\\','K','E','R','N','E','L','.','E','L','F',0};
static CHAR16 ROOTFS_PATH[] = {'\\','R','O','O','T','F','S','.','T','A','R',0};
static CHAR16 MSG_ROOTFS[] = {
    'L','o','a','d','i','n','g',' ',
    'R','O','O','T','F','S','.','T','A','R',
    '.','.','.',
    '\r','\n',0
};

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table);
/* Force one PE base relocation so firmware may freely relocate BOOTX64.EFI. */
__attribute__((used)) static void *g_relocation_anchor = (void *)&efi_main;

static EFI_SYSTEM_TABLE *g_st;
static EFI_BOOT_SERVICES *g_bs;
static BootInfo g_boot_info;

static void text(CHAR16 *s) {
    if (g_st && g_st->ConOut) g_st->ConOut->OutputString(g_st->ConOut, s);
}

static void status_hex(EFI_STATUS status) {
    static const CHAR16 digits[] = {'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};
    CHAR16 buf[21];
    buf[0] = '0'; buf[1] = 'x';
    for (u32 i = 0; i < 16; ++i) {
        u32 shift = (15 - i) * 4;
        buf[2 + i] = digits[(status >> shift) & 0xF];
    }
    buf[18] = '\r'; buf[19] = '\n'; buf[20] = 0;
    text(buf);
}

static EFI_STATUS fail(EFI_STATUS status) {
    text(MSG_FAIL);
    status_hex(status);
    return status;
}

static int guid_equal(const EFI_GUID *a, const EFI_GUID *b) {
    if (a->Data1 != b->Data1 || a->Data2 != b->Data2 || a->Data3 != b->Data3) return 0;
    for (u32 i = 0; i < 8; ++i) if (a->Data4[i] != b->Data4[i]) return 0;
    return 1;
}

static u64 find_acpi_rsdp(void) {
    if (!g_st || !g_st->ConfigurationTable) return 0;
    u64 acpi10 = 0;
    for (UINTN i = 0; i < g_st->NumberOfTableEntries; ++i) {
        EFI_CONFIGURATION_TABLE *entry = &g_st->ConfigurationTable[i];
        if (guid_equal(&entry->VendorGuid, &ACPI_20_GUID)) return (u64)(UINTN)entry->VendorTable;
        if (guid_equal(&entry->VendorGuid, &ACPI_10_GUID)) acpi10 = (u64)(UINTN)entry->VendorTable;
    }
    return acpi10;
}

static void copy_bytes(void *dst, const void *src, u64 count) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < count; ++i) d[i] = s[i];
}

static void zero_bytes(void *dst, u64 count) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < count; ++i) d[i] = 0;
}

static u64 align_down(u64 value, u64 alignment) { return value & ~(alignment - 1); }
static u64 align_up(u64 value, u64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }

typedef struct {
    u64 base;
    u64 size;
    u64 entry;
} LoadedKernel;

static EFI_STATUS read_entire_file(EFI_HANDLE image, CHAR16 *path, void **out_buffer, u64 *out_size) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_FILE_PROTOCOL *file = 0;
    void *info = 0;
    void *data = 0;

    status = g_bs->HandleProtocol(image, &LOADED_IMAGE_GUID, (void **)&loaded);
    if (EFI_ERROR(status)) return status;
    status = g_bs->HandleProtocol(loaded->DeviceHandle, &SIMPLE_FS_GUID, (void **)&fs);
    if (EFI_ERROR(status)) return status;
    status = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(status)) return status;
    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) { root->Close(root); return status; }

    UINTN info_size = 0;
    status = file->GetInfo(file, &FILE_INFO_GUID, &info_size, 0);
    if (status != EFI_BUFFER_TOO_SMALL) { file->Close(file); root->Close(root); return status; }
    status = g_bs->AllocatePool(EfiLoaderData, info_size, &info);
    if (EFI_ERROR(status)) { file->Close(file); root->Close(root); return status; }
    status = file->GetInfo(file, &FILE_INFO_GUID, &info_size, info);
    if (EFI_ERROR(status)) { g_bs->FreePool(info); file->Close(file); root->Close(root); return status; }

    u64 size = ((EFI_FILE_INFO_PREFIX *)info)->FileSize;
    g_bs->FreePool(info);
    if (size == 0) { file->Close(file); root->Close(root); return EFI_UNSUPPORTED; }

    status = g_bs->AllocatePool(EfiLoaderData, (UINTN)size, &data);
    if (EFI_ERROR(status)) { file->Close(file); root->Close(root); return status; }
    UINTN read_size = (UINTN)size;
    status = file->Read(file, &read_size, data);
    file->Close(file);
    root->Close(root);
    if (EFI_ERROR(status) || read_size != size) {
        g_bs->FreePool(data);
        return EFI_UNSUPPORTED;
    }

    *out_buffer = data;
    *out_size = size;
    return EFI_SUCCESS;
}

static EFI_STATUS load_kernel_elf(EFI_HANDLE image, LoadedKernel *out) {
    enum { MAX_WRITABLE_RANGES = 16 };
    typedef struct { u64 low, high; } PageRange;

    void *file = 0;
    u64 file_size = 0;
    EFI_STATUS status = read_entire_file(image, KERNEL_PATH, &file, &file_size);
    if (EFI_ERROR(status)) return status;
    if (file_size < sizeof(Elf64_Ehdr)) { g_bs->FreePool(file); return EFI_UNSUPPORTED;}
    Elf64_Ehdr *header = (Elf64_Ehdr *)file;
    if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E' || header->e_ident[2] != 'L' || header->e_ident[3] != 'F' ||
        header->e_ident[4] != ELFCLASS64 || header->e_ident[5] != ELFDATA2LSB || header->e_type != ET_DYN ||
        header->e_machine != EM_X86_64 || header->e_phentsize != sizeof(Elf64_Phdr) || header->e_phnum == 0) {
        g_bs->FreePool(file);
        return EFI_UNSUPPORTED;
    }
    if (header->e_phoff > file_size || (u64)header->e_phnum * sizeof(Elf64_Phdr) > file_size - header->e_phoff) {
        g_bs->FreePool(file);
        return EFI_UNSUPPORTED;
    }

    Elf64_Phdr *program = (Elf64_Phdr *)((u8 *)file + header->e_phoff);
    PageRange writable[MAX_WRITABLE_RANGES];
    u32 writable_count = 0;
    u64 minimum = ~0ULL;
    u64 maximum = 0;
    u32 load_count = 0;
    int entry_is_executable = 0;

    for (u16 i = 0; i < header->e_phnum; ++i) {
        if (program[i].p_type != PT_LOAD || program[i].p_memsz == 0) continue;
        if (program[i].p_filesz > program[i].p_memsz || program[i].p_offset > file_size ||
            program[i].p_filesz > file_size - program[i].p_offset ||
            program[i].p_vaddr + program[i].p_memsz < program[i].p_vaddr) {
            g_bs->FreePool(file);
            return EFI_UNSUPPORTED;
        }
        u64 low = align_down(program[i].p_vaddr, 4096);
        u64 high = align_up(program[i].p_vaddr + program[i].p_memsz, 4096);
        if (low < minimum) minimum = low;
        if (high > maximum) maximum = high;
        if ((program[i].p_flags & PF_X) && header->e_entry >= program[i].p_vaddr &&
            header->e_entry < program[i].p_vaddr + program[i].p_memsz)
            entry_is_executable = 1;
        if (program[i].p_flags & PF_W) {
            if (writable_count == MAX_WRITABLE_RANGES) {
                g_bs->FreePool(file);
                return EFI_UNSUPPORTED;
            }
            writable[writable_count].low = low;
            writable[writable_count].high = high;
            ++writable_count;
        }
        ++load_count;
    }
    if (!load_count || maximum <= minimum || !entry_is_executable) {
        g_bs->FreePool(file);
        return EFI_UNSUPPORTED;
    }

    /* A page cannot be both executable-only and writable in this simple loader.
       The linker script deliberately starts the data segment on a new page. */
    for (u16 i = 0; i < header->e_phnum; ++i) {
        if (program[i].p_type != PT_LOAD || program[i].p_memsz == 0 || (program[i].p_flags & PF_W)) continue;
        u64 low = align_down(program[i].p_vaddr, 4096);
        u64 high = align_up(program[i].p_vaddr + program[i].p_memsz, 4096);
        for (u32 j = 0; j < writable_count; ++j) {
            if (low < writable[j].high && writable[j].low < high) {
                g_bs->FreePool(file);
                return EFI_UNSUPPORTED;
            }
        }
    }

    /* Sort and merge writable ranges before retyping their pages. */
    for (u32 i = 1; i < writable_count; ++i) {
        PageRange value = writable[i];
        u32 j = i;
        while (j && writable[j - 1].low > value.low) {
            writable[j] = writable[j - 1];
            --j;
        }
        writable[j] = value;
    }
    u32 merged_count = 0;
    for (u32 i = 0; i < writable_count; ++i) {
        if (merged_count && writable[i].low <= writable[merged_count - 1].high) {
            if (writable[i].high > writable[merged_count - 1].high)
                writable[merged_count - 1].high = writable[i].high;
        } else {
            writable[merged_count++] = writable[i];
        }
    }
    writable_count = merged_count;

    u64 image_size = maximum - minimum;
    UINTN pages = (UINTN)(image_size / 4096);
    EFI_PHYSICAL_ADDRESS physical = 0;
    status = g_bs->AllocatePages(AllocateAnyPages, EfiLoaderCode, pages, &physical);
    if (EFI_ERROR(status)) { g_bs->FreePool(file); return status; }

    /* Keep executable pages classified as LoaderCode and reclassify writable
       PT_LOAD pages as LoaderData. This matches how UEFI loads PE code/data and
       avoids relying on an implementation granting every page RWX. */
    for (u32 i = 0; i < writable_count; ++i) {
        EFI_PHYSICAL_ADDRESS address = physical + (writable[i].low - minimum);
        UINTN range_pages = (UINTN)((writable[i].high - writable[i].low) / 4096);
        status = g_bs->FreePages(address, range_pages);
        if (EFI_ERROR(status)) { g_bs->FreePool(file); return status; }
        EFI_PHYSICAL_ADDRESS requested = address;
        status = g_bs->AllocatePages(AllocateAddress, EfiLoaderData, range_pages, &requested);
        if (EFI_ERROR(status) || requested != address) {
            g_bs->FreePool(file);
            return EFI_ERROR(status) ? status : EFI_UNSUPPORTED;
        }
    }

    zero_bytes((void *)(UINTN)physical, image_size);
    u64 bias = physical - minimum;
    for (u16 i = 0; i < header->e_phnum; ++i) {
        if (program[i].p_type != PT_LOAD || program[i].p_memsz == 0) continue;
        copy_bytes((void *)(UINTN)(bias + program[i].p_vaddr),
                   (u8 *)file + program[i].p_offset,
                   program[i].p_filesz);
    }

    out->base = physical;
    out->size = image_size;
    out->entry = bias + header->e_entry;
    g_bs->FreePool(file);
    return EFI_SUCCESS;
}

static EFI_STATUS load_rootfs(EFI_HANDLE image, BootInfo *boot) {
    void *data = 0;
    u64 size = 0;

    EFI_STATUS status = read_entire_file(
        image,
        ROOTFS_PATH,
        &data,
        &size
    );

    if (EFI_ERROR(status))
        return status;

    boot->initrd_base = (u64)(UINTN)data;
    boot->initrd_size = size;

    return EFI_SUCCESS;
}

static int framebuffer_mode_supported(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info) {
    return info && info->HorizontalResolution && info->VerticalResolution &&
        info->PixelsPerScanLine >= info->HorizontalResolution &&
        info->PixelFormat <= 2U &&
        (info->PixelFormat != 2U ||
         (info->PixelInformation.RedMask | info->PixelInformation.GreenMask |
          info->PixelInformation.BlueMask));
}

static EFI_STATUS select_framebuffer_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop) {
    if (!gop || !gop->Mode || !gop->QueryMode || !gop->SetMode) return EFI_UNSUPPORTED;
    u32 selected = gop->Mode->Mode;
    u64 selected_area = 0ULL;
    int exact = 0;
    for (u32 mode = 0; mode < gop->Mode->MaxMode; ++mode) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = 0;
        UINTN info_size = 0;
        EFI_STATUS status = gop->QueryMode(gop, mode, &info_size, &info);
        if (EFI_ERROR(status) || !framebuffer_mode_supported(info)) {
            if (info) g_bs->FreePool(info);
            continue;
        }
        int requested = JCOS_FRAMEBUFFER_WIDTH && JCOS_FRAMEBUFFER_HEIGHT;
        int is_exact = requested &&
            info->HorizontalResolution == JCOS_FRAMEBUFFER_WIDTH &&
            info->VerticalResolution == JCOS_FRAMEBUFFER_HEIGHT;
        u64 area = (u64)info->HorizontalResolution * info->VerticalResolution;
        int automatic_candidate = !requested &&
            info->HorizontalResolution <= JCOS_FRAMEBUFFER_AUTO_MAX_WIDTH &&
            info->VerticalResolution <= JCOS_FRAMEBUFFER_AUTO_MAX_HEIGHT;
        if (is_exact || (!exact && automatic_candidate && area > selected_area)) {
            selected = mode;
            selected_area = area;
            if (is_exact) exact = 1;
        }
        g_bs->FreePool(info);
    }
    if (selected == gop->Mode->Mode) return EFI_SUCCESS;
    return gop->SetMode(gop, selected);
}

static EFI_STATUS get_framebuffer(BootInfo *boot) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_STATUS status = g_bs->LocateProtocol(&GOP_GUID, 0, (void **)&gop);
    if (EFI_ERROR(status)) return status;
    status = select_framebuffer_mode(gop);
    if (EFI_ERROR(status)) return status;
    if (!gop || !gop->Mode || !gop->Mode->Info || !gop->Mode->FrameBufferBase || gop->Mode->Info->PixelFormat == 3) {
        return EFI_UNSUPPORTED;
    }

    boot->framebuffer_base = gop->Mode->FrameBufferBase;
    boot->framebuffer_size = gop->Mode->FrameBufferSize;
    boot->framebuffer_width = gop->Mode->Info->HorizontalResolution;
    boot->framebuffer_height = gop->Mode->Info->VerticalResolution;
    boot->framebuffer_pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
    boot->framebuffer_pixel_format = gop->Mode->Info->PixelFormat;
    boot->framebuffer_red_mask = gop->Mode->Info->PixelInformation.RedMask;
    boot->framebuffer_green_mask = gop->Mode->Info->PixelInformation.GreenMask;
    boot->framebuffer_blue_mask = gop->Mode->Info->PixelInformation.BlueMask;
    boot->framebuffer_reserved_mask = gop->Mode->Info->PixelInformation.ReservedMask;
    return EFI_SUCCESS;
}

static EFI_STATUS allocate_kernel_stack(BootInfo *boot) {
    const UINTN pages = 16; /* 64 KiB */
    EFI_PHYSICAL_ADDRESS base = 0;
    EFI_STATUS status = g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &base);
    if (EFI_ERROR(status)) return status;
    boot->kernel_stack_base = base;
    boot->kernel_stack_size = pages * 4096;
    boot->kernel_stack_top = align_down(base + pages * 4096, 16);
    return EFI_SUCCESS;
}

static EFI_STATUS exit_boot_services(EFI_HANDLE image, BootInfo *boot) {
    EFI_STATUS status;
    UINTN needed = 0, key = 0, descriptor_size = 0;
    u32 descriptor_version = 0;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN capacity = 0;

    status = g_bs->GetMemoryMap(&needed, 0, &key, &descriptor_size, &descriptor_version);
    if (status != EFI_BUFFER_TOO_SMALL || descriptor_size == 0) return status;

    capacity = needed + descriptor_size * 32;
    status = g_bs->AllocatePool(EfiLoaderData, capacity, (void **)&map);
    if (EFI_ERROR(status)) return status;
    g_bs->SetWatchdogTimer(0, 0, 0, 0);

    for (u32 attempt = 0; attempt < 8; ++attempt) {
        UINTN map_size = capacity;
        status = g_bs->GetMemoryMap(&map_size, map, &key, &descriptor_size, &descriptor_version);
        if (status == EFI_BUFFER_TOO_SMALL) {
            g_bs->FreePool(map);
            capacity = map_size + descriptor_size * 32;
            status = g_bs->AllocatePool(EfiLoaderData, capacity, (void **)&map);
            if (EFI_ERROR(status)) return status;
            continue;
        }
        if (EFI_ERROR(status)) return status;

        boot->memory_map = (u64)(UINTN)map;
        boot->memory_map_size = map_size;
        boot->memory_map_descriptor_size = descriptor_size;
        boot->memory_map_descriptor_version = descriptor_version;

        /* No Boot Services calls may occur between this memory map and ExitBootServices. */
        status = g_bs->ExitBootServices(image, key);
        if (status == EFI_SUCCESS) return EFI_SUCCESS;
        if (status != EFI_INVALID_PARAMETER) return status;
    }
    return EFI_INVALID_PARAMETER;
}

typedef void (EFIAPI *KERNEL_ENTRY)(BootInfo *);

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    g_st = system_table;
    g_bs = system_table->BootServices;
    if (system_table->ConOut) system_table->ConOut->ClearScreen(system_table->ConOut);
    text(MSG_START);
    text(MSG_LOAD);

    LoadedKernel kernel;
    EFI_STATUS status = load_kernel_elf(image, &kernel);
    if (EFI_ERROR(status)) return fail(status);

    g_boot_info.magic = BOOT_INFO_MAGIC;
    g_boot_info.version = BOOT_INFO_VERSION;
    g_boot_info.size = sizeof(BootInfo);
    g_boot_info.kernel_base = kernel.base;
    g_boot_info.kernel_size = kernel.size;
    g_boot_info.kernel_entry = kernel.entry;

    text(MSG_ROOTFS);
    status = load_rootfs(image, &g_boot_info);
    if (EFI_ERROR(status)) return fail(status);

    g_boot_info.acpi_rsdp = find_acpi_rsdp();
    if (!g_boot_info.acpi_rsdp) text(MSG_NO_ACPI);

    status = get_framebuffer(&g_boot_info);
    if (EFI_ERROR(status)) return fail(status);
    status = allocate_kernel_stack(&g_boot_info);
    if (EFI_ERROR(status)) return fail(status);

    text(MSG_EXIT);
    status = exit_boot_services(image, &g_boot_info);
    if (EFI_ERROR(status)) return fail(status);

    ((KERNEL_ENTRY)(UINTN)kernel.entry)(&g_boot_info);
    for (;;) __asm__ volatile ("hlt");
}
