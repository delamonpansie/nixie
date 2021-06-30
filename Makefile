MCU = atmega328p
F_CPU = 16000000UL

# To compile:
# make target=ncm109
# make target=oc2cpu

obj += usart/uart.o
obj += main.o
obj += $(target).o


.PHONY: flash
flash-utk500: $(target).hex
	avrdude -p m328p -c stk500 -B4MHz -P /dev/ttyUTK500 -U flash:w:$<

.PHONY: flash
flash-serial: $(target).hex
	avrdude -p m328p -c arduino -P /dev/ttyUSB0 -U flash:w:$<

include rules.mk
