#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>

#include <sys/mman.h>
#include <sys/socket.h>

#include "shmem.h"
#include "tcp_client.h"
#include "../../include/common.h"

static char recv_buf[128];

int main(int argc, char **argv)
{
	int ret;
	ssize_t ret_size;
	char *shmem = NULL;
	char type = 0;
	uint64_t dma_addr, dma_size;


	if (init_shared_memory(&shmem) < 0) {
	    printf("init_shared_memory failed\n");
	    goto err;
	}

	printf("tcp_client: start\n");
	ret = init_tcp(argc, argv);
	if (ret != 0) {
	    goto err_unmap;
	} 

	/*** Check in-turn if either mmio message has arrived in shmem or server sent message ***/
	
	while (1) {
	    // Check if the vm has written something to the mmio region
	    ret = get_write_doorbell();
	    if (ret == 1) {
		// Message ready to send
		ret = tcp_send_buf(shmem + MMIO_REGION_OFFSET, 1 + sizeof(struct mmio_message));

		if (ret != 0) {
		    perror("send for mmio failed\n");
		    goto err_unmap;
		}

		reset_write_doorbell();
	    }


	    // Read first byte, if available, to determine type of message
	    errno = 0;
	    ret = tcp_recv_data(&type, 1, MSG_DONTWAIT);
	    if (ret == -1) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
		    // No data available
		    continue;
		} else {
		    perror("recv failed");
		    goto err_unmap;
		}
	    }

	    switch (type) {
		case OP_MMIO_READ:
		    /*** Client sent reply to MMIO read ***/

		    // Receive rest of message 
		    ret = tcp_recv_data(recv_buf, sizeof(uint64_t), 0);
		    if (ret != 0) {
			perror("Recv of mmio request reply failed");
			goto err_unmap;
		    }

		    // Received message. Write to shmem
		    ret_size = ivshmem_write(recv_buf, sizeof(uint64_t), 1);
		    if (ret_size == -1) {
			printf("shmem write of mmio reply failed\n");
			goto err_unmap;
		    }

		    break;

		case OP_DMA_TO_DEVICE:
		    /*** Client requests DMA region ***/

		    // Receive rest of message (i.e. address and size)
		    ret = tcp_recv_data(&recv_buf, sizeof(uint64_t) * 2, 0);
		    if (ret != 0) {
			printf("Recv of DMA request addres and size failed\n");
			goto err_unmap;
		    }

		    dma_addr = *((uint64_t *)recv_buf);
		    dma_size = *(((uint64_t *)recv_buf) + 1);

		    // Validate requested region
		    if ((char *)dma_addr < shmem + DMA_REGION_OFFSET 
				|| (char *)dma_addr + dma_size >= shmem + SHMEM_SIZE) {
			fprintf(stderr, "Requested DMA memory region does not lie within valid shmem DMA region:"
					 "addr: 0x%lx, size: %ld\n", dma_addr, dma_size);
			goto err_unmap;
		    }	

		    // Send type first in different message as otherwise would require big copy of DMA region
		    // (Works as this is a single-thread process)
		    type = OP_DMA_TO_DEVICE;
		    if (tcp_send_buf(&type, 1) != 0) {
			perror("send of OP_DMA_TO_DEVICE failed");
			goto err_unmap;
		    }

		    if (tcp_send_buf((void *) dma_addr, dma_size) != 0) {
			perror("send of DMA region failed");
			goto err_unmap;
		    }

		    break;

		case OP_DMA_FROM_DEVICE:
		    /*** Client send DMA region ***/

		    // Receive rest of message (i.e. address and size)
		    ret = tcp_recv_data(&recv_buf, sizeof(uint64_t) * 2, 0);
		    if (ret != 0) {
			perror("Recv of DMA request addres and size failed");
			goto err_unmap;
		    }

		    dma_addr = *((uint64_t *)recv_buf);
		    dma_size = *(((uint64_t *)recv_buf) + 1);

		    // Validate sent region
		    if ((char *)dma_addr < shmem + DMA_REGION_OFFSET 
			    || (char *)dma_addr + dma_size >= shmem + SHMEM_SIZE) {
			fprintf(stderr, "Sent DMA memory region does not lie within valid shmem DMA region:"
					 "addr: 0x%lx, size: %ld\n", dma_addr, dma_size);
			goto err_unmap;
		    }	

		    ret = tcp_recv_data((void *)dma_addr, dma_size, 0);
		    if (ret != 0) {
			perror("Recv of DMA region failed");
			goto err_unmap;
		    }

		    break;

		default:
		    printf("Error: Unknown type\n");
		    goto err_unmap;
	    }
	    
	}


	// Will never reach that
	printf("tcp_client: end %d\n", ret);
	exit(EXIT_SUCCESS);

err_unmap:
	munmap(shmem, SHMEM_SIZE);
err:
	exit(EXIT_FAILURE);
}

