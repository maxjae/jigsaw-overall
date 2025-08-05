#include <sys/socket.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <arpa/inet.h>

#include "tcp_server.h"

int lfd = -1; // Listening fd
int cfd = -1; // Client fd
struct disagg_regions_tcp *regions_tcp;

// Register signals with signal handler to close socket
void signal_handler(int signum) {
    printf("signal_handler: received signal %d\n", signum);

    if (lfd != -1) 
	close(lfd);
    
    if (cfd != -1)
	close(cfd);

    exit(EXIT_FAILURE);
}

int tcp_send(void *buf, size_t count)
{
    ssize_t ret;

    ret = send(cfd, buf, count, 0);	

    if (ret != (ssize_t) count) {
	printf("send failed\n");
	return 1;
    }

    return 0;
}

// Calls recv until has received size bytes
static int recv_data(int ifd, void *buf, size_t size, int flags)
{
    size_t size_recv = 0;
    ssize_t ret;


    while (size_recv < size) {
	ret = recv(ifd, buf + size_recv, size - size_recv, flags);

	// Check for error
	if (ret <= 0) {
	    if (ret == 0) {
		// Peer closed connection
		printf("Connection was closed by peer\n");
		close(ifd);
		close(lfd);
		exit(EXIT_SUCCESS);
	    } else if (ret == -1) {
		// Error
		perror("recv failed");
		return 1;
	    }
	}

	size_recv += ret;
    }

    return 0;
}

void *tcp_recv_mmio_request(void)
{
    void *ret;
    if (regions_tcp->recv_buf_last_valid != -1) {
	// There is a buffered request available
	ret = regions_tcp->recv_buf[regions_tcp->recv_buf_next];
	++regions_tcp->recv_buf_next;
	if (regions_tcp->recv_buf_next > regions_tcp->recv_buf_last_valid) {
	    // Read all buffered data
	    regions_tcp->recv_buf_last_valid = -1;
	    regions_tcp->recv_buf_next = 0;
	}

	return ret;
    }

    // recv new data
    if (recv_data(cfd, regions_tcp->recv_buf, 1 + sizeof(struct mmio_message), 0) != 0) {
	printf("Error receiving data\n");
	return NULL;
    }

    return (char *)regions_tcp->recv_buf;
}

void tcp_read_dma(uint64_t addr, size_t count)
{
    size_t size_to_send = 1 + (sizeof(uint64_t) * 2);
    ssize_t ret;
    uint8_t type;

    /*** Send request for DMA region to proxy ***/
    regions_tcp->dma_mem[0] = OP_DMA_TO_DEVICE;
    *((uint64_t *)(regions_tcp->dma_mem + 1)) = addr;
    *((uint64_t *)(regions_tcp->dma_mem + sizeof(uint64_t) + 1)) = count;

    ret = send(cfd, regions_tcp->dma_mem, size_to_send, 0);	

    if (ret != (ssize_t) size_to_send) {
	printf("send failed for DMA to device request\n");
	return;
    }


    /*** Wait for reply to request ***/
    do {
	// Read only first byte to determine type
	if (recv_data(cfd, &type, 1, 0) != 0) {
	    printf("recv failed for read of TYPE\n");
	    return;
	}	

	// If read message is a MMIO request, store in buffer
	if (type == OP_DMA_TO_DEVICE) {
	    // Received response to DMA request
	    if (recv_data(cfd, 
			  regions_tcp->dma_buf, 
			  count, 0) != 0) {
		printf("recv failed for dma read response\n");
		return;
	    }
	} else {
	    // Received message is MMIO
	    ++regions_tcp->recv_buf_last_valid;

	    void *dst = regions_tcp->recv_buf + ((1 + sizeof(struct mmio_message)) * regions_tcp->recv_buf_last_valid); 

	    if (recv_data(cfd, 
			  dst + 1, 
			  sizeof(struct mmio_message), 0) != 0) {
		printf("recv failed for filling of mmio requests in buffer\n");
		return;
	    }
	    memcpy(dst, &type, 1);
	}

    } while(type != OP_DMA_TO_DEVICE);
}

void tcp_write_dma(uint64_t addr, size_t count)
{
    size_t size_to_send = 1 + (sizeof(uint64_t) * 2) + count;
    ssize_t ret;

    /*** Send DMA region to proxy ***/
    regions_tcp->dma_mem[0] = OP_DMA_FROM_DEVICE;
    *((uint64_t *)(regions_tcp->dma_mem + 1)) = addr;
    *((uint64_t *)(regions_tcp->dma_mem + 1 + sizeof(uint64_t))) = count;

    ret = send(cfd, regions_tcp->dma_mem, size_to_send, 0);	

    if (ret != (ssize_t) size_to_send) {
	printf("send failed for DMA to device request\n");
	return;
    }
}

int init_tcp(int argc, char **argv)
{
    int op, ret = 0;
    uint32_t ret_size;
    char *localAddrString = NULL, *localPortString = NULL;
    int localPort = -1;
    struct sockaddr_in ssaddr, csaddr; // server/client sock addr
    char csaddrString[INET_ADDRSTRLEN + 1]; // Used to print the client address

    /*** Register signal handler ***/
    struct sigaction sig_action[1];
    memset(&sig_action[0], 0, sizeof(struct sigaction));
    sig_action[0].sa_handler = signal_handler;

    if (sigaction(SIGINT, &sig_action[0], NULL) != 0) {
	perror("sigaction failed for SIGINT");
	goto out;
    }	
    if (sigaction(SIGTERM, &sig_action[0], NULL) != 0) {
	perror("sigaction failed for SIGINT");
	goto out;
    }	

    /*** Read ip address and port for listening socket from command line arguments ***/
    struct option long_opts[] = {
	{ "localAddr", 1, NULL, 'a' },
	{ "local", 1, NULL, 'b' },
	{ NULL, 0, NULL, 0 }
    };

    while ((op = getopt_long(argc, argv, "a:b:c:d:", long_opts, NULL)) != -1) {
	switch (op) {
	case 'a':
	    localAddrString = optarg;
	    break;
	case 'b':
	    localPortString = optarg;
	    break;
	default:
	    printf("usage: %s\n", argv[0]);
	    printf("\t[--localAddr [IP address of local interface]              or -a]\n");
	    printf("\t[--localPort [Port to use]                                or -b]\n");
	    goto out;
	}
    }

    if (!localAddrString || !localPortString) {
	printf("All two arguments have to be specified\n");
	printf("usage: %s\n", argv[0]);
	printf("\t[--localAddr [IP address of local interface]              or -a]\n");
	printf("\t[--localPort [Port to use]                                or -b]\n");
	goto out;
    }

    localPort = atoi(localPortString);
    if (localPort < 1024 || localPort >= 65536) {
	printf("Invalid port number for localPort\n");
	goto out;
    }

    /*** Create listening socket ***/
    int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lfd == -1) {
	perror("Creating of listening socket failed");
	goto out;
    }

    // Convert command line arguments to address info for server address (address of this interface)
    ssaddr.sin_family = AF_INET;
    ssaddr.sin_port = htons(localPort);
    if (0 == inet_aton(localAddrString, &ssaddr.sin_addr)) {
	printf("Invalid localAddr: %s\n", localAddrString);
	goto err_sock;
    }

    ret = bind(lfd, (struct sockaddr *)&ssaddr, sizeof(ssaddr));
    if (ret != 0) {
	perror("Bind to server address failed");
	goto err_sock;	
    }

    ret = listen(lfd, 1);
    if (ret != 0){
	perror("Marking the socket as listening failed");
	goto err_sock;
    }


    /*** Accept the incoming connection ***/
    ret_size = sizeof(csaddr); // size argument of struct sockaddr is needed as pointer
    cfd = accept(lfd, (struct sockaddr *)&csaddr, &ret_size); // Blocks until incoming connection as lfd is not marked as non-blocking

    if (cfd == -1) {
	perror("Accept failed");
	ret = 1;
	goto err_sock;
    }

    if (ret_size != sizeof(csaddr)) {
	printf("Returned size of client address is different than expected\n");
	ret = 1;
	goto err_acc;
    }

    if (setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int)) != 0)
	printf("TCP_NODELAY could not be set for the socket. May result in worse performance\n");


    // Print the address information of connected client
    if (NULL == inet_ntop(AF_INET, &csaddr.sin_addr.s_addr, csaddrString, sizeof(csaddrString) - 1)) {
	perror("inet_ntop failed");
	ret = 1;
	goto err_acc;
    }

    csaddrString[sizeof(csaddrString) - 1] = '\0';
    printf("Accepted connection to client: address = %s, port = %d\n", csaddrString, ntohs(csaddr.sin_port));


    /*** Alloc struct regions_tcp ***/
    regions_tcp = malloc(sizeof(struct disagg_regions_tcp));
    if (!regions_tcp) {
	printf("malloc failed for struct disagg_regions_tcp\n");
	ret = 1;
	goto err_acc;
    }

    regions_tcp->recv_buf_last_valid = -1;
    regions_tcp->recv_buf_next = 0;

    regions_tcp->dma_buf = regions_tcp->dma_mem + 1 + (sizeof(uint64_t) * 2);

    return 0; 

err_acc:
    if (cfd != -1)
	close(cfd);
err_sock:
    if (lfd != -1)
	close(lfd);
out:
    return 1;
}
