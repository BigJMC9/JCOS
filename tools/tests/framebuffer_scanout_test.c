/* Actual framebuffer/display/terminal code, simulated MMIO and IRQ boundaries.
 * Process/capability doubles are only routing scaffolding; live Ring3 teardown
 * and process-fault recovery are covered separately by the QEMU diagnostics. */
#include <stdio.h>
#include <string.h>
#include "../../kernel/display.h"
#include "../../kernel/interrupts.h"
#include "../../kernel/lib.h"
#include "../../kernel/serial.h"
#include "../../kernel/terminal.h"
#include "../../kernel/user_memory.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "scanout line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define WIDTH 128U
#define HEIGHT 64U
#define PITCH 132U
#define WORDS (3U * 4096U)
#define USER_BASE 0x0000022000000000ULL
static _Alignas(4096) u32 pixels[WORDS];
static u32 saved[WORDS];
static DeviceResource *display_device;
static Process user;
static u32 serial_bytes, observed_lines;
static bool fail_read, retire_on_read, check_handoff_on_read;
static bool callback_ok, retirement_completed;
static void (*on_enable)(void);

void interrupts_disable(void) {}
void interrupts_enable(void) {
    void (*callback)(void) = on_enable;
    on_enable = 0; /* Simulate one preemption, never recursively inject. */
    if (callback) callback();
}
void serial_putc(char c) { (void)c; ++serial_bytes; }
void serial_clear(void) {}
void serial_write(const char *s) { while (s && *s) serial_putc(*s++); }
bool device_resource_create(DeviceResource *r, DeviceResourceKind kind) {
    if (r->initialized) return false;
    r->initialized = true; r->kind = kind; r->id = 1;
    display_device = r;
    return true;
}
bool device_resource_valid(const DeviceResource *r) { return r && r->initialized; }
bool process_storage_in_use(const Process *p) { return p == &user; }
CapabilityTable *process_capabilities(Process *p) { return p == &user ? &p->capabilities : 0; }
bool capability_lookup_rights(const CapabilityTable *table, CapabilityHandle handle,
    CapabilityType type, CapabilityRights rights, void **out) {
    if (table != &user.capabilities || handle != 5 ||
        type != CAPABILITY_TYPE_DEVICE_RESOURCE || rights != CAPABILITY_RIGHT_WRITE) return false;
    *out = display_device;
    return true;
}

static bool prepare(void) {
    ProgramBorrowedMappingSpec mapping;
    FramebufferInfo info;
    u64 pointer;
    return display_direct_prepare(USER_BASE, &mapping, &info, &pointer);
}
static void retire_during_writer(void) {
    FramebufferScanoutStatus status;
    retirement_completed = display_retire_firmware_scanout();
    framebuffer_scanout_status(&status);
    callback_ok = !retirement_completed && status.state == FRAMEBUFFER_SCANOUT_RETIRING &&
        status.kernel_writers == 1 && !framebuffer_firmware_valid() &&
        !framebuffer_write_begin() && !prepare();
    /* New primitive admission must be closed while the old writer drains. */
    u32 before = pixels[0];
    framebuffer_pixel(0, 0, ~before);
    callback_ok = callback_ok && pixels[0] == before;
}
bool user_memory_read(const Process *p, u64 address, void *destination, u64 size) {
    if (p != &user || fail_read) return false;
    if (check_handoff_on_read) {
        check_handoff_on_read = false;
        callback_ok = !prepare() && display_direct_state() == DISPLAY_LEASE_KERNEL;
    }
    if (retire_on_read) {
        retire_on_read = false;
        retire_during_writer();
    }
    memcpy(destination, (const void *)address, (size_t)size);
    return true;
}
static void observe(const char *line, u32 length, void *context) {
    (void)line; (void)length; (void)context;
    ++observed_lines;
}
static bool unchanged(void) { return !memcmp(pixels, saved, sizeof(pixels)); }
static void attempt_writes(void) {
    const u8 row[] = {0xFF, 0x00};
    framebuffer_pixel(0, 0, 0xABCDEF);
    framebuffer_fill(0xABCDEF);
    framebuffer_fill_rect(0, 0, WIDTH, HEIGHT, 0xABCDEF);
    framebuffer_scroll_up(1, 0xABCDEF);
    framebuffer_scroll_up(HEIGHT, 0xABCDEF);
    framebuffer_blit_rgb332_row(0, 0, WIDTH, row, 2);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    FramebufferScanoutStatus status;
    framebuffer_scanout_status(&status);
    CHECK(status.state == FRAMEBUFFER_SCANOUT_UNAVAILABLE && !status.kernel_writers);
    CHECK(!framebuffer_write_begin() && !display_retire_firmware_scanout());
    BootMemoryDescriptor memory = {0};
    memory.type = 11;
    memory.physical_start = (u64)pixels;
    memory.number_of_pages = sizeof(pixels) / 4096U;
    BootInfo boot = {0};
    boot.framebuffer_base = (u64)pixels;
    boot.framebuffer_size = sizeof(pixels);
    boot.framebuffer_width = WIDTH;
    boot.framebuffer_height = HEIGHT;
    boot.framebuffer_pixels_per_scanline = PITCH;
    boot.framebuffer_pixel_format = 1;
    boot.memory_map = (u64)&memory;
    boot.memory_map_size = sizeof(memory);
    boot.memory_map_descriptor_size = sizeof(memory);
    CHECK(framebuffer_init(&boot) && display_init() && terminal_init());
    user.initialized = true; user.id = 7;
    CHECK(display_available() && framebuffer_firmware_valid());
    FramebufferInfo info, retained;
    CHECK(framebuffer_info(&info) && info.direct_map_safe);
    u64 refused_identity=~0ULL;
    CHECK(!framebuffer_native_install(&info,false,&refused_identity) && !refused_identity &&
        framebuffer_firmware_valid() && framebuffer_output_matches(0));
    CHECK(!framebuffer_native_install(&info,false,0));
    CHECK(!framebuffer_native_install(0,false,&refused_identity) && !refused_identity);

    /* Rendering clips to visible pixels; pitch padding/trailing bytes survive. */
    for (u32 i = 0; i < WORDS; ++i) pixels[i] = 0x55AA55AAU;
    framebuffer_fill(0x010203U);
    for (u32 y = 0; y < HEIGHT; ++y)
        for (u32 x = 0; x < PITCH; ++x)
            CHECK(pixels[y * PITCH + x] == (x < WIDTH ? 0x010203U : 0x55AA55AAU));
    CHECK(pixels[WORDS - 1] == 0x55AA55AAU);
    framebuffer_pixel(1, 1, 0x040506U);
    CHECK(pixels[PITCH + 1] == 0x040506U);
    framebuffer_fill_rect(WIDTH - 1, HEIGHT - 1, ~0U, ~0U, 0x070809U);
    CHECK(pixels[(HEIGHT - 1) * PITCH + WIDTH - 1] == 0x070809U);
    memcpy(saved, pixels, sizeof(pixels));
    framebuffer_fill_rect(~0U, ~0U, 2, 2, 0);
    framebuffer_pixel(WIDTH, HEIGHT, 0);
    CHECK(unchanged());
    framebuffer_scroll_up(1, 0x112233);
    CHECK(pixels[1] == 0x040506U && pixels[(HEIGHT - 1) * PITCH] == 0x112233U);
    const u8 row[] = {0xE0, 0x03};
    framebuffer_blit_rgb332_row(0, 0, 2, row, 2);
    CHECK(pixels[0] == 0xFF0000U && pixels[1] == 0x0000FFU);

    CHECK(framebuffer_write_begin());
    CHECK(!prepare() && framebuffer_firmware_valid());
    framebuffer_write_end();
    CHECK(framebuffer_writers_idle());
    CHECK(prepare() && display_direct_state() == DISPLAY_LEASE_HANDOFF);
    CHECK(!display_retire_firmware_scanout() && framebuffer_firmware_valid());
    memcpy(saved, pixels, sizeof(pixels));
    attempt_writes();
    CHECK(unchanged() && !framebuffer_write_begin());
    CHECK(display_direct_abort() && terminal_render_enabled());
    CHECK(prepare() && display_direct_commit(&user));
    CHECK(!display_retire_firmware_scanout() && framebuffer_firmware_valid());
    memcpy(saved, pixels, sizeof(pixels));
    attempt_writes();
    CHECK(unchanged() && !display_direct_reclaim(user.id + 1));
    CHECK(display_direct_reclaim(user.id) && terminal_render_enabled());
    CHECK(display_direct_state() == DISPLAY_LEASE_KERNEL);

    const u8 image[] = {0xE0, 0x03, 0x1C, 0xFF};
    check_handoff_on_read = true;
    CHECK(display_present_user(&user, 5, (u64)image, 2, 2) && callback_ok);
    CHECK(framebuffer_writers_idle());
    fail_read = true;
    CHECK(!display_present_user(&user, 5, (u64)image, 2, 2));
    fail_read = false;
    CHECK(framebuffer_writers_idle());

    if (!strcmp(argv[1], "preempt")) {
        on_enable = retire_during_writer;
        framebuffer_fill(0x334455);
        CHECK(callback_ok && !retirement_completed);
        CHECK(pixels[0] == 0x334455U); /* An admitted primitive can finish. */
    } else if (!strcmp(argv[1], "present")) {
        retire_on_read = true;
        CHECK(!display_present_user(&user, 5, (u64)image, 2, 2));
        CHECK(callback_ok && !retirement_completed);
    } else {
        CHECK(!strcmp(argv[1], "idle"));
        CHECK(display_retire_firmware_scanout());
    }
    framebuffer_scanout_status(&status);
    CHECK(status.state == FRAMEBUFFER_SCANOUT_RETIRED && !status.kernel_writers);
    CHECK(display_retire_firmware_scanout() && !framebuffer_firmware_valid());
    CHECK(!display_available() && !display_resource() && !terminal_render_enabled());
    CHECK(!prepare() && !display_direct_commit(&user));
    CHECK(!framebuffer_init(&boot)); /* Saved metadata cannot reactivate GOP. */
    CHECK(framebuffer_info(&retained) && !memcmp(&info, &retained, sizeof(info)));
    CHECK(framebuffer_width() == WIDTH && framebuffer_height() == HEIGHT);
    memcpy(saved, pixels, sizeof(pixels));
    attempt_writes();
    CHECK(!display_present_user(&user, 5, (u64)image, 2, 2));
    CHECK(unchanged());

    /* Exercise the actual terminal after retirement, including forced redraw. */
    u32 previous_serial = serial_bytes;
    terminal_set_line_observer(observe, 0);
    terminal_set_render_enabled(true);
    terminal_writeln("GPU unavailable; serial diagnostics remain alive.");
    terminal_redraw();
    terminal_cursor_enable(true);
    terminal_cursor_toggle();
    terminal_scrollback_page_up();
    terminal_scrollback_to_bottom();
    terminal_clear();
    CHECK(!terminal_render_enabled() && unchanged());
    CHECK(serial_bytes > previous_serial && observed_lines > 0);
    framebuffer_scanout_status(&status);
    CHECK(status.state == FRAMEBUFFER_SCANOUT_RETIRED && !status.kernel_writers);
    /* Native/rescue descriptors are privileged test fixtures. No GPU proof,
     * real mappings, process fault or hardware restoration is claimed here. */
    FramebufferOutputStatus output;
    framebuffer_output_status(&output);
    CHECK(!output.valid && !output.identity && output.kind==FRAMEBUFFER_OUTPUT_NONE);
    FramebufferInfo candidate=info, active;
    u64 native_id=~0ULL, rescue_id=0;
    CHECK(!framebuffer_output_revoke(0) && !framebuffer_output_matches(0));
    for (unsigned bad=0; bad<18; ++bad) {
        candidate=info;
        switch (bad) {
        case 0: candidate.physical_base+=4096; break;
        case 1: candidate.size--; break;
        case 2: candidate.mapping_physical_base+=4096; break;
        case 3: candidate.mapping_size+=4096; break;
        case 4: candidate.page_offset++; break;
        case 5: candidate.memory_type=7; break;
        case 6: candidate.direct_map_safe=false; break;
        case 7: candidate.pixel_format=0; break;
        case 8: candidate.red_mask=1; break;
        case 9: candidate.green_mask=1; break;
        case 10: candidate.blue_mask=1; break;
        case 11: candidate.reserved_mask=1; break;
        case 12: candidate.width=0; break;
        case 13: candidate.height=0; break;
        case 14: candidate.width=candidate.pixels_per_scanline+1; break;
        case 15: candidate.pixels_per_scanline=~0U; break;
        case 16: candidate.height=~0U; break;
        case 17: candidate.width--; break;
        }
        CHECK(!framebuffer_native_install(&candidate,false,&native_id) && !native_id &&
            !framebuffer_output_valid() && !framebuffer_output_info(&active));
    }
    candidate=info;
    CHECK(framebuffer_native_install(&candidate,false,&native_id) && native_id &&
        framebuffer_output_valid() && framebuffer_output_matches(native_id) &&
        !framebuffer_firmware_valid() && !framebuffer_output_matches(0));
    CHECK(framebuffer_info(&retained) && !memcmp(&retained,&info,sizeof(info)));
    CHECK(framebuffer_output_info(&active) && !memcmp(&active,&candidate,sizeof(candidate)));
    CHECK(framebuffer_width()==candidate.width && framebuffer_height()==candidate.height);
    CHECK(!framebuffer_native_install(&info,true,&rescue_id) && !rescue_id);
    CHECK(!display_available() && !prepare()); /* Firmware-only service cannot regain authority. */
    CHECK(!display_retire_firmware_scanout() && framebuffer_output_valid());
    memcpy(saved,pixels,sizeof(pixels));
    CHECK(!display_present_user(&user,5,(u64)image,2,2) && unchanged());
    terminal_set_render_enabled(true);
    CHECK(terminal_render_enabled());
    framebuffer_pixel(candidate.width-1,candidate.height-1,0xAABBCC);
    CHECK(pixels[(candidate.height-1)*PITCH+candidate.width-1]==0xAABBCC);
    memcpy(saved,pixels,sizeof(pixels));
    framebuffer_pixel(candidate.width,candidate.height,0x112233);
    CHECK(unchanged() && framebuffer_writers_idle());
    ProgramBorrowedMappingSpec mapping;
    u64 pointer=~0ULL;
    CHECK(!display_direct_prepare_native(0,USER_BASE,&mapping,&active,&pointer) && !pointer);
    CHECK(!display_direct_prepare_native(native_id+1,USER_BASE,&mapping,&active,&pointer) && !pointer);
    CHECK(display_direct_prepare_native(native_id,USER_BASE,&mapping,&active,&pointer) &&
        pointer==USER_BASE+candidate.page_offset && mapping.physical_base==candidate.physical_base &&
        active.width==candidate.width && display_direct_state()==DISPLAY_LEASE_HANDOFF);
    CHECK(framebuffer_output_revoke(native_id) && !display_direct_commit(&user));
    CHECK(!framebuffer_native_install(&info,true,&rescue_id) && !rescue_id);
    CHECK(display_direct_abort() && !terminal_render_enabled());
    CHECK(framebuffer_native_install(&info,true,&rescue_id) && rescue_id!=native_id);
    CHECK(!framebuffer_output_revoke(native_id) && framebuffer_output_matches(rescue_id));
    framebuffer_output_status(&output);
    CHECK(output.kind==FRAMEBUFFER_OUTPUT_RESCUE && output.valid && output.identity==rescue_id);
    terminal_set_render_enabled(true);
    CHECK(display_direct_prepare_native(rescue_id,USER_BASE,&mapping,&active,&pointer) &&
        display_direct_commit(&user) && display_direct_owner_process_id()==user.id);
    memcpy(saved,pixels,sizeof(pixels));
    attempt_writes();
    CHECK(unchanged() && !framebuffer_write_begin());
    CHECK(framebuffer_output_revoke(rescue_id) && !terminal_render_enabled());
    CHECK(!framebuffer_native_install(&info,true,&native_id) && !native_id);
    CHECK(!display_direct_reclaim(user.id+1) && display_direct_reclaim(user.id));
    CHECK(!terminal_render_enabled() && display_direct_state()==DISPLAY_LEASE_KERNEL);
    CHECK(framebuffer_native_install(&candidate,false,&native_id) && native_id>rescue_id);
    CHECK(framebuffer_write_begin() && framebuffer_output_revoke(native_id));
    CHECK(!framebuffer_write_begin() && !framebuffer_native_install(&info,true,&rescue_id));
    framebuffer_write_end();
    CHECK(framebuffer_native_install(&info,true,&rescue_id) && rescue_id>native_id);
    CHECK(!framebuffer_output_revoke(native_id));
    CHECK(framebuffer_output_revoke(rescue_id) && framebuffer_output_revoke(rescue_id));
    CHECK(!framebuffer_output_valid() && !framebuffer_output_matches(rescue_id));
    framebuffer_output_status(&output);
    CHECK(output.kind==FRAMEBUFFER_OUTPUT_QUARANTINED && output.identity==rescue_id &&
        !output.valid && !output.kernel_writers);
    memcpy(saved,pixels,sizeof(pixels));
    attempt_writes();
    CHECK(unchanged());
    framebuffer_scanout_status(&status);
    CHECK(status.state==FRAMEBUFFER_SCANOUT_RETIRED && !status.kernel_writers);
    CHECK(framebuffer_info(&retained) && !memcmp(&info,&retained,sizeof(info)));
    printf("Framebuffer scanout %s: PASS (production rendering, lease, irreversible retirement, native/rescue descriptor generations and serial terminal)\n", argv[1]);
    return 0;
}
