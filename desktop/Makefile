# xwired: test a wired GIP controller on macOS or Linux (needs libusb-1.0)
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo -I/opt/homebrew/include/libusb-1.0)
USB_LIBS := $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo -L/opt/homebrew/lib -lusb-1.0)
# micro-ecc: only P-256, portable C (no assembly)
ECC_FLAGS = -DuECC_SUPPORTS_secp160r1=0 -DuECC_SUPPORTS_secp192r1=0 \
	-DuECC_SUPPORTS_secp224r1=0 -DuECC_SUPPORTS_secp256k1=0 -DuECC_PLATFORM=uECC_arch_other
# gip.c keeps the wireless transport too, so the adapter code links in (unused)
CORE = ../core/mt76.c ../core/dongle.c ../core/gip.c ../core/sha256.c
SRC = xwired.c $(CORE)

xwired: $(SRC) $(wildcard ../core/*.h) uECC.o
	$(CC) $(CFLAGS) $(USB_CFLAGS) -o $@ $(SRC) uECC.o $(USB_LIBS)

uECC.o: ../third_party/micro-ecc/uECC.c
	$(CC) -O2 $(ECC_FLAGS) -c -o $@ $<

clean:
	rm -f xwired uECC.o

.PHONY: clean
