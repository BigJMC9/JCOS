#ifndef JCOS_DISPLAY_SERVICE_PROTOCOL_H
#define JCOS_DISPLAY_SERVICE_PROTOCOL_H

#include "service_protocol.h"

#define JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION JCOS_SERVICE_PROTOCOL_VERSION_1

#define JCOS_DISPLAY_SERVICE_OP_PING             1ULL
#define JCOS_DISPLAY_SERVICE_OP_REDRAW           2ULL
#define JCOS_DISPLAY_SERVICE_OP_SHUTDOWN         3ULL
#define JCOS_DISPLAY_SERVICE_OP_DIAG_FAULT       4ULL
#define JCOS_DISPLAY_SERVICE_OP_PRESENT_SURFACE  5ULL
#define JCOS_DISPLAY_SERVICE_OP_SET_PIXEL_MASKS  6ULL
#define JCOS_DISPLAY_SERVICE_OP_CLEAR            7ULL
#define JCOS_DISPLAY_SERVICE_OP_PRESENT_RECT     8ULL
#define JCOS_DISPLAY_SERVICE_OP_SAMPLE           9ULL

#define JCOS_DISPLAY_SERVICE_REPLY_PONG           0x4453504C59504F4EULL
#define JCOS_DISPLAY_SERVICE_REPLY_DRAWN          0x4453504C59445257ULL
#define JCOS_DISPLAY_SERVICE_REPLY_STOPPED        0x4453504C59535450ULL
#define JCOS_DISPLAY_SERVICE_REPLY_PRESENTED      0x4453504C59505253ULL
#define JCOS_DISPLAY_SERVICE_REPLY_MASKS_SET      0x4453504C594D4153ULL
#define JCOS_DISPLAY_SERVICE_REPLY_CLEARED        0x4453504C59434C52ULL
#define JCOS_DISPLAY_SERVICE_REPLY_RECT_PRESENTED 0x4453504C59524543ULL
#define JCOS_DISPLAY_SERVICE_REPLY_SAMPLED        0x4453504C59534D50ULL

#define JCOS_DISPLAY_SERVICE_HEADER(op, version) \
    ((((version) & 0xFFFFFFFFULL) << 32) | ((op) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_SERVICE_HEADER_OP(header) ((header) & 0xFFFFFFFFULL)
#define JCOS_DISPLAY_SERVICE_HEADER_VERSION(header) (((header) >> 32) & 0xFFFFFFFFULL)

/* Startup argument packing after the managed-service incarnation:
 *   arg[1] = direct framebuffer virtual address
 *   arg[2] = width[31:0] | height[63:32]
 *   arg[3] = pixels_per_scanline[31:0] | GOP pixel_format[63:32]
 * PixelBitMask channel masks are supplied after launch through the versioned
 * SET_PIXEL_MASKS operation so the common program-startup ABI remains stable.
 */
#define JCOS_DISPLAY_GEOMETRY(width, height) \
    (((unsigned long long)(height) << 32) | (unsigned long long)(width))
#define JCOS_DISPLAY_FORMAT(stride, format) \
    (((unsigned long long)(format) << 32) | (unsigned long long)(stride))
#define JCOS_DISPLAY_WIDTH(word) ((unsigned int)((word) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_HEIGHT(word) ((unsigned int)(((word) >> 32) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_STRIDE(word) ((unsigned int)((word) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_PIXEL_FORMAT(word) ((unsigned int)(((word) >> 32) & 0xFFFFFFFFULL))

#define JCOS_DISPLAY_MASK_PAIR(low, high) ((((unsigned long long)(high)) << 32) | (unsigned long long)(low))
#define JCOS_DISPLAY_MASK_LOW(word) ((unsigned int)((word) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_MASK_HIGH(word) ((unsigned int)(((word) >> 32) & 0xFFFFFFFFULL))

/* Bounded shared-surface ABI. Every slot is one page. Writers map one surface
 * RW/NX in their private process; the display service maps each slot RO/NX.
 * Placement, z-order and focus policy belong to the separate Ring3 compositor.
 */
#define JCOS_DISPLAY_SURFACE_VIRTUAL_BASE  0x0000023000000000ULL
#define JCOS_DISPLAY_SURFACE_WIDTH         64U
#define JCOS_DISPLAY_SURFACE_HEIGHT        64U
#define JCOS_DISPLAY_SURFACE_STRIDE        64U
#define JCOS_DISPLAY_SURFACE_BYTES         4096U
#define JCOS_DISPLAY_SURFACE_FORMAT_RGB332 1U
#define JCOS_DISPLAY_SURFACE_CAPACITY      4U

#define JCOS_DISPLAY_SURFACE_SLOT_VA(slot) (JCOS_DISPLAY_SURFACE_VIRTUAL_BASE + ((unsigned long long)(slot) * (unsigned long long)JCOS_DISPLAY_SURFACE_BYTES))
#define JCOS_DISPLAY_SURFACE_CONFIG(slot, z) ((((unsigned long long)(z)) << 32) | (unsigned long long)(slot))
#define JCOS_DISPLAY_SURFACE_CONFIG_SLOT(word) ((unsigned int)((word) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_SURFACE_CONFIG_Z(word) ((unsigned int)(((word) >> 32) & 0xFFFFFFFFULL))

#define JCOS_DISPLAY_SURFACE_POSITION(x, y) ((((unsigned long long)(y)) << 32) | (unsigned long long)(x))
#define JCOS_DISPLAY_SURFACE_POSITION_X(word) ((unsigned int)((word) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_SURFACE_POSITION_Y(word) ((unsigned int)(((word) >> 32) & 0xFFFFFFFFULL))

#define JCOS_DISPLAY_SURFACE_EXTENT(width, height) ((((unsigned long long)(height)) << 32) | (unsigned long long)(width))
#define JCOS_DISPLAY_SURFACE_EXTENT_WIDTH(word) ((unsigned int)((word) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_SURFACE_EXTENT_HEIGHT(word) ((unsigned int)(((word) >> 32) & 0xFFFFFFFFULL))

#define JCOS_DISPLAY_COMPOSE_DETAIL(count, sample) ((((unsigned long long)(sample)) << 32) | (unsigned long long)(count))
#define JCOS_DISPLAY_COMPOSE_COUNT(word) ((unsigned int)((word) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_COMPOSE_SAMPLE(word) ((unsigned int)(((word) >> 32) & 0xFFFFFFFFULL))

#define JCOS_DISPLAY_SURFACE_FOCUS_NONE 0xFFFFFFFFU

/* Graphical clients receive only a dedicated RECEIVE endpoint plus their own
 * RW/NX surface mapping; they never receive compositor command authority or a
 * direct framebuffer mapping. */
#define JCOS_DISPLAY_CLIENT_PROTOCOL_VERSION 1ULL
#define JCOS_DISPLAY_CLIENT_OP_KEY_EVENT     1ULL
#define JCOS_DISPLAY_CLIENT_OP_DIAG_FAULT    2ULL

#define JCOS_DISPLAY_CLIENT_HEADER(op, version) ((((version) & 0xFFFFFFFFULL) << 32) | ((op) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_CLIENT_HEADER_OP(header) ((header) & 0xFFFFFFFFULL)
#define JCOS_DISPLAY_CLIENT_HEADER_VERSION(header) (((header) >> 32) & 0xFFFFFFFFULL)

#define JCOS_DISPLAY_INPUT_KEY_NONE       0U
#define JCOS_DISPLAY_INPUT_KEY_CHARACTER  1U
#define JCOS_DISPLAY_INPUT_KEY_ENTER      2U
#define JCOS_DISPLAY_INPUT_KEY_BACKSPACE  3U
#define JCOS_DISPLAY_INPUT_KEY_TAB        4U
#define JCOS_DISPLAY_INPUT_KEY_ESCAPE     5U
#define JCOS_DISPLAY_INPUT_KEY_UP         6U
#define JCOS_DISPLAY_INPUT_KEY_DOWN       7U
#define JCOS_DISPLAY_INPUT_KEY_LEFT       8U
#define JCOS_DISPLAY_INPUT_KEY_RIGHT      9U
#define JCOS_DISPLAY_INPUT_KEY_HOME       10U
#define JCOS_DISPLAY_INPUT_KEY_END        11U
#define JCOS_DISPLAY_INPUT_KEY_DELETE     12U
#define JCOS_DISPLAY_INPUT_KEY_PAGE_UP    13U
#define JCOS_DISPLAY_INPUT_KEY_PAGE_DOWN  14U

#define JCOS_DISPLAY_INPUT_EVENT(key, character) ((((unsigned long long)((unsigned char)(character))) << 32) | ((unsigned long long)(key) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_INPUT_EVENT_KEY(word) ((unsigned int)((word) & 0xFFFFFFFFULL))
#define JCOS_DISPLAY_INPUT_EVENT_CHARACTER(word) ((unsigned char)(((word) >> 32) & 0xFFULL))

#define JCOS_DISPLAY_INPUT_FLAG_PRESSED (1ULL << 0)
#define JCOS_DISPLAY_INPUT_FLAG_SHIFT   (1ULL << 1)
#define JCOS_DISPLAY_INPUT_FLAG_CTRL    (1ULL << 2)
#define JCOS_DISPLAY_INPUT_FLAG_ALT     (1ULL << 3)

#define JCOS_DISPLAY_SURFACE_INPUT_CLIENT_COOKIE 0x523843494E505554ULL
#define JCOS_DISPLAY_SURFACE_SESSION_CLIENT_COOKIE 0x523843534553534EULL
#define JCOS_DISPLAY_SURFACE_INPUT_MARKER        0xC3U
#define JCOS_DISPLAY_SURFACE_INPUT_MARKER_OFFSET (JCOS_DISPLAY_SURFACE_BYTES - 4U)
#define JCOS_DISPLAY_SURFACE_INPUT_KEY_OFFSET    (JCOS_DISPLAY_SURFACE_BYTES - 3U)
#define JCOS_DISPLAY_SURFACE_INPUT_CHAR_OFFSET   (JCOS_DISPLAY_SURFACE_BYTES - 2U)
#define JCOS_DISPLAY_SURFACE_INPUT_FLAGS_OFFSET  (JCOS_DISPLAY_SURFACE_BYTES - 1U)

#define JCOS_DISPLAY_SURFACE_APP_PATH "/bin/surfaceapp.elf"
#define JCOS_DISPLAY_SURFACE_APP_COOKIE 0x5238435355524641ULL
#define JCOS_DISPLAY_SURFACE_APP_MARKER 0xA5U
#define JCOS_DISPLAY_SURFACE_PRODUCER_COOKIE 0x52384350524F4455ULL
#define JCOS_DISPLAY_SURFACE_COLOR_RED   0xE0U
#define JCOS_DISPLAY_SURFACE_COLOR_GREEN 0x1CU

#endif