#include "timer.h"
#include "arch.h"
#include "interrupt_controller.h"

#define PIT_CHANNEL0_DATA 0x40U
#define PIT_COMMAND       0x43U

#define PIT_INPUT_HZ      1193182U

/*
 * Channel 0
 * Access low byte + high byte
 * Mode 3 square wave
 * Binary counting
 */
#define PIT_COMMAND_MODE3 0x36U

static volatile u64 g_ticks;
static u32 g_frequency;
static bool g_initialized;

bool timer_init(u32 frequency_hz) {
    if (g_initialized)
        return false;

    if (!frequency_hz ||
        frequency_hz > PIT_INPUT_HZ) {

        return false;
    }

    /*
     * Round to the nearest PIT divisor.
     */
    u32 divisor =
        (PIT_INPUT_HZ +
         frequency_hz / 2U) /
        frequency_hz;

    if (!divisor ||
        divisor > 0xFFFFU) {

        return false;
    }

    /*
     * The kernel still has interrupts disabled
     * during initialization.
     *
     * Program PIT channel 0 before exposing IRQ0.
     */
    arch_out8(
        PIT_COMMAND,
        PIT_COMMAND_MODE3
    );

    arch_out8(
        PIT_CHANNEL0_DATA,
        (u8)(divisor & 0xFFU)
    );

    arch_out8(
        PIT_CHANNEL0_DATA,
        (u8)((divisor >> 8) & 0xFFU)
    );

    /*
     * v1 uses the legacy PIC path only.
     */
    if (!interrupt_controller_unmask_legacy_irq(0U))
        return false;

    g_ticks = 0;
    g_frequency = frequency_hz;
    g_initialized = true;

    return true;
}

void timer_handle_irq(void) {
    if (g_initialized)
        ++g_ticks;
}

bool timer_initialized(void) {
    return g_initialized;
}

u32 timer_frequency(void) {
    return
        g_initialized
            ? g_frequency
            : 0U;
}

u64 timer_ticks(void) {
    return g_ticks;
}