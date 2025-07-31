#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dma.h"

#include "../../include/common.h"
#include "ethernet.h"

static int send_dma_to_device_request(dma_addr_t addr, size_t count)
{
    ssize_t ret;
    size_t size_to_send;
    uint8_t *send_buf;

    size_to_send = 1 + (sizeof(uint64_t) * 2);

    send_buf = eth_get_send_buf(size_to_send);
    if (!send_buf) {
	fprintf(stderr, "send_dma_to_device_request: eth_get_send_buf failed\n");
	return 1;
    }

    /*** Send request for DMA region to proxy ***/
    send_buf[0] = OP_DMA_TO_DEVICE;
    *((uint64_t *)(send_buf + 1)) = addr;
    *((uint64_t *)(send_buf + sizeof(uint64_t) + 1)) = count;

    ret = eth_send_buf(send_buf);	

    if (ret != 0) {
	printf("send of DMA_TO_DEVICE request failed\n");
	return 1;
    }

    return 0;
}

static void *wait_for_dma_reply(void)
{
    uint8_t *recv_buf;

    recv_buf = eth_recv_first(true);
    if (!recv_buf) {
	fprintf(stderr, "wait_for_dma_reply: eth_recv_first failed\n");
	return NULL;
    }

    while (recv_buf[0] != OP_DMA_TO_DEVICE) {

	recv_buf = eth_recv_next(true, recv_buf);
	if (!recv_buf) {
	    fprintf(stderr, "wait_for_dma_reply: eth_recv_next failed\n");
	    return NULL;
	}

    }

    // Got the right response
    return recv_buf + 1;
}

void pci_dma_read(dma_addr_t addr, void *buf, size_t len)
{
    int ret;
    void *resp;

    ret = send_dma_to_device_request(addr, len);
    if (ret != 0)
	exit(EXIT_FAILURE);

    resp = wait_for_dma_reply();
    if (!resp)
	exit(EXIT_FAILURE);

    memcpy(buf, resp, len);

    eth_recv_done(resp);
}

static void *prepare_meta(dma_addr_t addr, size_t count)
{
    size_t size_to_send = 1 + (sizeof(uint64_t) * 2) + count;
    uint8_t *send_buf;

    send_buf = eth_get_send_buf(size_to_send);
    if (!send_buf) {
	fprintf(stderr, "prepare_meta: eth_get_send_buf failed\n");
	return NULL;
    }

    send_buf[0] = OP_DMA_FROM_DEVICE;
    *((uint64_t *)(send_buf + 1)) = addr;
    *((uint64_t *)(send_buf + 1 + sizeof(uint64_t))) = count;

    return send_buf;
}

void pci_dma_write(dma_addr_t addr, void *buf, size_t len)
{
    uint8_t *send_buf;
    int ret;

    send_buf = prepare_meta(addr, len);

    memcpy(send_buf + 1 + (sizeof(uint64_t) * 2), buf, len);

    ret = eth_send_buf(send_buf);
    if (ret != 0) {
	fprintf(stderr, "pci_dma_write: send of encrypted data failed\n");
	exit(EXIT_FAILURE);
    }
}

