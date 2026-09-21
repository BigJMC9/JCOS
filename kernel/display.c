#include "display.h"

#include "capability.h"
#include "framebuffer.h"
#include "user_memory.h"
#include "../include/media_abi.h"

static DeviceResource g_display;
static u8 g_source_row[JCOS_MEDIA_MAX_WIDTH];
static u64 g_owner_process_id;

bool display_init(void) {
    return framebuffer_width() && framebuffer_height() &&
        device_resource_create(&g_display, DEVICE_RESOURCE_DISPLAY);
}

bool display_available(void) {
    return device_resource_valid(&g_display);
}

DeviceResource *display_resource(void) {
    return display_available() ? &g_display : 0;
}

void display_session_reset(void) {
    g_owner_process_id = 0ULL;
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
