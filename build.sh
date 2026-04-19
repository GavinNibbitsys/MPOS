#!/bin/bash
set -e

# Cleaning
rm -rf iso *.o *.bin *.iso

# 1. Assemble Boot Stub
nasm -f elf32 boot.asm -o boot.o

# 2. Compile C Kernel
# -ffreestanding: Tells GCC we are not in Linux (no printf, no main() args)
# -m32: Compile for 32-bit x86
gcc -m32 -ffreestanding -c kernel.c -o kernel.o

# 3. Link them together
ld -m elf_i386 -T linker.ld -o kernel.bin boot.o kernel.o

# 4. Check Multiboot
if grub-file --is-x86-multiboot kernel.bin; then
    echo "Multiboot Confirmed"
else
    echo "Error: Not Multiboot"
    exit 1
fi

# 5. Build ISO
mkdir -p iso/boot/grub
cp kernel.bin iso/boot/
cp grub.cfg iso/boot/grub/
grub-mkrescue -o mpos_c.iso iso
echo "Done! mpos_c.iso created."