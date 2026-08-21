CC=clang
LD=ld
NASM=nasm
HOSTCC=cc
CFLAGS=--target=x86_64-elf -std=gnu89 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mcmodel=kernel -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -m64 -O2 -Wall -Wextra -Werror
GMCFLAGS=--target=x86_64-elf -std=gnu89 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie -mcmodel=small -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -msoft-float -m64 -O2 -Wall -Wextra -Werror -ffunction-sections -fdata-sections
HOSTCFLAGS=-std=gnu89 -O2 -Wall -Wextra -Werror
LDFLAGS=-nostdlib -static -z max-page-size=0x1000 -T linker.ld
OBJS=start.o isr.o kernel.o core.o virtio.o net.o tcpstore.o programs.o shell.o
LIMINE_COMMIT=ee5d29cd0a8034612dcd1df3f00052480db785c5

.PHONY: all clean bootfiles iso run store check test gm

all: gmker.elf gmstored gmpack

gmker.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

gmstored: gmstored.c
	$(HOSTCC) $(HOSTCFLAGS) $< -o $@

gmpack: gmpack.c gmabi.h
	$(HOSTCC) $(HOSTCFLAGS) $< -o $@

start.o: start.asm
	$(NASM) -f elf64 $< -o $@

isr.o: isr.asm
	$(NASM) -f elf64 $< -o $@

%.o: %.c gmker.h gmabi.h limine.h
	$(CC) $(CFLAGS) -c $< -o $@

gm: gmpack gmprog.h gmabi.h gmprog.ld
	@set -e; test -n "$(SRC)" && test -n "$(OUT)" || { echo "usage: make gm SRC=program.c OUT=program.gm"; exit 1; }; trap 'rm -f .gm-build.o .gm-build.elf' EXIT; $(CC) $(GMCFLAGS) -c "$(SRC)" -o .gm-build.o; $(LD) -nostdlib -static -T gmprog.ld .gm-build.o -o .gm-build.elf; ./gmpack .gm-build.elf "$(OUT)"

check: gmker.elf gmstored gmpack
	@size gmker.elf
	@echo "===== OK GMKER 3.0 ====="

test: check iso
	@./test_runtime.sh

bootfiles:
	@rm -rf .limine-fetch
	@git clone -q https://github.com/limine-bootloader/limine.git .limine-fetch
	@cd .limine-fetch && git checkout -q $(LIMINE_COMMIT) && $(MAKE) -s limine
	@cp .limine-fetch/limine-bios.sys .
	@cp .limine-fetch/limine-bios-cd.bin .
	@cp .limine-fetch/limine-uefi-cd.bin .
	@cp .limine-fetch/BOOTX64.EFI .
	@cp .limine-fetch/BOOTIA32.EFI .
	@cp .limine-fetch/limine .
	@rm -rf .limine-fetch
	@echo "===== OK LIMINE $(LIMINE_COMMIT) ====="

iso: gmker.elf limine limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI BOOTIA32.EFI
	@rm -rf .iso-root
	@mkdir -p .iso-root/boot/limine .iso-root/EFI/BOOT
	@cp gmker.elf .iso-root/gmker.elf
	@cp limine.conf .iso-root/boot/limine/limine.conf
	@cp limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin .iso-root/boot/limine/
	@cp BOOTX64.EFI BOOTIA32.EFI .iso-root/EFI/BOOT/
	@xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label .iso-root -o gmker.iso
	@./limine bios-install gmker.iso
	@rm -rf .iso-root
	@echo "===== OK gmker.iso ====="

run: iso
	@set +e; qemu-system-x86_64 -m 128M -cdrom gmker.iso -display none -serial mon:stdio -no-reboot -device isa-debug-exit,iobase=0x501,iosize=0x04 -netdev user,id=net0 -device virtio-net-pci,disable-legacy=on,disable-modern=off,netdev=net0; rc=$$?; if [ $$rc -eq 33 ]; then exit 0; else exit $$rc; fi

store: gmstored
	./gmstored --root store --host 0.0.0.0 --port 7070

clean:
	rm -f *.o gmker.elf gmker.iso gmstored gmpack .gm-build.o .gm-build.elf
	rm -rf .iso-root .limine-fetch
