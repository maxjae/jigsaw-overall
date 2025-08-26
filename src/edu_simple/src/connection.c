#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>

#include "connection.h"
#include "sec_disagg.h"

//#define CONFIG_DISAGG_DEBUG_MMIO

#define SHMEM_FILE "/dev/shm/ivshmem"  // Adjust this path as needed
#define SHMEM_SIZE (1 << 20)  // 1 MB, adjust as needed
#define READ_DOORBELL_OFFSET 0
#define WRITE_DOORBELL_OFFSET 1
#define DOORBELL_SIZE 1  // 1 byte for each doorbell
#define TOTAL_DOORBELL_SIZE (DOORBELL_SIZE * 2)
#define MMIO_REGION_OFFSET (8)
#define DMA_REGION_OFFSET (1 << 12) // 4K aligned
#define DMA_SIZE (SHMEM_SIZE - DMA_REGION_OFFSET)

/* Offsets in the shared memory with special values */
#define OFFSET_PROXY_SHMEM (256)
//#define OFFSET_BAR_PHYS_ADDR (264)

static void *shmem = NULL;
static volatile uint8_t *read_doorbell = NULL;
static volatile uint8_t *write_doorbell = NULL;

static int create_or_open_shmem_file() {
    int fd = open(SHMEM_FILE, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        perror("Failed to open or create shared memory file");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("Failed to get file status");
        close(fd);
        return -1;
    }

    if (st.st_size != SHMEM_SIZE) {
        if (ftruncate(fd, SHMEM_SIZE) < 0) {
            perror("Failed to set file size");
            close(fd);
            return -1;
        }
        printf("Created shared memory file with size %d bytes\n", SHMEM_SIZE);
    } else {
        printf("Opened existing shared memory file with correct size\n");
    }

    return fd;
}

static int init_shared_memory() {
    int fd = create_or_open_shmem_file();
    if (fd < 0) {
        return -1;
    }

    shmem = mmap(NULL, SHMEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shmem == MAP_FAILED) {
        perror("Failed to mmap shared memory");
        close(fd);
        return -1;
    }

    read_doorbell = (volatile uint8_t *)shmem + READ_DOORBELL_OFFSET;
    write_doorbell = (volatile uint8_t *)shmem + WRITE_DOORBELL_OFFSET;
    close(fd);
    
    // Initialize doorbells to 0
    *read_doorbell = 0;
    *write_doorbell = 0;

    // write proxyShmem address into shmem
    *((uint64_t *)(shmem + OFFSET_PROXY_SHMEM)) = (uint64_t) shmem + DMA_REGION_OFFSET; 


    msync(shmem, TOTAL_DOORBELL_SIZE, MS_SYNC);
    
    return 0;
}

static void wait_for_write_doorbell_set() {
    while (__atomic_load_n(write_doorbell, __ATOMIC_ACQUIRE) == 0) { }
}

static void wait_for_read_doorbell_clear() {
    while (__atomic_load_n(read_doorbell, __ATOMIC_ACQUIRE) != 0) { }
}

static int ivshmem_mmio_region_read(void *buf) {
    size_t msg_size;

    wait_for_write_doorbell_set();

    memcpy(disagg_crypto_mmio_global.buf, shmem + MMIO_REGION_OFFSET, 1 + sizeof(struct mmio_message_header) + disagg_crypto_mmio_global.authsize);

    __atomic_store_n(write_doorbell, 0, __ATOMIC_RELEASE);

    if (*((uint8_t *)disagg_crypto_mmio_global.buf) == OP_READ)
	msg_size = sizeof(struct mmio_message_header) - sizeof(uint64_t);
    else
	msg_size = sizeof(struct mmio_message_header);

    return disagg_mmio_decrypt(disagg_crypto_mmio_global.buf + 1, buf, msg_size);
}

static int ivshmem_mmio_region_write(void *buf, size_t count) {

    if (disagg_mmio_encrypt(buf, disagg_crypto_mmio_global.buf + 1, count) != 0)
	return 1;

    // Op-type can only be OP_READ
    *((uint8_t *) disagg_crypto_mmio_global.buf) = OP_READ;

    wait_for_read_doorbell_clear();

    memcpy(shmem + MMIO_REGION_OFFSET, disagg_crypto_mmio_global.buf, 1 + count + disagg_crypto_mmio_global.authsize);

    __atomic_store_n(read_doorbell, 1, __ATOMIC_RELEASE);

    return 0;
}

void *run_shmem_app(disagg_pci_dev_info *pci_info, void *opaque) {
    if (init_shared_memory() < 0) {
        printf("SHMEM: init_shared_memory failed\n");
    }
    
    if (disagg_init_crypto()) {
	printf("SHMEM: disagg_init_crypto failed\n");
    }

    printf("connection.c: In shmem app\n");

    printf("SHMEM application started. Waiting for messages...\n");

    char data[sizeof(uint64_t)];
    bool is_write = false;
    region_access_cb_t *cb;
    loff_t offset;

    while (1) {
        struct mmio_message_header header;
        if (ivshmem_mmio_region_read(&header) != 0) {
            perror("Failed to read message");
            continue;
        }

#ifdef CONFIG_DISAGG_DEBUG_MMIO
        printf("Received message: operation=%u, address=0x%lx, length=%u\n",
               header.operation, header.address, header.length);
#endif

        switch (header.operation) {

	    case OP_READ:
#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("connection.c: OP_READ: Received read operation: Address 0x%lx, Length %u\n", header.address, header.length);
#endif
		int pci_region = 0;

#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("connection.c: OP_READ: Got PCI region: %d ", pci_region);
#endif
		cb = pci_info->regions[pci_region].cb;

		offset = header.address;
#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("with offset %ld\n", offset);
#endif
		is_write = false;
		uint32_t ret = cb(opaque, data, header.length, offset, is_write);

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

		if (ivshmem_mmio_region_write(data, sizeof(data)) != 0) {
		    perror("Failed to write response");
		    continue;
		}
		continue;

	    case OP_WRITE:
#ifdef CONFIG_DISAGG_DEBUG_MMIO
		printf("connection.c: OP_WRITE: Received write operation: Address 0x%lx, Length %u\n", header.address, header.length);
#endif

		memcpy(data, &header.value, sizeof(data));

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
		ret = cb(opaque, data, header.length, offset, is_write);

		if (ret != header.length) {
		    printf("connection.c: OP_WRITE: Writing %lu bytes failed\n", header.length);
		}

		continue;

	    default:
		fprintf(stderr, "Unknown operation: %d\n", header.operation);
		continue;
        }

        printf("Response sent. Waiting for next message...\n");
    }

    munmap(shmem, SHMEM_SIZE);
}

