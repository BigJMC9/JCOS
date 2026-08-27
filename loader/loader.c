#include "uefi.h"
#include "elf.h"

static EFI_GUID LOADED_IMAGE_GUID = {0x5B1B31A1,0x9562,0x11D2,{0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static EFI_GUID SIMPLE_FS_GUID    = {0x964E5B22,0x6459,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static EFI_GUID FILE_INFO_GUID    = {0x09576E92,0x6D3F,0x11D2,{0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B}};
static EFI_GUID GOP_GUID          = {0x9042A9DE,0x23DC,0x4A38,{0x96,0xFB,0x7A,0xDE,0xD0,0x80,0x51,0x6A}};

static CHAR16 MSG_START[] = {'H','e','l','l','o',' ','O','S',' ','v','4',' ','U','E','F','I',' ','l','o','a','d','e','r','\r','\n',0};
static CHAR16 MSG_LOAD[]  = {'L','o','a','d','i','n','g',' ','K','E','R','N','E','L','.','E','L','F','.','.','.','\r','\n',0};
static CHAR16 MSG_EXIT[]  = {'E','x','i','t','i','n','g',' ','U','E','F','I',' ','b','o','o','t',' ','s','e','r','v','i','c','e','s','.','.','.','\r','\n',0};
static CHAR16 MSG_FAIL[]  = {'B','o','o','t',' ','f','a','i','l','e','d',':',' ',0};
static CHAR16 KERNEL_PATH[] = {'\\','K','E','R','N','E','L','.','E','L','F',0};

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

static EFI_STATUS fail(EFI_STATUS s) {
    text(MSG_FAIL);
    status_hex(s);
    return s;
}

static void copy_bytes(void *dst, const void *src, u64 n) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (u64 i = 0; i < n; ++i) d[i] = s[i];
}

static void zero_bytes(void *dst, u64 n) {
    u8 *d = (u8 *)dst;
    for (u64 i = 0; i < n; ++i) d[i] = 0;
}

static u64 align_down(u64 v, u64 a) { return v & ~(a - 1); }
static u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

typedef struct {
    u64 base;
    u64 size;
    u64 entry;
} LoadedKernel;

static EFI_STATUS read_entire_file(EFI_HANDLE image, void **out_buffer, u64 *out_size) {
    EFI_STATUS st;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_FILE_PROTOCOL *file = 0;
    void *info = 0;
    void *data = 0;

    st = g_bs->HandleProtocol(image, &LOADED_IMAGE_GUID, (void **)&loaded);
    if (EFI_ERROR(st)) return st;
    st = g_bs->HandleProtocol(loaded->DeviceHandle, &SIMPLE_FS_GUID, (void **)&fs);
    if (EFI_ERROR(st)) return st;
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st)) return st;
    st = root->Open(root, &file, KERNEL_PATH, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st)) { root->Close(root); return st; }

    UINTN info_size = 0;
    st = file->GetInfo(file, &FILE_INFO_GUID, &info_size, 0);
    if (st != EFI_BUFFER_TOO_SMALL) { file->Close(file); root->Close(root); return st; }
    st = g_bs->AllocatePool(EfiLoaderData, info_size, &info);
    if (EFI_ERROR(st)) { file->Close(file); root->Close(root); return st; }
    st = file->GetInfo(file, &FILE_INFO_GUID, &info_size, info);
    if (EFI_ERROR(st)) { g_bs->FreePool(info); file->Close(file); root->Close(root); return st; }

    u64 size = ((EFI_FILE_INFO_PREFIX *)info)->FileSize;
    g_bs->FreePool(info);
    if (size < sizeof(Elf64_Ehdr)) { file->Close(file); root->Close(root); return EFI_UNSUPPORTED; }

    st = g_bs->AllocatePool(EfiLoaderData, (UINTN)size, &data);
    if (EFI_ERROR(st)) { file->Close(file); root->Close(root); return st; }
    UINTN read_size = (UINTN)size;
    st = file->Read(file, &read_size, data);
    file->Close(file);
    root->Close(root);
    if (EFI_ERROR(st) || read_size != size) {
        g_bs->FreePool(data);
        return EFI_UNSUPPORTED;
    }

    *out_buffer = data;
    *out_size = size;
    return EFI_SUCCESS;
}

static EFI_STATUS load_kernel_elf(EFI_HANDLE image, LoadedKernel *out) {
    void *file = 0;
    u64 file_size = 0;
    EFI_STATUS st = read_entire_file(image, &file, &file_size);
    if (EFI_ERROR(st)) return st;

    Elf64_Ehdr *eh = (Elf64_Ehdr *)file;
    if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F' ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB || eh->e_type != ET_DYN ||
        eh->e_machine != EM_X86_64 || eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0) {
        g_bs->FreePool(file);
        return EFI_UNSUPPORTED;
    }
    if (eh->e_phoff > file_size || (u64)eh->e_phnum * sizeof(Elf64_Phdr) > file_size - eh->e_phoff) {
        g_bs->FreePool(file);
        return EFI_UNSUPPORTED;
    }

    Elf64_Phdr *ph = (Elf64_Phdr *)((u8 *)file + eh->e_phoff);
    u64 min_vaddr = ~0ULL;
    u64 max_vaddr = 0;
    u32 load_count = 0;
    for (u16 i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (ph[i].p_filesz > ph[i].p_memsz || ph[i].p_offset > file_size || ph[i].p_filesz > file_size - ph[i].p_offset) {
            g_bs->FreePool(file);
            return EFI_UNSUPPORTED;
        }
        if (ph[i].p_vaddr + ph[i].p_memsz < ph[i].p_vaddr) {
            g_bs->FreePool(file);
            return EFI_UNSUPPORTED;
        }
        u64 lo = align_down(ph[i].p_vaddr, 4096);
        u64 hi = align_up(ph[i].p_vaddr + ph[i].p_memsz, 4096);
        if (lo < min_vaddr) min_vaddr = lo;
        if (hi > max_vaddr) max_vaddr = hi;
        ++load_count;
    }
    if (load_count == 0 || max_vaddr <= min_vaddr) { g_bs->FreePool(file); return EFI_UNSUPPORTED; }

    u64 image_size = max_vaddr - min_vaddr;
    UINTN pages = (UINTN)(image_size / 4096);
    EFI_PHYSICAL_ADDRESS phys = 0;
    st = g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &phys);
    if (EFI_ERROR(st)) { g_bs->FreePool(file); return st; }

    zero_bytes((void *)(UINTN)phys, image_size);
    u64 bias = phys - min_vaddr;
    for (u16 i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        copy_bytes((void *)(UINTN)(bias + ph[i].p_vaddr), (u8 *)file + ph[i].p_offset, ph[i].p_filesz);
    }

    out->base = phys;
    out->size = image_size;
    out->entry = bias + eh->e_entry;
    g_bs->FreePool(file);
    return EFI_SUCCESS;
}

static EFI_STATUS get_framebuffer(BootInfo *bi) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    EFI_STATUS st = g_bs->LocateProtocol(&GOP_GUID, 0, (void **)&gop);
    if (EFI_ERROR(st)) return st;
    if (!gop || !gop->Mode || !gop->Mode->Info || !gop->Mode->FrameBufferBase || gop->Mode->Info->PixelFormat == 3) {
        return EFI_UNSUPPORTED;
    }

    bi->framebuffer_base = gop->Mode->FrameBufferBase;
    bi->framebuffer_size = gop->Mode->FrameBufferSize;
    bi->framebuffer_width = gop->Mode->Info->HorizontalResolution;
    bi->framebuffer_height = gop->Mode->Info->VerticalResolution;
    bi->framebuffer_pixels_per_scanline = gop->Mode->Info->PixelsPerScanLine;
    bi->framebuffer_pixel_format = gop->Mode->Info->PixelFormat;
    bi->framebuffer_red_mask = gop->Mode->Info->PixelInformation.RedMask;
    bi->framebuffer_green_mask = gop->Mode->Info->PixelInformation.GreenMask;
    bi->framebuffer_blue_mask = gop->Mode->Info->PixelInformation.BlueMask;
    bi->framebuffer_reserved_mask = gop->Mode->Info->PixelInformation.ReservedMask;
    return EFI_SUCCESS;
}

static EFI_STATUS allocate_kernel_stack(BootInfo *bi) {
    const UINTN pages = 16; /* 64 KiB */
    EFI_PHYSICAL_ADDRESS base = 0;
    EFI_STATUS st = g_bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &base);
    if (EFI_ERROR(st)) return st;
    bi->kernel_stack_top = align_down(base + pages * 4096, 16);
    return EFI_SUCCESS;
}

static EFI_STATUS exit_boot_services(EFI_HANDLE image, BootInfo *bi) {
    EFI_STATUS st;
    UINTN needed = 0, key = 0, desc_size = 0;
    u32 desc_version = 0;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    UINTN capacity = 0;

    st = g_bs->GetMemoryMap(&needed, 0, &key, &desc_size, &desc_version);
    if (st != EFI_BUFFER_TOO_SMALL || desc_size == 0) return st;

    capacity = needed + desc_size * 32;
    st = g_bs->AllocatePool(EfiLoaderData, capacity, (void **)&map);
    if (EFI_ERROR(st)) return st;

    /* Disable UEFI's boot watchdog before taking the final memory map. */
    g_bs->SetWatchdogTimer(0, 0, 0, 0);

    for (u32 attempt = 0; attempt < 8; ++attempt) {
        UINTN map_size = capacity;
        st = g_bs->GetMemoryMap(&map_size, map, &key, &desc_size, &desc_version);
        if (st == EFI_BUFFER_TOO_SMALL) {
            g_bs->FreePool(map);
            capacity = map_size + desc_size * 32;
            st = g_bs->AllocatePool(EfiLoaderData, capacity, (void **)&map);
            if (EFI_ERROR(st)) return st;
            continue;
        }
        if (EFI_ERROR(st)) return st;

        bi->memory_map = (u64)(UINTN)map;
        bi->memory_map_size = map_size;
        bi->memory_map_descriptor_size = desc_size;
        bi->memory_map_descriptor_version = desc_version;

        /* No Boot Services calls may occur between the successful map and this call. */
        st = g_bs->ExitBootServices(image, key);
        if (st == EFI_SUCCESS) return EFI_SUCCESS;
        if (st != EFI_INVALID_PARAMETER) return st;
        /* Firmware changed the map. Retry using the already allocated buffer. */
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
    EFI_STATUS st = load_kernel_elf(image, &kernel);
    if (EFI_ERROR(st)) return fail(st);

    g_boot_info.magic = BOOT_INFO_MAGIC;
    g_boot_info.kernel_base = kernel.base;
    g_boot_info.kernel_size = kernel.size;
    g_boot_info.kernel_entry = kernel.entry;

    st = get_framebuffer(&g_boot_info);
    if (EFI_ERROR(st)) return fail(st);
    st = allocate_kernel_stack(&g_boot_info);
    if (EFI_ERROR(st)) return fail(st);

    text(MSG_EXIT);
    st = exit_boot_services(image, &g_boot_info);
    if (EFI_ERROR(st)) return fail(st); /* Safe only because ExitBootServices failed. */

    ((KERNEL_ENTRY)(UINTN)kernel.entry)(&g_boot_info);
    for (;;) __asm__ volatile ("hlt");
}
