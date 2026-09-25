#include "display.h"

#include "address_space.h"
#include "capability.h"
#include "framebuffer.h"
#include "interrupts.h"
#include "lib.h"
#include "terminal.h"
#include "user_memory.h"
#include "../include/media_abi.h"

#define DISPLAY_RFLAGS_IF (1ULL << 9)

static DeviceResource g_display;
static u8 g_source_row[JCOS_MEDIA_MAX_WIDTH];
static u64 g_owner_process_id;
static DisplayLeaseState g_direct_state;
static u64 g_direct_owner_process_id;
static bool g_restore_terminal_render;

static u64 display_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void display_irq_restore(u64 flags) {
    if (flags & DISPLAY_RFLAGS_IF) interrupts_enable();
}

bool display_init(void) {
    if (!framebuffer_width() || !framebuffer_height() ||
        !device_resource_create(&g_display, DEVICE_RESOURCE_DISPLAY)) return false;
    g_owner_process_id = 0ULL;
    g_direct_owner_process_id = 0ULL;
    g_restore_terminal_render = false;
    g_direct_state = DISPLAY_LEASE_KERNEL;
    return true;
}

bool display_available(void) {
    return device_resource_valid(&g_display);
}

DeviceResource *display_resource(void) {
    return display_available() ? &g_display : 0;
}

void display_session_reset(void) {
    /* Legacy DISPLAY_PRESENT ownership is independent of the R8 direct lease. */
    g_owner_process_id = 0ULL;
}

DisplayLeaseState display_direct_state(void) {
    u64 flags = display_irq_save();
    DisplayLeaseState state = g_direct_state;
    display_irq_restore(flags);
    return state;
}

u64 display_direct_owner_process_id(void) {
    u64 flags = display_irq_save();
    u64 owner = g_direct_state == DISPLAY_LEASE_USER ?
        g_direct_owner_process_id : 0ULL;
    display_irq_restore(flags);
    return owner;
}

bool display_direct_prepare(u64 user_virtual_base, ProgramBorrowedMappingSpec *out_mapping, FramebufferInfo *out_info, u64 *out_user_framebuffer) {
    if (!out_mapping || !out_info || !out_user_framebuffer) return false;
    k_memset(out_mapping, 0, sizeof(*out_mapping));
    k_memset(out_info, 0, sizeof(*out_info));
    *out_user_framebuffer = 0ULL;

    if (!display_available() ||
        (user_virtual_base & (VM_PAGE_SIZE - 1ULL)) ||
        user_virtual_base < ADDRESS_SPACE_USER_BASE ||
        user_virtual_base >= ADDRESS_SPACE_USER_LIMIT) return false;

    FramebufferInfo info;
    if (!framebuffer_info(&info) || !info.physical_base || !info.size ||
        !info.direct_map_safe || !info.mapping_physical_base || !info.mapping_size ||
        info.page_offset >= VM_PAGE_SIZE ||
        info.mapping_size > ADDRESS_SPACE_USER_LIMIT - user_virtual_base ||
        user_virtual_base > ~0ULL - info.page_offset) return false;

    ProgramBorrowedMappingSpec mapping;
    k_memset(&mapping, 0, sizeof(mapping));
    mapping.physical_base = info.physical_base;
    mapping.size = info.size;
    mapping.virtual_base = user_virtual_base;
    mapping.flags = VM_WRITE | VM_UNCACHED;

    u64 flags = display_irq_save();
    if (g_direct_state != DISPLAY_LEASE_KERNEL) {
        display_irq_restore(flags);
        return false;
    }

    /* HANDOFF blocks the legacy kernel-mediated presenter before the direct
     * mapping can become runnable. Preserve deep-suite quiet rendering state. */
    g_direct_state = DISPLAY_LEASE_HANDOFF;
    g_direct_owner_process_id = 0ULL;
    g_owner_process_id = 0ULL;
    g_restore_terminal_render = terminal_render_enabled();
    if (g_restore_terminal_render) terminal_set_render_enabled(false);
    display_irq_restore(flags);

    *out_mapping = mapping;
    *out_info = info;
    *out_user_framebuffer = user_virtual_base + info.page_offset;
    return true;
}

bool display_direct_commit(Process *process) {
    if (!process || !process_storage_in_use(process) || !process->initialized ||
        process->kernel || !process->id) return false;

    u64 flags = display_irq_save();
    bool committed = g_direct_state == DISPLAY_LEASE_HANDOFF;
    if (committed) {
        g_direct_owner_process_id = process->id;
        g_direct_state = DISPLAY_LEASE_USER;
    }
    display_irq_restore(flags);
    return committed;
}

static bool display_direct_restore_kernel(DisplayLeaseState expected, u64 owner_process_id) {
    bool restore_render = false;
    u64 flags = display_irq_save();
    if (g_direct_state != expected ||
        (expected == DISPLAY_LEASE_USER &&
         (!owner_process_id || g_direct_owner_process_id != owner_process_id))) {
        display_irq_restore(flags);
        return false;
    }

    g_direct_state = DISPLAY_LEASE_RECLAIM;
    g_direct_owner_process_id = 0ULL;
    g_owner_process_id = 0ULL;
    restore_render = g_restore_terminal_render;
    g_restore_terminal_render = false;
    display_irq_restore(flags);

    /* Keep legacy presentation blocked while the terminal reconstructs its
     * viewport. Serial logging remains independent throughout the handoff. */
    if (restore_render) terminal_set_render_enabled(true);

    flags = display_irq_save();
    if (g_direct_state != DISPLAY_LEASE_RECLAIM) {
        display_irq_restore(flags);
        return false;
    }
    g_direct_state = DISPLAY_LEASE_KERNEL;
    display_irq_restore(flags);
    return true;
}

bool display_direct_abort(void) {
    return display_direct_restore_kernel(DISPLAY_LEASE_HANDOFF, 0ULL);
}

bool display_direct_reclaim(u64 owner_process_id) {
    return display_direct_restore_kernel(DISPLAY_LEASE_USER, owner_process_id);
}

static bool display_authorized(Process *process, CapabilityHandle handle) {
    CapabilityTable *caps = process ? process_capabilities(process) : 0;
    void *resource = 0;
    return caps && capability_lookup_rights(caps, handle,
        CAPABILITY_TYPE_DEVICE_RESOURCE, CAPABILITY_RIGHT_WRITE, &resource) &&
        resource == &g_display;
}

bool display_present_user(Process *process, CapabilityHandle handle, u64 pixels,
    u32 width, u32 height) {
    if (display_direct_state() != DISPLAY_LEASE_KERNEL) return false;
    if (!display_authorized(process, handle) || !pixels || !width || !height ||
        width > JCOS_MEDIA_MAX_WIDTH || height > JCOS_MEDIA_MAX_HEIGHT) return false;

    u32 screen_width = framebuffer_width();
    u32 screen_height = framebuffer_height();
    if (!screen_width || !screen_height) return false;

    u32 target_width = screen_width;
    u32 target_height = (u32)(((u64)height * screen_width) / width);
    if (!target_height) target_height = 1U;
    if (target_height > screen_height) {
        target_height = screen_height;
        target_width = (u32)(((u64)width * screen_height) / height);
        if (!target_width) target_width = 1U;
    }
    u32 left = (screen_width - target_width) / 2U;
    u32 top = (screen_height - target_height) / 2U;

    if (g_owner_process_id != process->id) {
        framebuffer_fill(framebuffer_rgb(0U, 0U, 0U));
        g_owner_process_id = process->id;
    }

    u32 loaded_row = ~0U;
    for (u32 y = 0; y < target_height; ++y) {
        u32 source_y = (u32)(((u64)y * height) / target_height);
        if (source_y != loaded_row) {
            u64 offset = (u64)source_y * width;
            if (pixels > ~0ULL - offset ||
                !user_memory_read(process, pixels + offset, g_source_row, width)) return false;
            loaded_row = source_y;
        }
        framebuffer_blit_rgb332_row(left, top + y, target_width, g_source_row, width);
    }
    return true;
}
