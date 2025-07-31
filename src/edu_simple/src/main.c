#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <unistd.h>

#include "edu.h"
#include "mmio.h"
#include "tcp_server.h"

int main(int argc, char **argv) {

    /** Initialize everything related to the device emulation **/
    EduState *edu = init_edu_device();

    if (edu == NULL)
	exit(EXIT_FAILURE);

    // Setup region info
    disagg_pci_dev_info *disagg_pci_info = (disagg_pci_dev_info *) malloc(sizeof(disagg_pci_dev_info));
    if (!disagg_pci_info) {
	printf("malloc failed\n");
	exit(EXIT_FAILURE);
    }

    for (int i = 0; i < PCI_NUM_REGIONS; i++)
    {
        disagg_pci_info->regions[i].addr = &(edu->regions[i].addr);
        disagg_pci_info->regions[i].size = &(edu->regions[i].size);
        disagg_pci_info->regions[i].vmPhys = 0;
        disagg_pci_info->regions[i].vmPhys_valid = false;
	disagg_pci_info->regions[i].cb = edu->regions[i].cb;

        printf("main.c: Setting for region %d, proxy Address: 0x%" PRIx64 ", size: %lu\n", i, *(disagg_pci_info->regions[i].addr), *(disagg_pci_info->regions[i].size));
    }

    /** Setup TCP **/
    int ret;
    ret = init_tcp(argc, argv);
    if (ret != 0)
	exit(EXIT_FAILURE);

    run_mmio_app(disagg_pci_info, edu);
}

