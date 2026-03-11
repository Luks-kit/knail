# .gdbinit
target remote localhost:1234
# Set the architecture manually to ensure consistency
set arch i386:x86-64
symbol-file build/knail.elf
