#include "usb_xhci.h"

#include "arch.h"
#include "lib.h"
#include "pci.h"
#include "pmm.h"
#include "physmap.h"
#include "serial.h"

#define XHCI_CLASS 0x0CU
#define XHCI_SUBCLASS 0x03U
#define XHCI_PROGIF 0x30U
#define XHCI_MAX_TRBS 256U
#define XHCI_QUEUE_SIZE 32U
#define XHCI_WAIT_LIMIT 200000U
#define XHCI_MMIO_SIZE 0x10000ULL
#define XHCI_MAX_PORTS 255U
#define XHCI_FAILURE_CAPACITY 64U
#define XHCI_RUNTIME_INTERRUPTER0_OFFSET 0x20U
#define XHCI_INPUT_CONTROL_SIZE 32U
#define XHCI_HCC_AC64 (1U << 0)
#define XHCI_HCC_CSZ  (1U << 2)
#define XHCI_HCC_PPC  (1U << 3)
#define XHCI_USBCMD_RUN (1U << 0)
#define XHCI_USBCMD_HCRST (1U << 1)
#define XHCI_USBSTS_HCH (1U << 0)
#define XHCI_USBSTS_CNR (1U << 11)
#define XHCI_PORTSC_CCS (1U << 0)
#define XHCI_PORTSC_PED (1U << 1)
#define XHCI_PORTSC_PR  (1U << 4)
#define XHCI_PORTSC_PP  (1U << 9)
#define XHCI_PORTSC_CSC (1U << 17)
#define XHCI_PORTSC_PEC (1U << 18)
#define XHCI_PORTSC_WRC (1U << 19)
#define XHCI_PORTSC_OCC (1U << 20)
#define XHCI_PORTSC_PRC (1U << 21)
#define XHCI_PORTSC_PLC (1U << 22)
#define XHCI_PORTSC_CEC (1U << 23)
#define XHCI_PORTSC_WPR (1U << 31)
#define XHCI_PORTSC_CHANGE_BITS \
    (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | \
     XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)
#define XHCI_PORTSC_WRITE_ONE_BITS \
    (XHCI_PORTSC_PED | XHCI_PORTSC_PR | XHCI_PORTSC_WPR | XHCI_PORTSC_CHANGE_BITS)
#define XHCI_ERDP_EHB (1U << 3)
#define XHCI_EXT_CAP_ID(value) ((u32)(value) & 0xFFU)
#define XHCI_EXT_CAP_NEXT(value) (((u32)(value) >> 8) & 0xFFU)
#define XHCI_EXT_CAP_SUPPORTED_PROTOCOL 2U
#define XHCI_LEGACY_BIOS_OWNED (1U << 16)
#define XHCI_LEGACY_OS_OWNED   (1U << 24)

#define TRB_TYPE(value) ((u32)(value) << 10)
#define TRB_SLOT_ID(value) ((u32)(value) << 24)
#define TRB_CYCLE (1U << 0)
#define TRB_ENT (1U << 1)
#define TRB_IOC (1U << 5)
#define TRB_IDT (1U << 6)
#define TRB_DIR (1U << 16)
#define TRB_CHAIN (1U << 4)
#define TRB_SETUP_TRANSFER_TYPE(value) ((u32)(value) << 16)
#define TRB_ENABLE_SLOT 9U
#define TRB_DISABLE_SLOT 10U
#define TRB_ADDRESS_DEVICE 11U
#define TRB_CONFIGURE_ENDPOINT 12U
#define TRB_EVALUATE_CONTEXT 13U
#define TRB_NORMAL 1U
#define TRB_SETUP_STAGE 2U
#define TRB_DATA_STAGE 3U
#define TRB_STATUS_STAGE 4U
#define TRB_LINK 6U
#define TRB_TRANSFER_EVENT 32U
#define TRB_COMMAND_COMPLETION 33U

#define USB_REQ_GET_DESCRIPTOR 6U
#define USB_REQ_SET_PROTOCOL 11U
#define USB_REQ_SET_CONFIGURATION 9U
#define USB_DESC_DEVICE 1U
#define USB_DESC_CONFIGURATION 2U
#define USB_CLASS_HID 3U

typedef struct PACKED {
    u64 parameter;
    u32 status;
    u32 control;
} XhciTrb;

typedef struct {
    bool initialized;
    bool keyboard_ready;
    volatile u8 *mmio;
    volatile u32 *op;
    volatile u32 *doorbells;
    volatile u32 *runtime;
    u32 context_size;
    u32 max_slots;
    u32 max_ports;
    u32 scratchpad_count;
    bool addr64;
    u8 port_protocol[XHCI_MAX_PORTS];
    u32 port;
    u32 slot;
    u32 endpoint_id;
    u16 packet_size;
    u8 report[8];
    u8 previous[6];
    u64 dcbaa_phys;
    u64 scratchpad_array_phys;
    u64 input_context_phys;
    u64 device_context_phys;
    u64 command_ring_phys;
    u64 event_ring_phys;
    u64 event_table_phys;
    u64 ep0_ring_phys;
    u64 endpoint_ring_phys;
    u64 transfer_buffer_phys;
    u32 command_index;
    u32 command_cycle;
    u32 endpoint_index;
    u32 transfer_index;
    u32 transfer_cycle;
    u32 event_index;
    u32 event_cycle;
} XhciState;

static XhciState g_xhci;
static KeyEvent g_queue[XHCI_QUEUE_SIZE];
static u32 g_queue_head;
static u32 g_queue_tail;
static XhciFailureRecord g_failures[XHCI_FAILURE_CAPACITY];
static u32 g_failure_count;

static volatile u32 *reg32(u64 base, u32 offset) {
    return (volatile u32 *)(u64)(base + offset);
}

static void xhci_report_failure_detail(const char *stage, u32 detail) {
    XhciFailureRecord *record = 0;
    if (g_failure_count < XHCI_FAILURE_CAPACITY) record = &g_failures[g_failure_count++];
    if (record) {
        u32 i = 0;
        if (stage) {
            while (stage[i] && i + 1U < sizeof(record->stage)) {
                record->stage[i] = stage[i];
                ++i;
            }
        }
        record->stage[i] = 0;
        if (g_xhci.op) {
            record->usbcmd = *reg32((u64)g_xhci.op, 0x00U);
            record->usbsts = *reg32((u64)g_xhci.op, 0x04U);
        }
        record->detail = detail;
    }
    serial_write("XHCI: FAIL ");
    serial_write(stage);
    if (g_xhci.op) {
        serial_write(" usbcmd=");
        serial_write_hex(*reg32((u64)g_xhci.op, 0x00U));
        serial_write(" usbsts=");
        serial_write_hex(*reg32((u64)g_xhci.op, 0x04U));
    }
    serial_write("\n");
}

static void xhci_report_failure(const char *stage) {
    xhci_report_failure_detail(stage, 0U);
}

static bool xhci_fail(const char *stage) {
    xhci_report_failure(stage);
    return false;
}

static void clear_state(void) {
    k_memset(&g_xhci, 0, sizeof(g_xhci));
    k_memset(g_failures, 0, sizeof(g_failures));
    g_failure_count = 0;
    g_queue_head = 0;
    g_queue_tail = 0;
}

static void *dma_pages(u64 count, u64 *physical) {
    if (!physical || !count || count > (~0ULL / FRAME_SIZE)) return 0;

    u64 bytes = count * FRAME_SIZE;
    u64 address = pmm_alloc_pages(count);
    if (!address) return 0;

    if (!g_xhci.addr64 &&
        (address > 0xFFFFFFFFULL || bytes - 1ULL > 0xFFFFFFFFULL - address)) {
        (void)frame_free_range(phys_to_frame(address), count);
        return 0;
    }

    void *virtual = phys_to_virt(address);
    if (!virtual) {
        (void)frame_free_range(phys_to_frame(address), count);
        return 0;
    }

    k_memset(virtual, 0, (usize)bytes);
    *physical = address;
    return virtual;
}

static void *dma_page(u64 *physical) {
    return dma_pages(1ULL, physical);
}

static bool setup_scratchpads(u32 hcs2) {
    u32 count = (((hcs2 >> 21) & 0x1FU) << 5) | ((hcs2 >> 27) & 0x1FU);
    g_xhci.scratchpad_count = count;

    u64 *dcbaa = (u64 *)phys_to_virt(g_xhci.dcbaa_phys);
    if (!dcbaa) return false;

    dcbaa[0] = 0;
    if (!count) return true;

    u64 array_bytes = (u64)count * sizeof(u64);
    u64 array_pages = (array_bytes + FRAME_SIZE - 1ULL) / FRAME_SIZE;
    u64 array_phys = 0;
    u64 *array = (u64 *)dma_pages(array_pages, &array_phys);
    if (!array) return false;

    for (u32 i = 0; i < count; ++i) {
        u64 buffer_phys = 0;
        if (!dma_page(&buffer_phys)) {
            for (u32 j = 0; j < i; ++j) {
                (void)frame_free(phys_to_frame(array[j]));
            }
            (void)frame_free_range(phys_to_frame(array_phys), array_pages);
            return false;
        }
        array[i] = buffer_phys;
    }

    g_xhci.scratchpad_array_phys = array_phys;
    dcbaa[0] = array_phys;
    __asm__ volatile ("mfence" ::: "memory");
    return true;
}

static bool controller_halted(void) {
    return (*reg32((u64)g_xhci.op, 0x04U) & XHCI_USBSTS_HCH) != 0;
}

static bool controller_not_ready(void) {
    return (*reg32((u64)g_xhci.op, 0x04U) & XHCI_USBSTS_CNR) == 0;
}

static bool wait_condition(bool (*condition)(void)) {
    for (u32 i = 0; i < XHCI_WAIT_LIMIT; ++i) {
        if (condition()) return true;
        arch_pause();
    }
    return false;
}

static void discover_port_protocols(u64 base, u32 hcc) {
    u32 pointer = ((hcc >> 16) & 0xFFFFU) * 4U;

    for (u32 inspected = 0; pointer && inspected < 64U; ++inspected) {
        if (pointer + 12U > XHCI_MMIO_SIZE) break;

        volatile u32 *capability = reg32(base, pointer);
        u32 header = capability[0];

        if (XHCI_EXT_CAP_ID(header) == XHCI_EXT_CAP_SUPPORTED_PROTOCOL) {
            u8 major = (u8)(header >> 24);
            u32 ports = capability[2];
            u32 offset = ports & 0xFFU;
            u32 count = (ports >> 8) & 0xFFU;

            if (offset) {
                for (u32 i = 0; i < count; ++i) {
                    u32 port = offset - 1U + i;
                    if (port < g_xhci.max_ports && port < XHCI_MAX_PORTS) {
                        g_xhci.port_protocol[port] = major;
                    }
                }
            }
        }

        u32 next = XHCI_EXT_CAP_NEXT(header);
        if (!next) break;
        pointer += next * 4U;
    }
}

static bool release_legacy_control(u64 base, u32 hcc) {
    u32 pointer = ((hcc >> 16) & 0xFFFFU) * 4U;
    for (u32 inspected = 0; pointer && inspected < 64U; ++inspected) {
        if (pointer + 8U > XHCI_MMIO_SIZE) return false;
        volatile u32 *capability = reg32(base, pointer);
        u32 header = *capability;
        if (XHCI_EXT_CAP_ID(header) == 1U) {
            volatile u32 *legacy_control = capability + 1;
            *legacy_control &= ~0x0FU;
            if (!(header & XHCI_LEGACY_BIOS_OWNED)) return true;
            *capability = header | XHCI_LEGACY_OS_OWNED;
            for (u32 wait = 0; wait < XHCI_WAIT_LIMIT; ++wait) {
                if (!(*capability & XHCI_LEGACY_BIOS_OWNED)) return true;
                arch_pause();
            }
            return false;
        }
        u32 next = XHCI_EXT_CAP_NEXT(header);
        if (!next) break;
        pointer += next * 4U;
    }
    return true;
}

static void ring_command_doorbell(void) {
    __asm__ volatile ("mfence" ::: "memory");
    g_xhci.doorbells[0] = 0U;
}

static void ring_endpoint_doorbell(u32 endpoint) {
    __asm__ volatile ("mfence" ::: "memory");
    if (g_xhci.slot && endpoint) {
        g_xhci.doorbells[g_xhci.slot] = endpoint;
    }
}

static u64 interrupter0(void) {
    return (u64)g_xhci.runtime + XHCI_RUNTIME_INTERRUPTER0_OFFSET;
}

static u32 portsc_neutral(u32 value) {
    return value & ~XHCI_PORTSC_WRITE_ONE_BITS;
}

static void portsc_set_bits(volatile u32 *portsc, u32 bits) {
    *portsc = portsc_neutral(*portsc) | bits;
}

static void portsc_ack_changes(volatile u32 *portsc, u32 bits) {
    *portsc = portsc_neutral(*portsc) | (bits & XHCI_PORTSC_CHANGE_BITS);
}

static u32 mfindex(void) {
    return g_xhci.runtime ?
        (*reg32((u64)g_xhci.runtime, 0x00U) & 0x3FFFU) : 0U;
}

static u32 mf_elapsed(u32 start) {
    return (mfindex() - start) & 0x3FFFU;
}

static bool wait_for_any_connection(u32 ports) {
    u32 start = mfindex();

    do {
        for (u32 port = 0; port < ports; ++port) {
            volatile u32 *portsc =
                reg32((u64)g_xhci.op, 0x400U + port * 0x10U);
            if (*portsc & XHCI_PORTSC_CCS) return true;
        }
        arch_pause();
    } while (mf_elapsed(start) < 2000U);

    return false;
}

static bool reset_port(volatile u32 *portsc, bool superspeed) {
    if (!portsc) return false;

    portsc_ack_changes(portsc, XHCI_PORTSC_CHANGE_BITS);

    if (superspeed && (*portsc & XHCI_PORTSC_PED)) return true;

    u32 reset_bit = superspeed ? XHCI_PORTSC_WPR : XHCI_PORTSC_PR;
    u32 completion_bits = superspeed ?
        (XHCI_PORTSC_WRC | XHCI_PORTSC_PRC) : XHCI_PORTSC_PRC;

    portsc_set_bits(portsc, reset_bit);

    u32 start = mfindex();
    do {
        u32 status = *portsc;

        if ((status & completion_bits) && !(status & reset_bit)) {
            portsc_ack_changes(portsc, completion_bits);
            return (status & XHCI_PORTSC_PED) != 0;
        }

        arch_pause();
    } while (mf_elapsed(start) < 1600U);

    return false;
}

static XhciTrb *command_ring(void) { return (XhciTrb *)phys_to_virt(g_xhci.command_ring_phys); }
static XhciTrb *event_ring(void) { return (XhciTrb *)phys_to_virt(g_xhci.event_ring_phys); }
static XhciTrb *endpoint_ring(void) { return (XhciTrb *)phys_to_virt(g_xhci.endpoint_ring_phys); }
static XhciTrb *ep0_ring(void) { return (XhciTrb *)phys_to_virt(g_xhci.ep0_ring_phys); }

static void ring_command(u64 parameter, u32 control) {
    XhciTrb *ring = command_ring();
    XhciTrb *trb = &ring[g_xhci.command_index++];
    trb->parameter = parameter;
    trb->status = 0;
    trb->control = control | g_xhci.command_cycle;
    if (g_xhci.command_index == XHCI_MAX_TRBS - 1U) {
        ring[g_xhci.command_index].parameter = g_xhci.command_ring_phys;
        ring[g_xhci.command_index].control = TRB_TYPE(TRB_LINK) | TRB_ENT | g_xhci.command_cycle;
        g_xhci.command_index = 0;
        g_xhci.command_cycle ^= TRB_CYCLE;
    }
    ring_command_doorbell();
}

static bool next_event(XhciTrb *event) {
    XhciTrb *ring = event_ring();
    XhciTrb current = ring[g_xhci.event_index];
    if ((current.control & TRB_CYCLE) != g_xhci.event_cycle) return false;
    *event = current;
    if (++g_xhci.event_index == XHCI_MAX_TRBS) {
        g_xhci.event_index = 0;
        g_xhci.event_cycle ^= TRB_CYCLE;
    }
    u64 dequeue = g_xhci.event_ring_phys + (u64)g_xhci.event_index * sizeof(XhciTrb);
    *(volatile u32 *)(interrupter0() + 0x18U) =
        (u32)dequeue | XHCI_ERDP_EHB;
    *(volatile u32 *)(interrupter0() + 0x1CU) =
        (u32)(dequeue >> 32);
    return true;
}

static bool wait_completion(u32 type, u32 *slot_out) {
    for (u32 i = 0; i < XHCI_WAIT_LIMIT; ++i) {
        XhciTrb event;
        if (next_event(&event)) {
            u32 event_type = (event.control >> 10) & 0x3FU;
            if (event_type == type) {
                if (slot_out) *slot_out = (event.control >> 24) & 0xFFU;
                return ((event.status >> 24) & 0xFFU) == 1U;
            }
        }
        arch_pause();
    }
    return false;
}

static bool command(u32 type, u64 parameter, u32 slot_id, u32 *slot) {
    ring_command(parameter, TRB_TYPE(type) | TRB_SLOT_ID(slot_id));
    return wait_completion(TRB_COMMAND_COMPLETION, slot);
}

static void release_current_slot(void) {
    if (!g_xhci.slot) return;

    u32 slot = g_xhci.slot;
    (void)command(TRB_DISABLE_SLOT, 0, slot, 0);

    u64 *dcbaa = (u64 *)phys_to_virt(g_xhci.dcbaa_phys);
    if (dcbaa) {
        dcbaa[slot] = 0;
        __asm__ volatile ("mfence" ::: "memory");
    }

    g_xhci.slot = 0;
}

static bool control_transfer(u8 request, u8 request_type, u16 value, u16 index, u16 length, bool in) {
    u64 setup = (u64)request_type | ((u64)request << 8) | ((u64)value << 16) |
        ((u64)index << 32) | ((u64)length << 48);
    XhciTrb *ring = ep0_ring();
    XhciTrb *trb = &ring[g_xhci.endpoint_index++];
    trb->parameter = setup;
    trb->status = 8U;
    trb->control = TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT |
        (length ? TRB_SETUP_TRANSFER_TYPE(in ? 3U : 2U) : 0U) | TRB_CYCLE;
    if (length) trb->control |= TRB_CHAIN;
    if (length) {
        trb = &ring[g_xhci.endpoint_index++];
        trb->parameter = g_xhci.transfer_buffer_phys;
        trb->status = length;
        trb->control = TRB_TYPE(TRB_DATA_STAGE) | TRB_CHAIN |
            (in ? TRB_DIR : 0U) | TRB_CYCLE;
    }
    trb = &ring[g_xhci.endpoint_index++];
    trb->parameter = 0;
    trb->status = 0;
    trb->control = TRB_TYPE(TRB_STATUS_STAGE) | (in ? 0U : TRB_DIR) | TRB_IOC | TRB_CYCLE;
    ring_endpoint_doorbell(1);
    return wait_completion(TRB_TRANSFER_EVENT, 0);
}

static u16 default_ep0_packet_size(u8 speed) {
    if (speed == 3U) return 64U;
    if (speed >= 4U) return 512U;
    return 8U;
}

static u8 endpoint_interval(u8 speed, u8 b_interval) {
    if (speed <= 2U) {
        u32 interval = 3U;
        u32 period = b_interval ? (u32)b_interval - 1U : 0U;
        while (period) {
            interval++;
            period >>= 1;
        }
        return interval > 15U ? 15U : (u8)interval;
    }
    if (speed >= 4U) return b_interval ? (u8)(b_interval - 1U) : 0U;
    return b_interval > 15U ? 15U : b_interval;
}

static bool configure_keyboard(u8 endpoint_address, u8 interval, u16 max_packet,
                               u8 transactions, u8 speed) {
    u8 *input = (u8 *)phys_to_virt(g_xhci.input_context_phys);
    u32 *control = (u32 *)(void *)input;
    u32 *slot = (u32 *)(void *)(input + XHCI_INPUT_CONTROL_SIZE);
    u32 *ep0 = (u32 *)(void *)(input + XHCI_INPUT_CONTROL_SIZE + g_xhci.context_size);
    u32 endpoint_id = (u32)(endpoint_address & 0x0FU) * 2U +
        ((endpoint_address & 0x80U) ? 1U : 0U);
    u32 *ep = (u32 *)(void *)(input + XHCI_INPUT_CONTROL_SIZE + g_xhci.context_size * endpoint_id);
    g_xhci.endpoint_id = endpoint_id;
    control[1] = (1U << 0) | (1U << 1) | (1U << endpoint_id);
    slot[0] = ((u32)speed << 20) | (endpoint_id << 27);
    slot[1] = (u32)(g_xhci.port + 1U) << 16;
    ep0[1] = ((u32)g_xhci.packet_size << 16) | (3U << 1) | (4U << 3);
        ep0[2] = (u32)g_xhci.ep0_ring_phys | 1U;
        ep0[3] = (u32)(g_xhci.ep0_ring_phys >> 32);
    ep0[4] = 8U;
    ep[0] = (u32)endpoint_interval(speed, interval) << 16 | ((u32)(transactions - 1U) << 8);
    ep[1] = ((u32)max_packet << 16) | (3U << 1) | (7U << 3);
    ep[2] = (u32)g_xhci.endpoint_ring_phys | 1U;
    ep[3] = (u32)(g_xhci.endpoint_ring_phys >> 32);
    ep[4] = (u32)max_packet | ((u32)max_packet * transactions << 16);
    return command(TRB_CONFIGURE_ENDPOINT, g_xhci.input_context_phys,
        g_xhci.slot, 0);
}

static void submit_keyboard_report(void) {
    XhciTrb *ring = endpoint_ring();
    XhciTrb *trb = &ring[g_xhci.transfer_index++];
    trb->parameter = g_xhci.transfer_buffer_phys;
    trb->status = 8U;
    trb->control = TRB_TYPE(TRB_NORMAL) | TRB_IOC | g_xhci.transfer_cycle;
    if (g_xhci.transfer_index == XHCI_MAX_TRBS - 1U) {
        ring[g_xhci.transfer_index].parameter = g_xhci.endpoint_ring_phys;
        ring[g_xhci.transfer_index].control = TRB_TYPE(TRB_LINK) | TRB_ENT | g_xhci.transfer_cycle;
        g_xhci.transfer_index = 0;
        g_xhci.transfer_cycle ^= TRB_CYCLE;
    }
    ring_endpoint_doorbell(g_xhci.endpoint_id);
}

static void prepare_address_context(u8 speed) {
    u8 *input = (u8 *)phys_to_virt(g_xhci.input_context_phys);
    u32 *control = (u32 *)(void *)input;
    u32 *slot = (u32 *)(void *)(input + XHCI_INPUT_CONTROL_SIZE);
    u32 *ep0 = (u32 *)(void *)(input + XHCI_INPUT_CONTROL_SIZE + g_xhci.context_size);
    control[1] = (1U << 0) | (1U << 1);
    slot[0] = ((u32)speed << 20) | (1U << 27);
    slot[1] = (u32)(g_xhci.port + 1U) << 16;
    ep0[1] = ((u32)g_xhci.packet_size << 16) | (3U << 1) | (4U << 3);
    ep0[2] = (u32)g_xhci.ep0_ring_phys | 1U;
    ep0[3] = (u32)(g_xhci.ep0_ring_phys >> 32);
    ep0[4] = 8U;
}

static bool update_ep0_context(void) {
    u8 *input = (u8 *)phys_to_virt(g_xhci.input_context_phys);
    u32 *control = (u32 *)(void *)input;
    u32 *ep0 = (u32 *)(void *)(input + XHCI_INPUT_CONTROL_SIZE + g_xhci.context_size);
    control[0] = 0;
    control[1] = 1U << 1;
    ep0[1] = ((u32)g_xhci.packet_size << 16) | (4U << 3);
    ep0[2] = (u32)g_xhci.ep0_ring_phys | 1U;
    ep0[3] = (u32)(g_xhci.ep0_ring_phys >> 32);
    ep0[4] = 8U;
    return command(TRB_EVALUATE_CONTEXT, g_xhci.input_context_phys,
        g_xhci.slot, 0);
}

static bool find_keyboard_endpoint(const u8 *descriptor, u16 length, u8 *interface_number,
                                   u8 *address, u8 *interval, u16 *packet,
                                   u8 *transactions) {
    bool hid_interface = false;
    u8 current_interface = 0;
    for (u16 offset = 0; offset + 2U <= length;) {
        u8 size = descriptor[offset];
        if (size < 2U || offset + size > length) return false;
        if (descriptor[offset + 1U] == 4U) {
            hid_interface = size >= 9U && descriptor[offset + 5U] == USB_CLASS_HID &&
                descriptor[offset + 6U] == 1U && descriptor[offset + 7U] == 1U;
            if (hid_interface) current_interface = descriptor[offset + 2U];
        }
        if (hid_interface && descriptor[offset + 1U] == 5U && size >= 7U &&
            (descriptor[offset + 2U] & 0x80U) && (descriptor[offset + 3U] & 0x03U) == 0x03U) {
            *interface_number = current_interface;
            *address = descriptor[offset + 2U];
            u16 packet_encoding = descriptor[offset + 4U] | ((u16)descriptor[offset + 5U] << 8);
            *packet = packet_encoding & 0x07FFU;
            *transactions = (u8)(((packet_encoding >> 11) & 0x03U) + 1U);
            *interval = descriptor[offset + 6U];
            return true;
        }
        offset += size;
    }
    return false;
}

static void queue_event(KeyCode key, char character, u8 modifiers) {
    u32 next = (g_queue_head + 1U) % XHCI_QUEUE_SIZE;
    if (next == g_queue_tail) return;
    KeyEvent *event = &g_queue[g_queue_head];
    event->key = key; event->character = character; event->pressed = true;
    event->shift = (modifiers & 0x22U) != 0; event->ctrl = (modifiers & 0x11U) != 0; event->alt = (modifiers & 0x44U) != 0;
    g_queue_head = next;
}

static KeyCode usage_key(u8 usage, char *character) {
    *character = 0;
    if (usage >= 4U && usage <= 29U) { *character = (char)('a' + usage - 4U); return KEY_CHARACTER; }
    if (usage >= 30U && usage <= 38U) { *character = (char)('1' + usage - 30U); return KEY_CHARACTER; }
    if (usage == 39U) { *character = '0'; return KEY_CHARACTER; }
    switch (usage) {
        case 40U: return KEY_ENTER;
        case 41U: return KEY_ESCAPE;
        case 42U: return KEY_BACKSPACE;
        case 43U: return KEY_TAB;
        case 44U: *character = ' '; return KEY_CHARACTER;
        case 45U: *character = '-'; return KEY_CHARACTER;
        case 46U: *character = '='; return KEY_CHARACTER;
        case 47U: *character = '['; return KEY_CHARACTER;
        case 48U: *character = ']'; return KEY_CHARACTER;
        case 54U: *character = ','; return KEY_CHARACTER;
        case 55U: *character = '.'; return KEY_CHARACTER;
        case 56U: *character = '/'; return KEY_CHARACTER;
        case 74U: return KEY_HOME;
        case 75U: return KEY_PAGE_UP;
        case 76U: return KEY_DELETE;
        case 77U: return KEY_END;
        case 78U: return KEY_PAGE_DOWN;
        case 79U: return KEY_RIGHT;
        case 80U: return KEY_LEFT;
        case 81U: return KEY_DOWN;
        case 82U: return KEY_UP;
        default: return KEY_NONE;
    }
}

static void decode_report(void) {
    u8 modifiers = g_xhci.report[0];
    for (u32 i = 2; i < 8U; ++i) {
        u8 usage = g_xhci.report[i];
        if (!usage) continue;
        bool was_down = false;
        for (u32 j = 0; j < 6U; ++j) if (g_xhci.previous[j] == usage) was_down = true;
        if (was_down) continue;
        char character = 0;
        KeyCode key = usage_key(usage, &character);
        if (key != KEY_NONE) {
            if ((modifiers & 0x22U) && character >= 'a' && character <= 'z') character = (char)(character - 'a' + 'A');
            queue_event(key, character, modifiers);
        }
    }
    k_memcpy(g_xhci.previous, &g_xhci.report[2], 6U);
}

bool xhci_init(VmPageMap *kernel_map) {
    clear_state();
    if (!kernel_map) return xhci_fail("no kernel map");
    const PciDevice *device = 0;
    for (u32 i = 0; i < pci_device_count(); ++i) {
        const PciDevice *candidate = pci_device(i);
        if (candidate && candidate->class_code == XHCI_CLASS && candidate->subclass == XHCI_SUBCLASS && candidate->prog_if == XHCI_PROGIF) { device = candidate; break; }
    }
    if (!device) return xhci_fail("no controller");
    PciBar bar;
    if (!pci_read_bar(device, 0, &bar) || (bar.type != PCI_BAR_MMIO32 && bar.type != PCI_BAR_MMIO64) || !bar.base) return xhci_fail("invalid BAR0");
    u64 base = bar.base & ~(VM_PAGE_SIZE - 1ULL);
    if (!vmm_identity_map_range(kernel_map, base, XHCI_MMIO_SIZE,
            VM_WRITE | VM_UNCACHED)) return xhci_fail("MMIO map");
    pci_enable_memory_space(device); pci_enable_bus_master(device);
    g_xhci.mmio = (volatile u8 *)(u64)base;
    u32 cap_length = g_xhci.mmio[0];
    u32 hcs1 = *reg32((u64)g_xhci.mmio, 0x04U);
    u32 hcs2 = *reg32((u64)g_xhci.mmio, 0x08U);
    u32 hcc = *reg32((u64)g_xhci.mmio, 0x10U);
    u32 doorbell_offset =
        *reg32((u64)g_xhci.mmio, 0x14U) & ~3U;
    u32 runtime_offset =
        *reg32((u64)g_xhci.mmio, 0x18U) & ~0x1FU;

    g_xhci.max_slots = hcs1 & 0xFFU;
    g_xhci.max_ports = (hcs1 >> 24) & 0xFFU;
    g_xhci.context_size = (hcc & XHCI_HCC_CSZ) ? 64U : 32U;
    g_xhci.addr64 = (hcc & XHCI_HCC_AC64) != 0;

    u64 port_register_end =
        (u64)cap_length + 0x400ULL + (u64)g_xhci.max_ports * 0x10ULL;
    u64 doorbell_end =
        (u64)doorbell_offset + ((u64)g_xhci.max_slots + 1ULL) * sizeof(u32);

    if (!cap_length || cap_length >= XHCI_MMIO_SIZE ||
        !g_xhci.max_slots || !g_xhci.max_ports ||
        g_xhci.max_ports > XHCI_MAX_PORTS ||
        port_register_end > XHCI_MMIO_SIZE ||
        doorbell_end > XHCI_MMIO_SIZE ||
        runtime_offset > XHCI_MMIO_SIZE - 0x40U ||
        !release_legacy_control(base, hcc)) {
        return xhci_fail("capability or legacy handoff");
    }

    discover_port_protocols(base, hcc);

    g_xhci.op = (volatile u32 *)(u64)(base + cap_length);
    g_xhci.doorbells =
        (volatile u32 *)(u64)(base + doorbell_offset);
    g_xhci.runtime =
        (volatile u32 *)(u64)(base + runtime_offset);

    if (!(*reg32((u64)g_xhci.op, 0x08U) & 1U)) {
        return xhci_fail("4K page unsupported");
    }
    volatile u32 *usbcmd = reg32((u64)g_xhci.op, 0x00U);
    *usbcmd &= ~XHCI_USBCMD_RUN;
    if (!wait_condition(controller_halted)) return xhci_fail("controller halt");
    *usbcmd |= XHCI_USBCMD_HCRST;
    for (u32 wait = 0; wait < XHCI_WAIT_LIMIT && (*usbcmd & XHCI_USBCMD_HCRST); ++wait) arch_pause();
    if (*usbcmd & XHCI_USBCMD_HCRST) return xhci_fail("controller reset");
    if (!wait_condition(controller_not_ready)) return xhci_fail("controller ready");
    u64 dcbaa = 0, input = 0, device_context = 0, command_ring_phys = 0, event_ring_phys = 0, event_table = 0, ep0_ring_phys = 0, endpoint_ring_phys = 0, buffer = 0;
    if (!dma_page(&dcbaa) || !dma_page(&input) || !dma_page(&device_context) || !dma_page(&command_ring_phys) || !dma_page(&event_ring_phys) || !dma_page(&event_table) || !dma_page(&ep0_ring_phys) || !dma_page(&endpoint_ring_phys) || !dma_page(&buffer)) return xhci_fail("DMA allocation");
    g_xhci.dcbaa_phys = dcbaa; g_xhci.input_context_phys = input; g_xhci.device_context_phys = device_context; g_xhci.command_ring_phys = command_ring_phys;
    g_xhci.event_ring_phys = event_ring_phys; g_xhci.event_table_phys = event_table; g_xhci.ep0_ring_phys = ep0_ring_phys; g_xhci.endpoint_ring_phys = endpoint_ring_phys; g_xhci.transfer_buffer_phys = buffer;
    if (!setup_scratchpads(hcs2)) return xhci_fail("scratchpad allocation");
    ((XhciTrb *)phys_to_virt(command_ring_phys))[XHCI_MAX_TRBS - 1U].parameter = command_ring_phys;
    ((XhciTrb *)phys_to_virt(command_ring_phys))[XHCI_MAX_TRBS - 1U].control = (6U << 10) | 2U | TRB_CYCLE;
    *reg32((u64)g_xhci.op, 0x30U) = (u32)dcbaa; *reg32((u64)g_xhci.op, 0x34U) = (u32)(dcbaa >> 32);
    *reg32((u64)g_xhci.op, 0x18U) = (u32)command_ring_phys | 1U; *reg32((u64)g_xhci.op, 0x1CU) = (u32)(command_ring_phys >> 32);
    *reg32((u64)g_xhci.op, 0x38U) = g_xhci.max_slots;

    u32 *event_table_words = (u32 *)phys_to_virt(event_table);
    event_table_words[0] = (u32)event_ring_phys;
    event_table_words[1] = (u32)(event_ring_phys >> 32);
    event_table_words[2] = XHCI_MAX_TRBS;
    event_table_words[3] = 0;

    *(volatile u32 *)(interrupter0() + 0x08U) = 1U;
    *(volatile u32 *)(interrupter0() + 0x10U) = (u32)event_table;
    *(volatile u32 *)(interrupter0() + 0x14U) = (u32)(event_table >> 32);
    *(volatile u32 *)(interrupter0() + 0x18U) =
        (u32)event_ring_phys | XHCI_ERDP_EHB;
    *(volatile u32 *)(interrupter0() + 0x1CU) =
        (u32)(event_ring_phys >> 32);
    __asm__ volatile ("mfence" ::: "memory");
    g_xhci.command_cycle = 1U; g_xhci.event_cycle = 1U; g_xhci.transfer_cycle = 1U; g_xhci.initialized = true;
    *usbcmd |= XHCI_USBCMD_RUN;
    if (!wait_condition(controller_not_ready) || controller_halted()) return xhci_fail("controller start");
    u32 ports = g_xhci.max_ports;

    if (hcc & XHCI_HCC_PPC) {
        for (u32 port = 0; port < ports; ++port) {
            volatile u32 *portsc =
                reg32((u64)g_xhci.op, 0x400U + port * 0x10U);
            if (!(*portsc & XHCI_PORTSC_PP)) {
                portsc_set_bits(portsc, XHCI_PORTSC_PP);
            }
        }
    }

    bool connected_port = wait_for_any_connection(ports);

    for (u32 port = 0; port < ports; ++port) {
        volatile u32 *portsc =
            reg32((u64)g_xhci.op, 0x400U + port * 0x10U);

        if (!(*portsc & XHCI_PORTSC_CCS)) continue;
        connected_port = true;

        bool superspeed = g_xhci.port_protocol[port] >= 3U;
        if (!reset_port(portsc, superspeed)) {
            xhci_report_failure_detail("port reset or enable", *portsc);
            continue;
        }

        g_xhci.port = port;
        u32 slot = 0;

        if (!command(TRB_ENABLE_SLOT, 0, 0, &slot) ||
            !slot || slot > g_xhci.max_slots) {
            xhci_report_failure("enable slot");
            continue;
        }

        g_xhci.slot = slot;

        u8 speed = (u8)((*portsc >> 10) & 0x0FU);
        g_xhci.packet_size = default_ep0_packet_size(speed);
        g_xhci.endpoint_index = 0U;
        g_xhci.transfer_index = 0U;
        g_xhci.transfer_cycle = 1U;

        k_memset(phys_to_virt(g_xhci.input_context_phys), 0,
            (usize)FRAME_SIZE);
        k_memset(phys_to_virt(g_xhci.device_context_phys), 0,
            (usize)FRAME_SIZE);
        k_memset(phys_to_virt(g_xhci.ep0_ring_phys), 0,
            (usize)FRAME_SIZE);
        k_memset(phys_to_virt(g_xhci.endpoint_ring_phys), 0,
            (usize)FRAME_SIZE);

        u64 *dcbaa_entries =
            (u64 *)phys_to_virt(g_xhci.dcbaa_phys);
        dcbaa_entries[g_xhci.slot] = g_xhci.device_context_phys;
        __asm__ volatile ("mfence" ::: "memory");

        prepare_address_context(speed);

        if (!command(TRB_ADDRESS_DEVICE, g_xhci.input_context_phys,
                g_xhci.slot, 0)) {
            xhci_report_failure("address device");
            release_current_slot();
            continue;
        }
        u8 *descriptor = (u8 *)phys_to_virt(buffer);
        if (!control_transfer(USB_REQ_GET_DESCRIPTOR, 0x80U, USB_DESC_DEVICE << 8, 0, 8, true)) {
            xhci_report_failure("device descriptor");
            release_current_slot();
            continue;
        }
        g_xhci.packet_size = descriptor[7] ? descriptor[7] : 8U;
        if (((*portsc >> 10) & 0x0FU) >= 4U) g_xhci.packet_size = (u16)(1U << g_xhci.packet_size);
        if (!update_ep0_context()) {
            xhci_report_failure("update control endpoint");
            release_current_slot();
            continue;
        }
        if (!control_transfer(USB_REQ_GET_DESCRIPTOR, 0x80U, USB_DESC_CONFIGURATION << 8, 0, 9, true)) {
            xhci_report_failure("configuration header");
            release_current_slot();
            continue;
        }
        u16 configuration_length = descriptor[2] | ((u16)descriptor[3] << 8);
        if (configuration_length > 4096U || configuration_length < 9U) {
            xhci_report_failure("configuration length");
            release_current_slot();
            continue;
        }
        if (!control_transfer(USB_REQ_GET_DESCRIPTOR, 0x80U, USB_DESC_CONFIGURATION << 8, 0, configuration_length, true)) {
            xhci_report_failure("configuration descriptor");
            release_current_slot();
            continue;
        }
        u8 interface_number = 0, endpoint_address = 0, interval = 0, transactions = 1U; u16 max_packet = 0;
        if (!find_keyboard_endpoint(descriptor, configuration_length, &interface_number,
                        &endpoint_address, &interval, &max_packet, &transactions)) {
            xhci_report_failure("boot keyboard endpoint");
            release_current_slot();
            continue;
        }
        if (!control_transfer(USB_REQ_SET_CONFIGURATION, 0x00U, descriptor[5], 0, 0, false)) {
            xhci_report_failure("set configuration");
            release_current_slot();
            continue;
        }
        if (!control_transfer(USB_REQ_SET_PROTOCOL, 0x21U, 0, interface_number, 0, false)) {
            xhci_report_failure("set boot protocol");
            release_current_slot();
            continue;
        }
        if (!configure_keyboard(endpoint_address, interval, max_packet, transactions,
                    (u8)((*portsc >> 10) & 0x0FU))) {
            xhci_report_failure("configure keyboard endpoint");
            release_current_slot();
            continue;
        }
        submit_keyboard_report();
        g_xhci.keyboard_ready = true;
        break;
    }
    if (!g_xhci.keyboard_ready) {
        if (connected_port) xhci_report_failure("no boot keyboard found");
        else {
            xhci_report_failure_detail("no connected USB port", ports);
            for (u32 port = 0; port < ports; ++port) {
                volatile u32 *portsc = reg32((u64)g_xhci.op, 0x400U + port * 0x10U);
                xhci_report_failure_detail("port status", *portsc);
            }
        }
    }
    return g_xhci.keyboard_ready;
}

void xhci_poll(void) {
    if (!g_xhci.keyboard_ready) return;
    XhciTrb event;
    while (next_event(&event)) {
        if (((event.control >> 10) & 0x3FU) != TRB_TRANSFER_EVENT) continue;
        k_memcpy(g_xhci.report, phys_to_virt(g_xhci.transfer_buffer_phys), 8U);
        decode_report();
           submit_keyboard_report();
    }
}

bool xhci_present(void) { return g_xhci.keyboard_ready; }

u32 xhci_failure_count(void) { return g_failure_count; }

const XhciFailureRecord *xhci_failure(u32 index) {
    return index < g_failure_count ? &g_failures[index] : 0;
}

bool xhci_get_event(KeyEvent *event) {
    if (!event || g_queue_tail == g_queue_head) return false;
    *event = g_queue[g_queue_tail];
    g_queue_tail = (g_queue_tail + 1U) % XHCI_QUEUE_SIZE;
    return true;
}