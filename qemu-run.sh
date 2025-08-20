# QEMU main branch with only vfio-user EDU (no SPDM) and vsock
./qemu/build/qemu-system-x86_64 \
	-m 8G \
	-object memory-backend-memfd,id=sysmem-file,size=8G \
	-numa node,memdev=sysmem-file \
	-smp 8 \
	-kernel $1/spdm-linux/arch/x86/boot/bzImage \
	-append "console=ttyS0 root=/dev/sda rw earlyprintk=serial net.ifnames=0 nokaslr" \
	-drive file=$2/qemu-image.img,format=raw \
	-object memory-backend-file,size=1M,share=on,mem-path=/dev/shm/ivshmem,id=hostmem \
	-device ivshmem-plain,memdev=hostmem \
	-net user,host=10.0.2.10,hostfwd=tcp:127.0.0.1:10021-:22 \
	-net nic,model=e1000 \
	-enable-kvm \
	-nographic \
	-cpu EPYC-v4,+vpclmulqdq \
	-no-reboot \
	-machine q35,confidential-guest-support=sev0 \
	-bios OVMF.fd \
	-device disagg-fake-pci,bar-size=1048576 \
	-object sev-snp-guest,id=sev0,cbitpos=51,reduced-phys-bits=1,policy=0x30000 \
	2>&1 | tee vm.log

