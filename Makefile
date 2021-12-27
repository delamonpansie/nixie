MCU = atmega328p
F_CPU = 16000000UL

.PHONY: help
help:
	@echo To compile:
	@echo \	1. make ncm109.elf
	@echo \	2. make oc2cpu.elf
	@echo
	@echo To flash:
	@echo \	1. make target=ncm109 flash-utk500
	@echo \	2. make target=ncm109 flash-serial
	@echo \	3. make target=ncm109 addr=\<esp link addr\> flash-esp-link
	@echo
	@echo Other targets:
	@echo \	1. make \$$board.elf
	@echo \	2. make \$$board.hex
	@echo \	3. make \$$board.eep
	@echo \	4. make \$$board.lss
	@echo \	5. make \$$board.size

# ncm109.o and oc2cpu.o implicitly included in corresponding %.elf target
obj += usart/uart.o
obj += main.o

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
