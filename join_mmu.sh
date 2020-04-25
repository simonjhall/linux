rm -f stripped.bin
rm -f stripped
mv vmlinux.gz stripped.gz
gzip -d stripped.gz
m68k-linux-gnu-objcopy -O binary stripped stripped.bin
cat mmu_header.bin > combined.bin
cat stripped.bin >> combined.bin

