/* spifi_glue.c — 80K_4320_Bootloader (reconstructed)
 *
 * Thin bootloader wrappers over NXP lpcspifilib: a reference-counted init/deinit
 * around a single device handle, plus an IRQ-masking SPIFI lock. The lpcspifilib
 * functions are IMPORTED (see lpcspifilib/), not reimplemented; each call is
 * tagged with the disassembly label it matched. Wrapper names are kept identical
 * to the disassembly.
 */

#include "bootloader.h"
#include "spifilib_api.h"   /* SPIFI_HANDLE_T, spifiInit, spifiInitDevice, ... */
#include "chip.h"           /* LPC_SPIFI_BASE, __LDREXW/__STREXW, __*_irq, __ISB */

/* Shared device handle (IDA: g_spifi_bss_start). flash_if.c also reads it.
 * Could equally be declared in bootloader.h; kept here to keep that header free
 * of lpcspifilib types. */
void *g_spifi_handle = NULL;

static volatile int      s_init_refcount = 0;   /* IDA: dword_10017000[0] */
/* SPIFI lock refcount (IDA: dword_10017058[0]). In the image this cell shares a
 * base with lpcspifilib's family registry — a linker placement detail; logically
 * it is just this lock. */
static volatile uint32_t s_spifi_lock    = 0;
/* Backing store for the device handle/context (IDA: buffer @0x10017004). The
 * driver requires a context <=0x53 bytes; 0x54 leaves room. */
static uint8_t s_handle_mem[0x54] __attribute__((aligned(4)));

/* ----------------------------------------------------------------------- */
int spifi_init(void)
{
    SPIFI_HANDLE_T *h;
    int rc;

    if (s_init_refcount > 0) {                /* already up: just bump refcount */
        if (g_spifi_handle == NULL)
            return FL_NOTINIT;
        s_init_refcount++;
        return FL_OK;
    }
    if (s_init_refcount != 0)                 /* negative: never expected */
        return FL_NOTINIT;

    rc = spifiInit(LPC_SPIFI_BASE, true);                       /* IDA: spifi_reset_controller */
    if (rc != 0)
        return rc;

    spifiRegisterFamily(SPIFI_REG_FAMILY_CommonCommandSet);     /* IDA: spifi_register_family(spifi_register_all_families) */

    /* spifiGetHandleMemSize (IDA: spifi_get_device_size) returns the RAM context
     * size, NOT a flash size; require 1..0x53. */
    if ((uint32_t)(spifiGetHandleMemSize(LPC_SPIFI_BASE) - 1u) > 0x52u)
        return FL_NOTINIT;

    h = spifiInitDevice(s_handle_mem, sizeof s_handle_mem,
                        LPC_SPIFI_BASE, SPIFI_XIP_BASE);        /* IDA: spifi_init_device */
    if (h == NULL)
        return FL_NOTINIT;

    rc = spifiDevSetOpts(h, 0x0Cu, true);    /* IDA: spifi_set_options (opts=0xC = quad I/O bits) */
    if (rc != 0)
        return rc;

    rc = spifiDevSetMemMode(h, true);         /* IDA: spifi_set_memmode -> enter XIP */
    if (rc != 0)
        return rc;

    g_spifi_handle = h;
    s_init_refcount++;
    return FL_OK;
}

int spifi_deinit(void)
{
    int rc;
    if (s_init_refcount == 1) {
        if (g_spifi_handle != NULL) {
            /* last release: restore XIP. The disassembly routes this through the
             * device mode-hook helper (IDA: spifi_dev_to_mem_mode); via the
             * public API that is spifiDevSetMemMode(h, true). */
            rc = spifiDevSetMemMode((SPIFI_HANDLE_T *)g_spifi_handle, true);
            s_init_refcount--;
            return rc;
        }
        return FL_NOTINIT;                    /* refcount==1 but no handle */
    }
    if (s_init_refcount > 0) {
        s_init_refcount--;
        return FL_OK;
    }
    return FL_NOTINIT;
}

/* Atomic refcounted SPIFI lock. On the 0->1 edge it masks IRQs (so nothing
 * fetches from XIP while the controller is in command mode); on 1->0 it unmasks. */
void spifi_lock_acquire(void)
{
    uint32_t cur;
    do {
        cur = __LDREXW(&s_spifi_lock) + 1u;
    } while (__STREXW(cur, &s_spifi_lock) != 0u);
    if (cur == 1u)
        __disable_irq();
}

void spifi_lock_release(void)
{
    uint32_t cur;
    do {
        cur = __LDREXW(&s_spifi_lock) - 1u;
    } while (__STREXW(cur, &s_spifi_lock) != 0u);
    if (cur == 0u) {
        __enable_irq();
        __ISB();
    }
}