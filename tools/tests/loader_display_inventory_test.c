/* Host-side firmware doubles exercise the real optional capture routine.
 * No OS/PCI device access is performed. All descriptors and IDs are fixtures. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../loader/display_inventory.h"
#include "../../include/boot_display.h"

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "loader inventory: line %d: %s\n", __LINE__, #expression); \
        return 0; \
    } \
} while (0)

typedef struct {
    EFI_PCI_IO_PROTOCOL io;
    u32 config[16];
    UINTN location[4];
    u8 resources[6][BOOT_DISPLAY_BAR_RESOURCE_BYTES + 46U];
    u32 resource_size[6];
    EFI_STATUS resource_status;
    int empty_resource;
    int reads;
    int changed;
    int error_resource;
    u32 queried;
} MockPci;

typedef struct MockHandle {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    MockPci *pci;
    struct MockHandle *parent;
    EFI_EDID_ACTIVE_PROTOCOL *edid;
    int gop_error;
} MockHandle;

static EFI_BOOT_SERVICES g_services;
static EFI_GRAPHICS_OUTPUT_PROTOCOL g_selected, g_other;
static MockPci g_rx, g_second;
static MockHandle g_handles[3], g_parent;
static EFI_EDID_ACTIVE_PROTOCOL g_edid_protocol;
static u8 g_edid[128];
static UINTN g_count;
static int g_allocations, g_writes, g_paths, g_locate_error;
static u32 g_cases;

static void put64(u8 *bytes, u32 offset, u64 value) {
    for (u32 i = 0; i < 8U; ++i) bytes[offset + i] = (u8)(value >> (i * 8U));
}

static void resource(u8 *bytes, u64 base, u64 length, u64 granularity) {
    memset(bytes, 0, 48U);
    bytes[0] = 0x8AU;
    bytes[1] = 43U;
    bytes[46] = 0x79U;
    put64(bytes, 6U, granularity);
    put64(bytes, 14U, base);
    put64(bytes, 22U, length - 1U); /* firmware alignment field, not a range end */
    put64(bytes, 38U, length);
}

static EFI_STATUS config_read(EFI_PCI_IO_PROTOCOL *io, u32 width, u32 offset,
    UINTN count, void *out) {
    MockPci *mock = (MockPci *)io;
    if (width != 2U || offset || count != 16U) return EFI_INVALID_PARAMETER;
    memcpy(out, mock->config, sizeof(mock->config));
    if (++mock->reads > 1 && mock->changed) ((u32 *)out)[4] ^= 0x10000000U;
    return EFI_SUCCESS;
}

static EFI_STATUS forbidden_write(EFI_PCI_IO_PROTOCOL *io, u32 width, u32 offset,
    UINTN count, void *out) {
    (void)io; (void)width; (void)offset; (void)count; (void)out;
    ++g_writes;
    return EFI_UNSUPPORTED;
}

static EFI_STATUS location(EFI_PCI_IO_PROTOCOL *io, UINTN *segment, UINTN *bus,
    UINTN *device, UINTN *function) {
    MockPci *mock = (MockPci *)io;
    *segment = mock->location[0];
    *bus = mock->location[1];
    *device = mock->location[2];
    *function = mock->location[3];
    return EFI_SUCCESS;
}

static EFI_STATUS attributes(EFI_PCI_IO_PROTOCOL *io, u8 index, u64 *supports, void **out) {
    MockPci *mock = (MockPci *)io;
    if (!supports || index >= 6U || !out) return EFI_INVALID_PARAMETER;
    *supports = 0U;
    mock->queried |= 1U << index;
    if (mock->empty_resource) {
        *out = 0;
        return EFI_SUCCESS;
    }
    if (mock->error_resource) {
        *out = (void *)1; /* failure output is unspecified and must not be freed */
        return mock->resource_status ? mock->resource_status : EFI_UNSUPPORTED;
    }
    if (!mock->resources[index][0]) return EFI_UNSUPPORTED;
    *out = malloc(mock->resource_size[index]);
    if (!*out) return EFIERR(9);
    ++g_allocations;
    memcpy(*out, mock->resources[index], mock->resource_size[index]);
    return mock->resource_status;
}

static EFI_STATUS handles(u32 type, EFI_GUID *guid, void *key, UINTN *count, EFI_HANDLE **out) {
    if (type != 2U || guid->Data1 != 0x9042A9DEU || key) return EFI_INVALID_PARAMETER;
    if (g_locate_error) {
        *out = (EFI_HANDLE *)1;
        return EFI_NOT_FOUND;
    }
    *count = g_count;
    if (!g_count) return EFI_SUCCESS;
    *out = malloc((size_t)g_count * sizeof(**out));
    if (!*out) return EFIERR(9);
    ++g_allocations;
    for (UINTN i = 0; i < g_count; ++i) (*out)[i] = i < 3U ? (EFI_HANDLE)&g_handles[i] : 0;
    return EFI_SUCCESS;
}

static EFI_STATUS protocol(EFI_HANDLE handle, EFI_GUID *guid, void **out) {
    MockHandle *mock = (MockHandle *)handle;
    *out = 0;
    if (!mock) return EFI_NOT_FOUND;
    switch (guid->Data1) {
        case 0x9042A9DEU:
            if (mock->gop_error) return EFI_NOT_FOUND;
            *out = mock->gop;
            break;
        case 0x4CF5B200U:
            *out = mock->pci ? &mock->pci->io : 0;
            break;
        case 0x09576E91U:
            *out = mock->parent ? mock : 0;
            break;
        case 0xBD8C1056U:
            *out = mock->edid;
            break;
        default:
            return EFI_NOT_FOUND;
    }
    return *out ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static EFI_STATUS path(EFI_GUID *guid, void **device_path, EFI_HANDLE *parent) {
    if (guid->Data1 != 0x4CF5B200U || !device_path || !*device_path) return EFI_NOT_FOUND;
    MockHandle *mock = (MockHandle *)*device_path;
    *parent = mock->parent;
    ++g_paths;
    return *parent ? EFI_SUCCESS : EFI_NOT_FOUND;
}

static EFI_STATUS free_pool(void *allocation) {
    if (allocation) {
        free(allocation);
        --g_allocations;
    }
    return EFI_SUCCESS;
}

static void fixture(void) {
    memset(&g_services, 0, sizeof(g_services));
    memset(&g_rx, 0, sizeof(g_rx));
    memset(&g_second, 0, sizeof(g_second));
    memset(g_handles, 0, sizeof(g_handles));
    memset(&g_parent, 0, sizeof(g_parent));
    memset(g_edid, 0, sizeof(g_edid));
    g_services.HandleProtocol = protocol;
    g_services.LocateHandleBuffer = handles;
    g_services.LocateDevicePath = path;
    g_services.FreePool = free_pool;
    g_rx.io.Pci.Read = config_read;
    g_rx.io.Pci.Write = forbidden_write;
    g_rx.io.GetLocation = location;
    g_rx.io.GetBarAttributes = attributes;
    g_rx.config[0] = 0x67DF1002U;
    g_rx.config[1] = 7U;
    g_rx.config[2] = 0x030000EFU;
    g_rx.config[4] = 0x5000000CU;
    g_rx.config[9] = 0x63400000U;
    g_rx.config[11] = 0x23101458U;
    g_rx.location[1] = 1U;
    for (u32 i = 0U; i < 6U; ++i) g_rx.resource_size[i] = 48U;
    resource(g_rx.resources[0], 0x50000000ULL, 0x10000000ULL, 64U);
    resource(g_rx.resources[5], 0x63400000ULL, 0x40000ULL, 32U);
    g_second = g_rx;
    g_second.config[0] = 0x1C8210DEU;
    g_second.location[1] = 3U;
    g_handles[0].gop = &g_other;
    g_handles[0].pci = &g_second;
    g_handles[1].gop = &g_selected;
    g_handles[1].pci = &g_rx;
    for (u32 i = 1U; i < 7U; ++i) g_edid[i] = 0xFFU;
    g_edid[18] = 1U;
    g_edid[19] = 4U;
    u8 sum = 0U;
    for (u32 i = 0U; i < 127U; ++i) sum = (u8)(sum + g_edid[i]);
    g_edid[127] = (u8)(0U - sum);
    g_edid_protocol.SizeOfEdid = sizeof(g_edid);
    g_edid_protocol.Edid = g_edid;
    g_handles[1].edid = &g_edid_protocol;
    g_count = 2U;
    g_writes = g_paths = g_locate_error = 0;
}

static int capture(BootInfo *boot) {
    memset(boot, 0xA5, sizeof(*boot)); /* prove optional failures clear old evidence */
    boot->framebuffer_mode = 0U; /* zero is a valid firmware mode index */
    boot->framebuffer_base = 0x50000000ULL;
    loader_capture_display_inventory(&g_services, boot, &g_selected);
    ++g_cases;
    CHECK(!g_allocations && !g_writes && !g_second.reads);
    CHECK(boot->framebuffer_mode == 0U && boot->framebuffer_base == 0x50000000ULL);
    CHECK(!boot->reserved1 && !boot->framebuffer_metadata_reserved && !boot->framebuffer_edid_reserved);
    for (u32 i = 0U; i < 6U; ++i) {
        CHECK(!boot->framebuffer_bar_audit[i].reserved);
        CHECK(boot->framebuffer_bar_queries[i].prefix_size <= BOOT_DISPLAY_QUERY_PREFIX_BYTES);
        if (boot->framebuffer_bar_audit[i].result == BOOT_DISPLAY_BAR_ACCEPTED) {
            u32 source = boot->framebuffer_bar_query_index[i];
            CHECK(source < 6U);
            CHECK(boot->framebuffer_bar_queries[source].matching_bars == (1U << i));
        } else {
            CHECK(boot->framebuffer_bar_query_index[i] == BOOT_DISPLAY_QUERY_NONE);
        }
        if (boot->framebuffer_bar_audit[i].result != BOOT_DISPLAY_BAR_ACCEPTED)
            CHECK(!boot->framebuffer_bars[i].flags && !boot->framebuffer_bars[i].base &&
                !boot->framebuffer_bars[i].size);
    }
    return 1;
}

static int selected_and_child(void) {
    BootInfo boot;
    fixture();
    CHECK(capture(&boot));
    CHECK(boot.framebuffer_pci_valid && boot.framebuffer_pci_bus == 1U);
    CHECK(boot.framebuffer_pci_vendor_device == 0x67DF1002U &&
        boot.framebuffer_pci_subsystem == 0x23101458U);
    CHECK(boot.framebuffer_bars[0].size == 0x10000000ULL &&
        boot.framebuffer_bars[5].size == 0x40000ULL && !boot.framebuffer_bars[1].flags);
    CHECK(boot.framebuffer_bar_audit[0].result == BOOT_DISPLAY_BAR_ACCEPTED &&
        boot.framebuffer_bar_audit[0].descriptor_count == 1U &&
        boot.framebuffer_bar_audit[0].reported_base == 0x50000000ULL);
    CHECK(boot.framebuffer_bar_audit[1].result == BOOT_DISPLAY_BAR_CONTINUATION &&
        boot.framebuffer_bar_audit[2].result == BOOT_DISPLAY_BAR_UNASSIGNED);
    CHECK(g_rx.queried == 0x3FU);
    CHECK(boot.framebuffer_edid_size == 128U && !memcmp(boot.framebuffer_edid, g_edid, 128U));
    CHECK(g_rx.config[1] == 7U && g_rx.config[4] == 0x5000000CU);
    fixture();
    g_handles[1].pci = 0;
    g_handles[1].parent = &g_parent;
    g_parent.pci = &g_rx;
    CHECK(capture(&boot) && boot.framebuffer_pci_valid && g_paths == 1);
    fixture();
    g_handles[1].pci = 0;
    g_handles[1].parent = &g_parent;
    g_parent.pci = &g_rx;
    g_rx.config[2] = 0x06040000U; /* bridge ancestor is not a display owner */
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid && !g_rx.queried);
    fixture();
    g_rx.location[0] = 2U; /* retain segment, never alias it to zero */
    CHECK(capture(&boot) && boot.framebuffer_pci_valid && boot.framebuffer_pci_segment == 2U);
    return 1;
}

static int association_failures(void) {
    BootInfo boot;
    fixture();
    g_count = 3U;
    g_handles[2].gop = &g_selected;
    g_handles[2].pci = &g_second;
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid && !boot.framebuffer_edid_size && !g_rx.reads);
    fixture();
    g_handles[0].gop_error = 1;
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid && !boot.framebuffer_edid_size);
    fixture();
    g_handles[1].gop = &g_other;
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid && !boot.framebuffer_edid_size);
    fixture();
    g_count = 65U;
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid && !g_rx.reads);
    fixture();
    g_count = 0U;
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid);
    fixture();
    g_locate_error = 1;
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid);
    const UINTN bad[] = {0x100000000ULL, 256U, 32U, 8U};
    for (u32 i = 0U; i < 4U; ++i) {
        fixture();
        g_rx.location[i] = bad[i];
        CHECK(capture(&boot) && !boot.framebuffer_pci_valid && !g_rx.queried);
    }
    fixture();
    g_rx.config[3] = 1U << 16U; /* unsupported PCI header */
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid);
    fixture();
    g_rx.changed = 1;
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid && !boot.framebuffer_bars[0].flags);
    CHECK(boot.framebuffer_bar_audit[0].result == BOOT_DISPLAY_BAR_CONFIG_CHANGED);
    return 1;
}

static int optional_failures(void) {
    BootInfo boot;
    fixture();
    g_rx.io.GetBarAttributes = 0;
    CHECK(capture(&boot) && boot.framebuffer_pci_valid && !boot.framebuffer_bars[0].flags);
    fixture();
    g_rx.error_resource = 1;
    CHECK(capture(&boot) && boot.framebuffer_pci_valid && !boot.framebuffer_bars[0].flags);
    fixture();
    g_rx.resources[5][0] = 0x87U; /* unsupported descriptor form */
    CHECK(capture(&boot) && boot.framebuffer_pci_valid && !boot.framebuffer_bars[5].flags);
    fixture();
    put64(g_rx.resources[0], 30U, 0x1000ULL); /* translated aperture unsupported */
    CHECK(capture(&boot) && boot.framebuffer_pci_valid && !boot.framebuffer_bars[0].flags);
    fixture();
    g_edid[127]++;
    CHECK(capture(&boot) && boot.framebuffer_pci_valid && !boot.framebuffer_edid_size);
    fixture();
    g_edid_protocol.SizeOfEdid = 127U;
    g_edid_protocol.Edid = (u8 *)1; /* short buffers must not be read */
    CHECK(capture(&boot) && !boot.framebuffer_edid_size);
    fixture();
    g_handles[0].edid = g_handles[1].edid;
    g_handles[1].edid = 0; /* never borrow EDID from a different GOP */
    CHECK(capture(&boot) && boot.framebuffer_pci_valid && !boot.framebuffer_edid_size);
    fixture();
    g_services.LocateDevicePath = 0;
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid && !boot.framebuffer_edid_size);
    return 1;
}

static void resource_list(u32 index, u32 count, u64 part_size) {
    for (u32 i = 0U; i < count; ++i)
        resource(g_rx.resources[index] + i * 46U, 0x63400000ULL + i * part_size, part_size, 32U);
    g_rx.resource_size[index] = 46U * count + 2U;
}

static int bar_audit_and_lists(void) {
    BootInfo boot;
    fixture();
    resource_list(5U, 2U, 0x20000ULL);
    CHECK(capture(&boot) && boot.framebuffer_bars[5].size == 0x40000ULL &&
        boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_ACCEPTED &&
        boot.framebuffer_bar_audit[5].descriptor_count == 2U);
    const u64 bad_bases[] = {0x63430000ULL, 0x63410000ULL};
    for (u32 i = 0U; i < 2U; ++i) {
        fixture();
        resource_list(5U, 2U, 0x20000ULL);
        put64(g_rx.resources[5] + 46U, 14U, bad_bases[i]);
        CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_NONCONTIGUOUS &&
            boot.framebuffer_bar_audit[5].reported_base == bad_bases[i]);
    }
    fixture();
    resource_list(5U, 2U, 0x20000ULL);
    put64(g_rx.resources[5] + 46U, 30U, 0x1000ULL);
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_TRANSLATED &&
        boot.framebuffer_bar_audit[5].translation == 0x1000ULL);
    fixture();
    resource_list(5U, 2U, 0x20000ULL);
    put64(g_rx.resources[5] + 46U, 6U, 64U);
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_TYPE_MISMATCH &&
        boot.framebuffer_bar_audit[5].granularity == 64U);
    fixture();
    resource_list(5U, 2U, 0x20000ULL);
    g_rx.resources[5][46U + 3U] = 1U;
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_TYPE_MISMATCH);
    fixture();
    resource_list(5U, BOOT_DISPLAY_BAR_DESCRIPTOR_LIMIT, 0x8000ULL);
    CHECK(capture(&boot) && boot.framebuffer_bars[5].size == 0x40000ULL &&
        boot.framebuffer_bar_audit[5].descriptor_count == BOOT_DISPLAY_BAR_DESCRIPTOR_LIMIT);
    fixture();
    resource_list(5U, BOOT_DISPLAY_BAR_DESCRIPTOR_LIMIT + 1U, 0x8000ULL);
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_LIST_LIMIT &&
        boot.framebuffer_bar_audit[5].descriptor_count == BOOT_DISPLAY_BAR_DESCRIPTOR_LIMIT &&
        boot.framebuffer_bar_audit[5].reported_base == 0x63438000ULL);
    fixture();
    g_rx.resources[5][1] = 42U;
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_DESCRIPTOR &&
        boot.framebuffer_bar_audit[5].descriptor_length == 42U);
    fixture();
    g_rx.resources[5][0] = 0x22U; /* unknown small item: do not read a large-item header */
    g_rx.resource_size[5] = 1U;
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_DESCRIPTOR &&
        boot.framebuffer_bar_audit[5].descriptor_tag == 0x22U);
    fixture();
    g_rx.resources[5][0] = 0x79U; /* success with no address descriptors */
    g_rx.resources[5][1] = 0U;
    g_rx.resource_size[5] = 2U;
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_EMPTY_LIST);
    fixture();
    g_rx.empty_resource = 1;
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_EMPTY_RESPONSE);
    fixture();
    g_rx.io.GetBarAttributes = 0;
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_NO_PROTOCOL);
    const EFI_STATUS errors[] = {EFI_UNSUPPORTED, EFI_INVALID_PARAMETER, EFIERR(7)};
    for (u32 i = 0U; i < 3U; ++i) {
        fixture();
        g_rx.error_resource = 1;
        g_rx.resource_status = errors[i];
        CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_QUERY_FAILED &&
            boot.framebuffer_bar_audit[5].query_status == errors[i]);
    }
    fixture();
    g_rx.resource_status = 1U; /* a non-error status must survive decoder initialization */
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_ACCEPTED &&
        boot.framebuffer_bar_audit[5].query_status == 1U);
    fixture();
    resource_list(5U, 2U, 0x20000ULL);
    u8 sum = 0U;
    for (u32 i = 0; i < g_rx.resource_size[5] - 1U; ++i) sum = (u8)(sum + g_rx.resources[5][i]);
    g_rx.resources[5][g_rx.resource_size[5] - 1U] = (u8)(0U - sum);
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_ACCEPTED);
    g_rx.resources[5][14U] ^= 1U; /* invalid checksum, leaving a supported list */
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result != BOOT_DISPLAY_BAR_ACCEPTED);
    fixture();
    g_rx.resources[5][47U] = 1U;
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_CHECKSUM);
    fixture();
    put64(g_rx.resources[5], 14U, 0x63300000ULL);
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_BASE_MISMATCH);
    fixture();
    put64(g_rx.resources[5], 38U, 0x30000ULL);
    CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_EXTENT);
    fixture();
    g_rx.config[8] = 0x4001U;
    resource(g_rx.resources[4], 0x4000ULL, 0x100ULL, 0U);
    g_rx.resources[4][3] = 1U;
    CHECK(capture(&boot) && boot.framebuffer_bars[4].size == 0x100ULL &&
        boot.framebuffer_bars[4].flags == BOOT_DISPLAY_BAR_VALID);
    return 1;
}

static int bounded_decoder(void) {
    u8 bytes[BOOT_DISPLAY_BAR_RESOURCE_BYTES];
    memset(bytes, 0, sizeof(bytes));
    resource(bytes, 0x63400000ULL, 0x40000ULL, 32U);
    for (u32 size = 0U; size <= sizeof(bytes); ++size) {
        BootDisplayBar bar;
        BootDisplayBarAudit audit;
        memset(&bar, 0xA5, sizeof(bar));
        int ok = boot_display_decode_bar_audited(bytes, size, 0x63400000U, 0U, 5U, &bar, &audit);
        CHECK(ok == (size == 48U));
        CHECK(ok || (!bar.flags && !bar.base && !bar.size));
    }
    puts("Bounded decoder: 371 prefix/trailing-length cases PASS");
    return 1;
}

static void packed_fixture(void) {
    fixture();
    memset(g_rx.resources, 0, sizeof(g_rx.resources));
    g_rx.config[6] = 0x6320000CU;
    g_rx.config[8] = 0x4001U;
    resource(g_rx.resources[0], 0x50000000ULL, 0x10000000ULL, 64U);
    resource(g_rx.resources[1], 0x63200000ULL, 0x200000ULL, 64U);
    resource(g_rx.resources[2], 0x4000ULL, 0x100ULL, 32U);
    g_rx.resources[2][3] = 1U;
    resource(g_rx.resources[3], 0x63400000ULL, 0x40000ULL, 32U);
    for (u32 i = 4U; i < 6U; ++i) {
        g_rx.resources[i][0] = 0x79U;
        g_rx.resource_size[i] = 2U;
    }
}

static int query_numbering_and_ambiguity(void) {
    BootInfo boot;
    packed_fixture();
    CHECK(capture(&boot) && g_rx.queried == 0x3FU);
    CHECK(boot.framebuffer_bars[2].size == 0x200000ULL &&
        boot.framebuffer_bars[4].size == 0x100ULL &&
        boot.framebuffer_bars[5].size == 0x40000ULL);
    CHECK(boot.framebuffer_bar_query_index[2] == 1U &&
        boot.framebuffer_bar_query_index[4] == 2U &&
        boot.framebuffer_bar_query_index[5] == 3U);
    CHECK(boot.framebuffer_bar_queries[4].audit.result == BOOT_DISPLAY_BAR_EMPTY_LIST &&
        boot.framebuffer_bar_queries[4].prefix_size == 2U &&
        boot.framebuffer_bar_queries[4].prefix[0] == 0x79U);
    CHECK(!boot.framebuffer_bars[1].flags && !boot.framebuffer_bars[3].flags);

    packed_fixture();
    /* Do not assume a dense scheme: exact contents also work in arbitrary order. */
    u8 swap[48];
    memcpy(swap, g_rx.resources[0], sizeof(swap));
    memcpy(g_rx.resources[0], g_rx.resources[3], sizeof(swap));
    memcpy(g_rx.resources[3], swap, sizeof(swap));
    CHECK(capture(&boot) && boot.framebuffer_bar_query_index[0] == 3U &&
        boot.framebuffer_bar_query_index[5] == 0U && boot.framebuffer_bars[5].size == 0x40000ULL);

    for (u32 conflict = 0U; conflict < 2U; ++conflict) {
        fixture();
        resource(g_rx.resources[4], 0x63400000ULL, conflict ? 0x20000ULL : 0x40000ULL, 32U);
        CHECK(capture(&boot) && boot.framebuffer_bar_audit[5].result == BOOT_DISPLAY_BAR_AMBIGUOUS &&
            !boot.framebuffer_bars[5].size && boot.framebuffer_bars[0].size == 0x10000000ULL);
        CHECK(boot.framebuffer_bar_queries[4].matching_bars == (1U << 5U) &&
            boot.framebuffer_bar_queries[5].matching_bars == (1U << 5U));
    }
    fixture();
    g_rx.config[6] = 0x63400000U; /* two physical slots with identical type/base */
    CHECK(capture(&boot) && boot.framebuffer_bar_queries[5].audit.result == BOOT_DISPLAY_BAR_AMBIGUOUS &&
        boot.framebuffer_bar_queries[5].matching_bars == ((1U << 2U) | (1U << 5U)) &&
        !boot.framebuffer_bars[2].flags && !boot.framebuffer_bars[5].flags);

    packed_fixture();
    g_rx.resources[2][3] = 0x41U; /* preserve user-transcribed anomaly; no bit masking */
    CHECK(capture(&boot) && boot.framebuffer_bar_queries[2].audit.result == BOOT_DISPLAY_BAR_TYPE_MISMATCH &&
        !boot.framebuffer_bar_queries[2].matching_bars && boot.framebuffer_bar_queries[2].prefix[3] == 0x41U &&
        !boot.framebuffer_bars[4].flags && boot.framebuffer_bars[2].size == 0x200000ULL &&
        boot.framebuffer_bars[5].size == 0x40000ULL);

    packed_fixture();
    put64(g_rx.resources[1], 6U, 32U);
    CHECK(capture(&boot) && !boot.framebuffer_bars[2].flags && boot.framebuffer_bars[5].size == 0x40000ULL);
    packed_fixture();
    put64(g_rx.resources[3], 30U, 0x1000ULL);
    CHECK(capture(&boot) && !boot.framebuffer_bars[5].flags &&
        boot.framebuffer_bar_queries[3].audit.result == BOOT_DISPLAY_BAR_TRANSLATED);

    fixture();
    g_rx.config[4] = 0xCU;
    g_rx.config[5] = 4U;
    resource(g_rx.resources[0], 0x400000000ULL, 0x10000000ULL, 64U);
    CHECK(capture(&boot) && boot.framebuffer_bars[0].base == 0x400000000ULL &&
        boot.framebuffer_bar_query_index[0] == 0U && !boot.framebuffer_bars[1].flags);

    packed_fixture();
    g_rx.changed = 1;
    CHECK(capture(&boot) && !boot.framebuffer_pci_valid);
    for (u32 i = 0; i < 6U; ++i) {
        CHECK(!boot.framebuffer_bar_queries[i].matching_bars &&
            boot.framebuffer_bar_queries[i].audit.result == BOOT_DISPLAY_BAR_CONFIG_CHANGED &&
            !boot.framebuffer_bars[i].flags);
    }
    return 1;
}

static int bounded_query_matcher(void) {
    u32 config[16] = {0};
    config[9] = 0x63400000U;
    BootDisplayBar extent;
    BootDisplayBarQuery query;
    boot_display_match_query((const u8 *)1, BOOT_DISPLAY_BAR_RESOURCE_BYTES + 1U,
        config, 5U, &query, &extent);
    CHECK(query.audit.result == BOOT_DISPLAY_BAR_MALFORMED && !extent.flags && !query.prefix_size);
    u8 bytes[48];
    resource(bytes, 0x63400000ULL, 0x40000ULL, 32U);
    for (u32 size = 0U; size <= sizeof(bytes); ++size) {
        boot_display_match_query(bytes, size, config, 3U, &query, &extent);
        CHECK((query.matching_bars != 0) == (size == sizeof(bytes)));
        CHECK(size == sizeof(bytes) || (!extent.flags && !extent.size));
    }
    puts("Bounded query association: 50 invalid/prefix-length cases PASS");
    return 1;
}

int main(void) {
    if (!selected_and_child() || !association_failures() || !optional_failures() ||
        !bar_audit_and_lists() || !bounded_decoder() ||
        !query_numbering_and_ambiguity() || !bounded_query_matcher()) return 1;
    printf("LOADER DISPLAY INVENTORY: PASS (%u mocked firmware cases; no hardware access)\n", g_cases);
    return 0;
}
