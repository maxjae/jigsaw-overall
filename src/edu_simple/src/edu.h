#ifndef EDU_H
#define EDU_H

#include "connection.h"

typedef uint64_t dma_addr_t;
typedef uint64_t hwaddr;

#define FACT_IRQ        0x00000001
#define DMA_IRQ         0x00000100

#define DMA_START       0x40000
#define DMA_SIZE       	1048576 

typedef struct EduState {
    struct IORegion {
	uint64_t addr;
	uint64_t size;
	region_access_cb_t *cb;
    } regions[PCI_NUM_REGIONS];

    pthread_t thread;
    pthread_mutex_t thr_mutex;
    pthread_cond_t thr_cond;
    bool stopping;

    uint32_t addr4;
    uint32_t fact;
#define EDU_STATUS_COMPUTING    0x01
#define EDU_STATUS_IRQFACT      0x80
    uint32_t status;

    uint32_t irq_status;

#define EDU_DMA_RUN             0x1
#define EDU_DMA_DIR(cmd)        (((cmd) & 0x2) >> 1)
# define EDU_DMA_FROM_PCI       0
# define EDU_DMA_TO_PCI         1
#define EDU_DMA_IRQ             0x4
    struct dma_state {
        dma_addr_t src;
        dma_addr_t dst;
        dma_addr_t cnt;
        dma_addr_t cmd;
    } dma;
    char dma_buf[DMA_SIZE];
} EduState;

EduState *init_edu_device(void);

#endif // EDU_H
