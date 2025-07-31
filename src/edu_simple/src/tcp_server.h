#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <sys/types.h>

#include "../../include/common.h"

#define MMIO_MSG_MAX_SIZE (sizeof(struct mmio_message) + 1)

struct disagg_regions_tcp {
    /*
     * This buffer contains already received mmio requests.
     * Before calling recv on socket it first checks if there already is data available in here.
     */
    char recv_buf[MMIO_MSG_MAX_SIZE][64]; // Just use a big size
    ssize_t recv_buf_last_valid; // The last valid index of MMIO message (-1 if empty)
    ssize_t recv_buf_next; // The next index to be read


    /*
     * Buffer used in sends.
     * First byte is TYPE_REPLY.
     */
    char send_buf[sizeof(struct mmio_message)];


    /*
     * Destination or source of DMA reads or writes.
     * Always contains an encrypted version of DMA memory.
     * First bytes are meta-data for proxy in request case.
     */
    char dma_mem[DMA_SIZE + (sizeof(uint64_t) * 2) + 1];
    char *dma_buf; // Pointer used by device to access only its relevant data (is dma_mem + 1)
};

extern struct disagg_regions_tcp *regions_tcp;

int init_tcp(int argc, char **argv);

/*
 * @return a received mmio request
 * First checks if there is already one available in struct tcp_mmio,
 * otherwise recv one from socket
 * @return NULL for failure
 */
void *tcp_recv_mmio_request(void);

/*
 * @return non-zero for failure
 */
int tcp_send(void *buf, size_t count);

/*
 * Fills regions_tcp->dma_buf with data at @addr of shmem
 */
void tcp_read_dma(uint64_t addr, size_t count);

/*
 * Writes regions_tcp->dma_buf to @addr of shmem
 */
void tcp_write_dma(uint64_t addr, size_t count);

#endif // TCP_SERVER_H
