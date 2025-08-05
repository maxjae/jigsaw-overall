#include <sys/socket.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

#include "../../include/common.h"

int fd = -1;
char send_buf[64];

// Register signals with signal handler to close socket
void signal_handler(int signum) {
    printf("signal_handler: received signal %d\n", signum);

    if (fd != -1) 
	close(fd);

    exit(EXIT_FAILURE);
}

// Calls recv until has received size bytes
int tcp_recv_data(void *buf, size_t size, int flags) {
    size_t size_recv = 0;
    ssize_t ret;


    while (size_recv < size) {
	ret = recv(fd, buf + size_recv, size - size_recv, flags);

	// Check for error
	if (ret <= 0) {
	    if (ret == 0) {
		printf("Connection was closed by peer\n");
		return -1;
	    } else if (ret == -1) {
		// Error
		return -1;
	    }
	}

	size_recv += ret;
    }

    return 0;
}

int tcp_send_buf(const char *buf, size_t size) {
    if ((size_t) send(fd, buf, size, 0) != size)
	return -1;

    return 0;
}

int init_tcp(int argc, char **argv) 
{
    int op, ret = 0;
    char *remoteAddrString = NULL, *remotePortString = NULL;
    char *localAddrString = NULL, *localPortString = NULL;
    int remotePort = -1;
    int localPort = -1;
    struct sockaddr_in lsaddr, ssaddr; // listening/server sock addr

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

    /*** Read command line arguments ***/
    struct option long_opts[] = {
	{ "remoteAddr", 1, NULL, 'a' },
	{ "remotePort", 1, NULL, 'b' },
	{ "localAddr", 1, NULL, 'c' },
	{ "localPort", 1, NULL, 'd' },
	{ NULL, 0, NULL, 0 }
    };

    while ((op = getopt_long(argc, argv, "a:b:c:d:", long_opts, NULL)) != -1) {
	switch (op) {
	case 'a':
	    remoteAddrString = optarg;
	    break;
	case 'b':
	    remotePortString = optarg;
	    break;
	case 'c':
	    localAddrString = optarg;
	    break;
	case 'd':
	    localPortString = optarg;
	    break;
	default:
	    printf("usage: %s\n", argv[0]);
	    printf("\t[--remoteAddr [IP address of remote host's interface]      or -a]\n");
	    printf("\t[--remotePort [Port of remote host]                        or -b]\n");
	    printf("\t[--localAddr [IP address of local interface]               or -c]\n");
	    printf("\t[--localPort [Local port to use]                           or -d]\n");
	    goto out;
	}
    }

    if (!remoteAddrString || !remotePortString || !localAddrString || !localPortString) {
	printf("All four arguments have to be specified\n");
	printf("usage: %s\n", argv[0]);
	printf("\t[--remoteAddr [IP address of remote host's interface]      or -a]\n");
        printf("\t[--remotePort [Port of remote host]                        or -b]\n");
        printf("\t[--localAddr [IP address of local interface]               or -c]\n");
        printf("\t[--localPort [Local port to use]                           or -d]\n");
	goto out;
    }

    remotePort = atoi(remotePortString);
    if (remotePort < 1024 || remotePort >= 65536) {
	printf("Invalid port number for remotePort\n");
	goto out;
    }

    localPort = atoi(localPortString);
    if (localPort < 1024 || localPort >= 65536) {
	printf("Invalid port number for localPort\n");
	goto out;
    }

    /*** Bind local address to socket ***/
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == -1) {
	perror("Creating of local socket failed");
	goto out;
    }

     if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int)) != 0)
	printf("TCP_NODELAY could not be set for the socket. May result in worse performance\n");


    // Convert command line arguments to address info for local address
    lsaddr.sin_family = AF_INET;
    lsaddr.sin_port = htons(localPort);
    if (0 == inet_aton(localAddrString, &lsaddr.sin_addr)) {
	printf("Invalid localAddr: %s\n", localAddrString);
	goto err_sock;
    }

    ret = bind(fd, (struct sockaddr *)&lsaddr, sizeof(lsaddr));
    if (ret != 0) {
	perror("Bind to local address failed");
	goto err_sock;
    }

    /*** Connect to server ***/
    // Convert command line arguments to address info for remote server address
    ssaddr.sin_family = AF_INET;
    ssaddr.sin_port = htons(remotePort);
    if (0 == inet_aton(remoteAddrString, &ssaddr.sin_addr)) {
	printf("Invalid remoteAddr: %s\n", remoteAddrString);
	goto err_sock;
    }

    ret = connect(fd, (struct sockaddr *)&ssaddr, sizeof(ssaddr));
    if (ret != 0) {
	perror("Connect to server failed");
	goto err_sock;
    }

    return 0;

err_sock:
    if (fd != -1)
	close(fd);
out:
    return 1;
} 

