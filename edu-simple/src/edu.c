/*
 * QEMU educational PCI device
 *
 * Copyright (c) 2012-2015 Jiri Slaby
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <pthread.h> // for thread, mutex and condition
#include <stdlib.h>
#include <time.h> // for timer
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "edu.h"
#include "sec_disagg.h"

/* Qemu normally provides those functions */
void pci_dma_read(dma_addr_t addr, void *buf, size_t len) {
    if (disagg_dma_decrypt((void *)addr, buf, len) != len)
	printf("pci_dma_read failed\n");
}

void pci_dma_write(dma_addr_t addr, void *buf, size_t len) {
    if (disagg_dma_encrypt(buf, (void *)addr, len) != 0)
	printf("pci_dma_write failed\n");
}
/* End QEMU API */

static void mutex_lock(pthread_mutex_t *mutex) {
    if (pthread_mutex_lock(mutex) != 0) {
	printf("Error: mutex lock\n");
	exit(EXIT_FAILURE);
    }
}

static void mutex_unlock(pthread_mutex_t *mutex) {
    if (pthread_mutex_unlock(mutex) != 0) {
	printf("Error: mutex unlock\n");
	exit(EXIT_FAILURE);
    }
}

static void cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (pthread_cond_wait(cond, mutex) != 0) {
	printf("Error: cond wait\n");
	exit(EXIT_FAILURE);
    }
}

static void cond_signal(pthread_cond_t *cond) {
    if (pthread_cond_signal(cond) != 0) {
	printf("Error: cond wait\n");
	exit(EXIT_FAILURE);
    }
}

/*
// Copied from https://stackoverflow.com/a/40949950
// We don't need a very accurate timer
static void start_timer(uint64_t ms) {
    int ms_since = clock() * 1000 / CLOCKS_PER_SEC;
    int end = ms_since + ms;

    do {
	ms_since = clock() * 1000 / CLOCKS_PER_SEC;
    } while (ms_since <= end);
}
*/

static bool edu_msi_enabled(EduState *edu)
{
    (void) edu;
    // No interrupts
    return false;
}

static void edu_raise_irq(EduState *edu, uint32_t val)
{
    // No interrupts
    edu->irq_status |= val;
    if (edu->irq_status) {
        if (edu_msi_enabled(edu)) {
            //msi_notify(&edu->pdev, 0);
        } else {
            //pci_set_irq(&edu->pdev, 1);
        }
    }
}

static void edu_lower_irq(EduState *edu, uint32_t val)
{
    edu->irq_status &= ~val;

    if (!edu->irq_status && !edu_msi_enabled(edu)) {
        //pci_set_irq(&edu->pdev, 0);
    }
}

static void edu_check_range(uint64_t xfer_start, uint64_t xfer_size,
                uint64_t dma_start, uint64_t dma_size)
{
    uint64_t xfer_end = xfer_start + xfer_size;
    uint64_t dma_end = dma_start + dma_size;

    /*
     * 1. ensure we aren't overflowing
     * 2. ensure that xfer is within dma address range
     */
    if (dma_end >= dma_start && xfer_end >= xfer_start &&
        xfer_start >= dma_start && xfer_end <= dma_end) {
        return;
    }
}

static void edu_dma_timer(void *opaque)
{
    EduState *edu = opaque;
    bool raise_irq = false;

    if (!(edu->dma.cmd & EDU_DMA_RUN)) {
        return;
    }

    if (EDU_DMA_DIR(edu->dma.cmd) == EDU_DMA_FROM_PCI) {
        uint64_t dst = edu->dma.dst;
        edu_check_range(dst, edu->dma.cnt, DMA_START, DMA_SIZE);
        dst -= DMA_START;
	pci_dma_read(edu->dma.src, edu->dma_buf + dst, edu->dma.cnt);
    } else {
        uint64_t src = edu->dma.src;
        edu_check_range(src, edu->dma.cnt, DMA_START, DMA_SIZE);
        src -= DMA_START;
	pci_dma_write(edu->dma.dst, edu->dma_buf + src, edu->dma.cnt);
    }

    edu->dma.cmd &= ~EDU_DMA_RUN;
    if (edu->dma.cmd & EDU_DMA_IRQ) {
        raise_irq = true;
    }

    if (raise_irq) {
        edu_raise_irq(edu, DMA_IRQ);
    }
}

static void dma_rw(EduState *edu, bool write, dma_addr_t *val, dma_addr_t *dma,
                bool timer)
{
    if (write && (edu->dma.cmd & EDU_DMA_RUN)) {
        return;
    }

    if (write) {
        *dma = *val;
    } else {
        *val = *dma;
    }

    if (timer) {
	//start_timer(100);
	edu_dma_timer(edu);
    }
}

static uint64_t edu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    EduState *edu = opaque;
    uint64_t val = ~0ULL;

    if (addr < 0x80 && size != 4) {
        return val;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return val;
    }

    switch (addr) {
    case 0x00:
        val = 0x010000edu;
        break;
    case 0x04:
        val = edu->addr4;
        break;
    case 0x08:
        mutex_lock(&edu->thr_mutex);
        val = edu->fact;
        mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        val =  __atomic_load_n(&edu->status, __ATOMIC_RELAXED);
        break;
    case 0x24:
        val = edu->irq_status;
        break;
    case 0x80:
        dma_rw(edu, false, &val, &edu->dma.src, false);
        break;
    case 0x88:
        dma_rw(edu, false, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        dma_rw(edu, false, &val, &edu->dma.cnt, false);
        break;
    case 0x98:
        dma_rw(edu, false, &val, &edu->dma.cmd, false);
        break;
    }

    return val;
}

static void edu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
    EduState *edu = opaque;
    if (addr < 0x80 && size != 4) {
        return;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return;
    }

    switch (addr) {
    case 0x04:
        edu->addr4 = ~val;
        break;
    case 0x08:
        if (__atomic_load_n(&edu->status, __ATOMIC_RELAXED) & EDU_STATUS_COMPUTING) {
            break;
        }
        /* EDU_STATUS_COMPUTING cannot go 0->1 concurrently, because it is only
         * set in this function and it is under the iothread mutex.
         */
        mutex_lock(&edu->thr_mutex);
        edu->fact = val;
	__atomic_fetch_or(&edu->status, EDU_STATUS_COMPUTING, __ATOMIC_SEQ_CST);
        cond_signal(&edu->thr_cond);
        mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        if (val & EDU_STATUS_IRQFACT) {
	    __atomic_fetch_or(&edu->status, EDU_STATUS_IRQFACT, __ATOMIC_SEQ_CST);
            /* Order check of the COMPUTING flag after setting IRQFACT.  */
	    __atomic_signal_fence(__ATOMIC_SEQ_CST);
        } else {
	    __atomic_fetch_and(&edu->status, ~EDU_STATUS_IRQFACT, __ATOMIC_SEQ_CST);
        }
        break;
    case 0x60:
        edu_raise_irq(edu, val);
        break;
    case 0x64:
        edu_lower_irq(edu, val);
        break;
    case 0x80:
        dma_rw(edu, true, &val, &edu->dma.src, false);
        break;
    case 0x88:
        dma_rw(edu, true, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        dma_rw(edu, true, &val, &edu->dma.cnt, false);
        break;
    case 0x98:
        if (!(val & EDU_DMA_RUN)) {
            break;
        }
        dma_rw(edu, true, &val, &edu->dma.cmd, true);
        break;
    }
}

/*
static const MemoryRegionOps edu_mmio_ops = {
    .read = edu_mmio_read,
    .write = edu_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },

};
*/

/*
 * We purposely use a thread, so that users are forced to wait for the status
 * register.
 */
static void *edu_fact_thread(void *opaque)
{
    EduState *edu = opaque;

    while (1) {
        uint32_t val, ret = 1;

	mutex_lock(&edu->thr_mutex);
	while (((__atomic_load_n(&edu->status, __ATOMIC_ACQUIRE) & EDU_STATUS_COMPUTING) == 0) &&
                        !edu->stopping) {
	    cond_wait(&edu->thr_cond, &edu->thr_mutex);
        }

        if (edu->stopping) {
	    mutex_unlock(&edu->thr_mutex);
            break;
        }

        val = edu->fact;
	mutex_unlock(&edu->thr_mutex);

        while (val > 0) {
            ret *= val--;
        }

        /*
         * We should sleep for a random period here, so that students are
         * forced to check the status properly.
         */

        mutex_lock(&edu->thr_mutex);
        edu->fact = ret;
        mutex_unlock(&edu->thr_mutex);
	__atomic_fetch_and(&edu->status, ~EDU_STATUS_COMPUTING, __ATOMIC_SEQ_CST);
    }

    return NULL;
}

size_t edu_mmio_rw_bar0(void *opaque, char *buf, size_t count, size_t offset, bool is_write) {
    if (!is_write) {
	uint64_t res = edu_mmio_read(opaque, offset, count);
	if (count == 4) {
	    uint32_t res32 = (uint32_t) res;
	    memcpy(buf, &res32, count);
	} else {
	    // count has to be 8
	    memcpy(buf, &res, count);
	}
    } else {
	if (count == 4) {
	    edu_mmio_write(opaque, offset, *((uint32_t *) buf), count);
	} else {
	    // count has to be 8
	    edu_mmio_write(opaque, offset, *((uint64_t *) buf), count);
	}
    }

    return count;
}

EduState *init_edu_device(void)
{
    EduState *edu = malloc(sizeof(EduState));
    if (!edu) {
	printf("Error: malloc failed\n");
	return NULL;
    }
    memset(edu, 0, sizeof(*edu));

    pthread_mutex_init(&edu->thr_mutex, NULL);

    pthread_cond_init(&edu->thr_cond, NULL);

    if (pthread_create(&edu->thread, NULL, edu_fact_thread, edu) != 0) {
	printf("Error pthread_create\n");
	return NULL;
    }

    // Set memory regions to zero
    memset(edu->regions, 0, sizeof(edu->regions));
    // Alloc mmio region
    edu->regions[0].size = 1048576;
    edu->regions[0].addr = (uint64_t) malloc(edu->regions[0].size);
    edu->regions[0].cb = edu_mmio_rw_bar0;
    if (!edu->regions[0].addr) {
	printf("Error: malloc of mmio region failed\n");
	return NULL;
    }

    return edu;
}
