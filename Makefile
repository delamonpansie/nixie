MCU = atmega328p
F_CPU = 16000000UL

# To compile:
#   1. make target=ncm109
#   2. make target=oc2cpu

# To flash:
#   1. make target=ncm109 flash-utk500
#   2. make target=ncm109 flash-serial
#   3. make target=ncm109 addr=<esp-link-ip-addr> flash-esp-link

obj += usart/uart.o
obj += main.o
obj += $(target).o

main.o: CFLAGS += -DVERSION='"$(shell git rev-parse HEAD)"'

.PHONY: flash-utk500
flash-utk500: $(target).hex
	avrdude -p m328p -c stk500 -B4MHz -P /dev/ttyUTK500 -U flash:w:$<

.PHONY: flash-serial
flash-serial: $(target).hex
	avrdude -p m328p -c arduino -P /dev/ttyUSB0 -U flash:w:$<

.PHONY: flash-esp-link
flash-esp-link: $(target).hex
	avrflash $(addr) $<

include rules.mk
