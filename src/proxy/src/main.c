#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>

#include <sys/mman.h>
#include <sys/socket.h>

#include "shmem.h"
#include "ethernet.h"
#include "../../include/common.h"

static int send_buf(void *buf, size_t size)
{
	void *send_buf;
	int ret;

	send_buf = eth_get_send_buf(size);
	if (!send_buf) {
		fprintf(stderr, "send_buf: eth_get_send_buf failed\n");
		return 1;
	}

	memcpy(send_buf, buf, size);

	ret = eth_send_buf(send_buf);
	if (ret != 0) {
		fprintf(stderr, "send_buf: eth_send_buf failed\n");
		return 1;
	}

	return 0;
}

static int send_buf_with_optype(uint8_t op_type, void *buf, size_t size)
{
	void *send_buf;
	int ret;

	send_buf = eth_get_send_buf(size + 1);
	if (!send_buf) {
		fprintf(stderr, "send_buf: eth_get_send_buf failed\n");
		return 1;
	}

	memcpy(send_buf, &op_type, 1);
	memcpy((uint8_t *)send_buf + 1, buf, size);

	ret = eth_send_buf(send_buf);
	if (ret != 0) {
		fprintf(stderr, "send_buf: eth_send_buf failed\n");
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int ret;
	ssize_t ret_size;
	char *shmem = NULL;
	uint64_t dma_addr, dma_size;
	uint8_t *recv_buf;


	if (init_shared_memory(&shmem) < 0) {
		printf("init_shared_memory failed\n");
		goto err;
	}

	printf("ethernet client: start\n");
	ret = init_ethernet(argc, argv);
	if (ret != 0) {
		goto err_unmap;
	} 

	/*** Check in-turn if either mmio message has arrived in shmem or server sent message ***/

	while (1) {
		// Check if the vm has written something to the mmio region
		ret = get_write_doorbell();
		if (ret == 1) {
			// Message ready to send
			ret = send_buf(shmem + MMIO_REGION_OFFSET, 1 + sizeof(struct mmio_message));

			if (ret != 0) {
				perror("send for mmio failed\n");
				goto err_unmap;
			}

			reset_write_doorbell();
		}


		// Read first byte, if available, to determine type of message
		recv_buf = eth_recv_first(false);
		if (!recv_buf) {
			continue;
		}

		switch (recv_buf[0]) {

			case OP_MMIO_READ:
				/*** Client sent reply to MMIO read ***/

				ret_size = ivshmem_write(recv_buf + 1, sizeof(uint64_t), 1);
				if (ret_size == -1) {
					printf("shmem write of mmio reply failed\n");
					goto err_unmap;
				}

				break;

			case OP_DMA_TO_DEVICE:
				/*** Client requests DMA region ***/

				dma_addr = *((uint64_t *)(recv_buf + 1));
				dma_size = *(((uint64_t *)(recv_buf + 1)) + 1);

				// Validate requested region
				if ((char *)dma_addr < shmem + DMA_REGION_OFFSET 
						|| (char *)dma_addr + dma_size >= shmem + SHMEM_SIZE) {
					fprintf(stderr,
						"Requested DMA memory region does not lie within valid shmem DMA region:"
						"addr: 0x%lx, size: %ld\n", dma_addr, dma_size);
					goto err_unmap;
				}	

				if (send_buf_with_optype(OP_DMA_TO_DEVICE, (void *)dma_addr, dma_size) != 0) {
					fprintf(stderr, "OP_DMA_TO_DEVICE: send of response failed\n");
					goto err_unmap;
				}

				break;

			case OP_DMA_FROM_DEVICE:
				/*** Client sent DMA region ***/

				dma_addr = *((uint64_t *)(recv_buf + 1));
				dma_size = *(((uint64_t *)(recv_buf + 1)) + 1);

				// Validate sent region
				if ((char *)dma_addr < shmem + DMA_REGION_OFFSET 
						|| (char *)dma_addr + dma_size >= shmem + SHMEM_SIZE) {
					fprintf(stderr, "Sent DMA memory region does not lie within valid shmem DMA region:"
							"addr: 0x%lx, size: %ld\n", dma_addr, dma_size);
					goto err_unmap;
				}	

				memcpy((void *)dma_addr, recv_buf + 1 + (sizeof(uint64_t) * 2), dma_size);

				break;

			default:
				printf("Error: Unknown type\n");
				goto err_unmap;
		}

		eth_recv_done(recv_buf);

	}


	// Will never reach that
	printf("tcp_client: end %d\n", ret);
	exit(EXIT_SUCCESS);

err_unmap:
	munmap(shmem, SHMEM_SIZE);
err:
	exit(EXIT_FAILURE);
}

