#include "lib/syscall.h"
#include "../include/pci_driver_probe_protocol.h"
#include "../include/pci_irq_protocol.h"
#include "../include/program_startup.h"

static void clear_message(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0U; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = 0ULL;
    message->word_count = 0U;
}

static int startup_valid(const JcosProgramStartup *startup) {
    return startup && startup->magic == JCOS_PROGRAM_STARTUP_MAGIC &&
        startup->version == JCOS_PROGRAM_STARTUP_VERSION &&
        startup->size == sizeof(JcosProgramStartup) &&
        startup->flags == JCOS_PROGRAM_STARTUP_FLAG_NONE &&
        startup->capability_count == 2U && startup->argument_count == 1U &&
        startup->environment_count == 0U &&
        startup->arguments[0] == JCOS_PCI_DRIVER_PROBE_VERSION &&
        startup->capabilities[0] != JCOS_CAPABILITY_INVALID_HANDLE &&
        startup->capabilities[1] != JCOS_CAPABILITY_INVALID_HANDLE;
}

static int send_words(JcosCapabilityHandle reply, JcosU64 a, JcosU64 b,
    JcosU64 c, JcosU64 d, JcosU32 count) {
    JcosIpcMessage message;
    clear_message(&message);
    message.word_count = count;
    message.words[0] = a;
    message.words[1] = b;
    message.words[2] = c;
    message.words[3] = d;
    return jcos_ipc_send_blocking(reply, &message);
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosCapabilityHandle command = startup->capabilities[0];
    JcosCapabilityHandle reply = startup->capabilities[1];
    JcosCapabilityHandle irq = JCOS_CAPABILITY_INVALID_HANDLE;
    JcosU64 dma_base = 0ULL;
    JcosU64 dma_size = 0ULL;

    if (!send_words(reply, JCOS_PCI_DRIVER_PROBE_READY,
            JCOS_PCI_DRIVER_PROBE_VERSION, 0ULL, 0ULL, 2U)) jcos_thread_exit();

    for (;;) {
        JcosIpcMessage request;
        clear_message(&request);
        if (!jcos_ipc_receive_blocking(command, &request) || !request.word_count)
            jcos_thread_exit();

        JcosU64 op = request.words[0];
        if (op == JCOS_PCI_DRIVER_PROBE_CONFIG && request.word_count == 4U) {
            dma_base = request.words[1];
            dma_size = request.words[2];
            irq = (JcosCapabilityHandle)request.words[3];
            if (!dma_base || !dma_size || irq == JCOS_CAPABILITY_INVALID_HANDLE)
                jcos_thread_exit();
            if (!send_words(reply, JCOS_PCI_DRIVER_PROBE_CONFIGURED,
                    dma_size, 0ULL, 0ULL, 2U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_PCI_DRIVER_PROBE_REQUEST_INVALID_DMA && request.word_count == 1U) {
            if (!dma_base || !dma_size ||
                !send_words(reply, JCOS_PCI_DRIVER_PROBE_DMA_REQUEST,
                    dma_size, 1ULL, 0xEEULL, 4U)) jcos_thread_exit();

            JcosIpcMessage result;
            clear_message(&result);
            if (!jcos_ipc_receive_blocking(command, &result) || result.word_count != 2U ||
                result.words[0] != JCOS_PCI_DRIVER_PROBE_DMA_RESULT ||
                result.words[1] != JCOS_PCI_DRIVER_PROBE_DMA_REJECTED ||
                !send_words(reply, JCOS_PCI_DRIVER_PROBE_INVALID_REJECTED,
                    0ULL, 0ULL, 0ULL, 1U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_PCI_DRIVER_PROBE_REQUEST_DMA && request.word_count == 4U) {
            JcosU64 offset = request.words[1];
            JcosU64 size = request.words[2];
            JcosU64 value = request.words[3] & 0xFFULL;
            if (!dma_base || !dma_size || offset >= dma_size || !size || size > dma_size - offset ||
                !send_words(reply, JCOS_PCI_DRIVER_PROBE_DMA_REQUEST,
                    offset, size, value, 4U)) jcos_thread_exit();

            JcosIpcMessage result;
            clear_message(&result);
            if (!jcos_ipc_receive_blocking(command, &result) || result.word_count != 2U ||
                result.words[0] != JCOS_PCI_DRIVER_PROBE_DMA_RESULT ||
                result.words[1] != JCOS_PCI_DRIVER_PROBE_DMA_COMPLETE) jcos_thread_exit();

            volatile unsigned char *bytes =
                (volatile unsigned char *)(JcosU64)(dma_base + offset);
            for (JcosU64 i = 0ULL; i < size; ++i)
                if (bytes[i] != (unsigned char)value) jcos_thread_exit();
            if (!send_words(reply, JCOS_PCI_DRIVER_PROBE_DMA_VERIFIED,
                    offset, size, value, 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_PCI_DRIVER_PROBE_REQUEST_DMA_THEN_FAULT && request.word_count == 4U) {
            JcosU64 offset = request.words[1];
            JcosU64 size = request.words[2];
            JcosU64 value = request.words[3] & 0xFFULL;
            if (!dma_base || !dma_size || offset >= dma_size || !size || size > dma_size - offset ||
                !send_words(reply, JCOS_PCI_DRIVER_PROBE_DMA_REQUEST,
                    offset, size, value, 4U)) jcos_thread_exit();

            JcosIpcMessage action;
            clear_message(&action);
            if (!jcos_ipc_receive_blocking(command, &action) || action.word_count != 1U ||
                action.words[0] != JCOS_PCI_DRIVER_PROBE_DIAG_FAULT) jcos_thread_exit();
            __asm__ volatile ("ud2");
            jcos_thread_exit();
        }

        if (op == JCOS_PCI_DRIVER_PROBE_REQUEST_DMA_THEN_HANG && request.word_count == 4U) {
            JcosU64 offset = request.words[1];
            JcosU64 size = request.words[2];
            JcosU64 value = request.words[3] & 0xFFULL;
            if (!dma_base || !dma_size || offset >= dma_size || !size || size > dma_size - offset ||
                !send_words(reply, JCOS_PCI_DRIVER_PROBE_DMA_REQUEST,
                    offset, size, value, 4U)) jcos_thread_exit();

            JcosIpcMessage action;
            clear_message(&action);
            if (!jcos_ipc_receive_blocking(command, &action) || action.word_count != 1U ||
                action.words[0] != JCOS_PCI_DRIVER_PROBE_WAIT_IRQ_AND_HANG ||
                irq == JCOS_CAPABILITY_INVALID_HANDLE) jcos_thread_exit();

            JcosIpcMessage event;
            clear_message(&event);
            if (!jcos_ipc_receive_blocking(irq, &event) ||
                event.word_count != JCOS_PCI_IRQ_EVENT_WORDS ||
                event.words[JCOS_PCI_IRQ_EVENT_MAGIC_WORD] != JCOS_PCI_IRQ_EVENT_MAGIC ||
                !event.words[JCOS_PCI_IRQ_EVENT_SEQUENCE_WORD] ||
                !send_words(reply, JCOS_PCI_DRIVER_PROBE_HANGING,
                    event.words[JCOS_PCI_IRQ_EVENT_SEQUENCE_WORD],
                    event.words[JCOS_PCI_IRQ_EVENT_SUPPRESSED_WORD],
                    event.words[JCOS_PCI_IRQ_EVENT_RESOURCE_WORD], 4U)) jcos_thread_exit();

            for (;;) __asm__ volatile ("pause");
        }

        if (op == JCOS_PCI_DRIVER_PROBE_WAIT_IRQ && request.word_count == 1U) {
            if (irq == JCOS_CAPABILITY_INVALID_HANDLE) jcos_thread_exit();
            JcosIpcMessage event;
            clear_message(&event);
            if (!jcos_ipc_receive_blocking(irq, &event) ||
                event.word_count != JCOS_PCI_IRQ_EVENT_WORDS ||
                event.words[JCOS_PCI_IRQ_EVENT_MAGIC_WORD] != JCOS_PCI_IRQ_EVENT_MAGIC ||
                !event.words[JCOS_PCI_IRQ_EVENT_SEQUENCE_WORD]) jcos_thread_exit();
            if (!send_words(reply, JCOS_PCI_DRIVER_PROBE_IRQ_ACK,
                    event.words[JCOS_PCI_IRQ_EVENT_SEQUENCE_WORD],
                    event.words[JCOS_PCI_IRQ_EVENT_SUPPRESSED_WORD],
                    event.words[JCOS_PCI_IRQ_EVENT_RESOURCE_WORD], 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_PCI_DRIVER_PROBE_DIAG_FAULT && request.word_count == 1U) {
            __asm__ volatile ("ud2");
            jcos_thread_exit();
        }

        if (op == JCOS_PCI_DRIVER_PROBE_EXIT && request.word_count == 1U)
            jcos_thread_exit();

        jcos_thread_exit();
    }
}
