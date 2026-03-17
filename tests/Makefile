# Build all Knail test binaries
# Usage: make
# Output: test_io, test_fork, test_exec, test_mm, test_tls, test_time, test_ioctl

CC      = gcc
CFLAGS  = -static -nostdlib -nostdinc -ffreestanding \
          -fno-stack-protector -fno-pie -no-pie \
          -O2 -Wall -Wextra -I/usr/include/
TESTS   = test_io test_fork test_exec test_mm test_tls test_time test_ioctl

all: $(TESTS)

%: %.c syscall_stubs.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)

# Copy to disk image (adjust DISK_MOUNT as needed)
DISK_MOUNT ?= /mnt/disk
install: $(TESTS)
	mkdir -p $(DISK_MOUNT)/tests
	cp $(TESTS) $(DISK_MOUNT)/tests/

.PHONY: all clean install
