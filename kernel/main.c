#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#include "drivers/uart.h"
#include "arch/aarch64/mmu.h"

#define KERNEL_STACK_SIZE (64 * 1024)

__attribute__((used, aligned(16)))
static uint8_t kernel_stack[KERNEL_STACK_SIZE];

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

static void hcf(void) {
    for (;;) {
        __asm__ volatile ("wfi");
    }
}

void kmain(void) {
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        hcf();
    }

    if (executable_address_request.response == NULL) {
        hcf();
    }
    mmu_init(executable_address_request.response->physical_base,
              executable_address_request.response->virtual_base);

    uart_init();

    uart_puts("NaumiOS boot OK\n");

    if (memmap_request.response != NULL) {
        uart_puts("Limine memmap entries received\n");
    }
    if (hhdm_request.response != NULL) {
        uart_puts("Limine HHDM offset received\n");
    }

    hcf();
}
