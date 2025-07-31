#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>

#include "mmio.h"
#include "ethernet.h"

//#define CONFIG_DISAGG_DEBUG_MMIO

static ssize_t mmio_recv(void *buf)
{
    uint8_t *recv_buf;
    ssize_t size; 

    recv_buf = eth_recv_first(true);
    if (!recv_buf) {
	fprintf(stderr, "mmio_recv: eth_recv_first failed\n");
	return -1;
    }

    while (recv_buf[0] != OP_MMIO_READ && recv_buf[0] != OP_MMIO_WRITE) {

	recv_buf = eth_recv_next(true, recv_buf);
	if (!recv_buf) {
	    fprintf(stderr, "mmio_recv: eth_recv_next failed\n");
	    return -1;
	}

    }

    if (recv_buf[0] == OP_MMIO_READ) {
	size = sizeof(struct mmio_message) - sizeof(uint64_t);
	memcpy(buf, recv_buf + 1, size);
    } else {
	size = sizeof(struct mmio_message);
	memcpy(buf, recv_buf + 1, size);
    }

    eth_recv_done(recv_buf);

    return size;
}

static int mmio_send_read_reply(void *buf)
{
    uint8_t *send_buf;
    int ret;

    send_buf = eth_get_send_buf(1 + sizeof(uint64_t));
    if (!send_buf) {
	fprintf(stderr, "mmio_send_read_reply: eth_get_send_buf failed\n");
	return 1;
    }

    memcpy(send_buf + 1, buf, sizeof(uint64_t));

    send_buf[0] = OP_MMIO_READ;

    ret = eth_send_buf(send_buf);
    if (ret != 0) {
	fprintf(stderr, "mmio_send_read_reply: eth_send_buf failed\n");
	return 1;
    }

    return 0;
}

void *run_mmio_app(disagg_pci_dev_info *pci_info, void *opaque)
{
    printf("MMIO communication application started. Waiting for messages...\n");

    char data[sizeof(uint64_t)];
    bool is_write = false;
    region_access_cb_t *cb;
    loff_t offset;
    int pci_region;
    uint32_t ret;

    while (1) {
        struct mmio_message header;
        if (mmio_recv(&header) < 0) {
            perror("Failed to read message");
            continue;
        }

#ifdef CONFIG_DISAGG_DEBUG_MMIO
        printf("Received message: operation=%u, address=0x%lx, length=%u\n",
               header.operation, header.address, header.length);
#endif

        switch (header.operation) {

	    case OP_MMIO_READ:

#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("connection.c: OP_READ: Received read operation: Address 0x%lx, Length %u\n", header.address, header.length);
#endif
		pci_region = 0;

#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("connection.c: OP_READ: Got PCI region: %d ", pci_region);
#endif
		cb = pci_info->regions[pci_region].cb;

		offset = header.address;
#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("with offset %ld\n", offset);
#endif
		is_write = false;
		ret = cb(opaque, data, header.length, offset, is_write);

		if (ret != header.length) {
		    printf("connection.c: OP_READ: Reading %lu bytes failed\n", header.length);
		    memset(data, 'A', header.length);
		}

#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("connection.c: OP_READ: Read data: ");
		for (uint32_t i = 0; i < header.length; i++) {
		    printf("%02X", ((uint8_t *)data)[i]);
		}
		printf("\n");
#endif

		if (mmio_send_read_reply(data) != 0) {
		    perror("Failed to write response");
		    continue;
		}

		continue;

	    case OP_MMIO_WRITE:
#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("connection.c: OP_WRITE: Received write operation: Address 0x%lx, Length %u\n", header.address, header.length);
#endif
		pci_region = 0;

		cb = pci_info->regions[pci_region].cb;

#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("connection.c: OP_WRITE: Write data: ");
		for (uint32_t i = 0; i < header.length; i++) {
		    printf("%02X", ((uint8_t *)data)[i]);
		}
		printf("\n");
#endif

		offset = header.address;
		is_write = true;
		ret = cb(opaque, (char *)&header.value, header.length, offset, is_write);

		if (ret != header.length) {
		    printf("connection.c: OP_WRITE: Writing %lu bytes failed\n", header.length);
		}

		continue;

	    default:
		fprintf(stderr, "Unknown operation: %d\n", header.operation);
		continue;
        }
    }
}

