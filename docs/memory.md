# JCOS memory ownership and mappings

This reference describes the
[2026-11-05 source baseline](https://github.com/BigJMC9/JCOS/commit/fcfe857140891d7c25a44814e1738970d72a852a).
Primary implementation: `kernel/pmm.c`, `kernel/vmm.c`,
`kernel/address_space.c`, `kernel/kernel_stack.c`, `kernel/user_elf.c`, and the
paging setup in `kernel/kernel.c`.

## Ownership layers

| Layer | Owns | Does not imply |
| --- | --- | --- |
| PMM | Allocation and reclamation of usable 4 KiB physical frames. A `frame_t` is a frame number. | That every physical address is safe ordinary RAM or mapped for CPU access. |
| `VmPageMap` | Its root and page-table structures in its declared owned PML4 range. | Ownership of leaf data frames or permission to free shared subtrees. |
| `AddressSpace` | A process's user page-map ownership and identity; references can prevent destruction. | Ownership of every buffer temporarily mapped into it. |
| Image, stack, surface and DMA objects | Their allocation and mapping ledgers under their particular lifetime contract. | Safe release merely because the owning process has stopped. |

Unmapping a page does not free its data frame. A successful leaf unmap commits
the removal and returns that frame to the caller; optional empty-table pruning
can remain pending. The caller must retain its frame ownership until release
succeeds.

## Physical direct map

The ordinary RAM window starts at **`0xFFFF800000000000`**. Boot builds it over
UEFI conventional-memory ranges at or above 1 MiB, subject to bounds checks.
CPU access to mapped PMM RAM uses `PHYS_MAP_BASE + physical`; PMM metadata and
page-table access switch to this window after the kernel installs its own CR3.

The conversion helper only computes an address. It does not map arbitrary
physical memory and does not validate that an MMIO or firmware range is ordinary
RAM. The window's address arithmetic accepts physical addresses below 128 TiB;
that is not a claim that the machine has that much RAM or that the whole window
is populated.

Hardware DMA addresses remain device-facing addresses, not CPU virtual pointers.
For the current trusted AHCI path, a buffer at physical `0x00102000` is accessed
by the CPU at `0xFFFF800000102000`, while the controller receives `0x00102000`.
A future translated device domain may use a different device address; this
example does not establish an IOMMU mapping.

## Shared kernel mappings already exist

User address spaces own PML4 entries **1 through 255** (`[1, 256)`). They share
low slot 0 while the kernel remains low-linked, plus high slots 256 through 511.
The user mapping range is:

```text
start, inclusive: 0x0000008000000000
end, exclusive:   0x0000800000000000
```

Shared kernel mappings remain supervisor-only. A user page map never frees the
kernel subtrees it imports. Exported kernel subtrees are pinned for the boot
lifetime in the current profile. Address-space construction rejects live object
storage and retains unpublished cleanup ownership if rollback cannot complete.

The shared slot arrangement is a transitional low-linked-kernel design, not a
claim that every low virtual address is exposed to Ring3.

## Permissions and special mappings

Boot establishes hardware NX and supervisor write protection (`CR0.WP`). Kernel
text is executable and not writable; kernel data and stacks are writable and
non-executable. Boot validation also rejects an unintended direct-map alias of
the kernel image. User address-space mappings reject simultaneous write and
execute permissions.

The static ELF loader validates load ranges, alignment, overflow, entry point,
overlap and reserved-stack conflicts. It rejects unsupported dynamic/TLS
segments and writable-executable segments, initializes allocated image memory,
and keeps per-page ownership during load rollback and unload.

Explicit compatibility mappings remain for the low-linked kernel, boot stack,
BootInfo, firmware memory map, initrd, framebuffer, and selected ACPI/APIC/AHCI
resources. Ordinary allocatable PMM RAM does not retain a blanket low identity
alias. MMIO resources and the GOP framebuffer have separately validated mapping
rules; they must not be treated as interchangeable with direct-mapped RAM.

## Stacks and teardown

Ordinary thread kernel stacks execute through supervisor-only virtual slots:
one unmapped guard page, 16 KiB of usable stack, and another unmapped guard page.
The physmap alias remains available for controlled kernel maintenance; RSP/RSP0
use the guarded arena. The scheduler has a separate private idle-stack slot.

Teardown preserves a retryable ledger:

- A failed live kernel-stack release restores its mapping instead of discarding
  the Thread's identity or ownership.
- ELF cleanup records a successful unmap before attempting frame release. A
  failed free leaves the page record intact for retry.
- Page-map destruction requires an inactive, non-exported map whose owned trees
  contain no remaining leaf mappings. Failed table release retains the root and
  remaining tables.
- A bounded VMM quarantine slot retains an allocated table that could not be
  linked or freed during rollback. Pending cleanup is not silently counted as
  successful reclamation.

These are single-CPU, no-PCID reclamation rules. Multicore TLB shootdown and
other broader invalidation contracts are not established by local interrupt
masking or the current CR3 switch.

## DMA is a separate lifetime boundary

DMA-buffer CPU mappings, device submissions, assignment authority and device
quiescence have separate lifetimes. Revoking a capability, terminating a driver,
or removing a CPU mapping cannot by itself make a device-accessible frame
reusable. The constrained PCI framework retains or quarantines resources until
its supported quiescence/reset proof permits release. Generic physical bus
mastering is not enabled by that framework.

See [service and device boundaries](SERVICE_BOUNDARY.md) for the supported
mediation model. [Testing](TESTING.md) covers baseline restoration and fault
paths. Equal free-frame counts are useful evidence, but cannot alone prove
ownership safety, isolation or exhaustive failure coverage.
