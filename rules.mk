.SUFFIXES:
.SECONDARY:

define var_test
  ifndef $(1)
    $$(error $(1) undefined)
  endif
endef
$(foreach var,MCU F_CPU obj,$(eval $(call var_test,$(var))))


CC = avr-gcc
CPP = avr-gcc
COMMON = -mmcu=$(MCU)
CFLAGS += $(COMMON)
CFLAGS += -Wall -Wno-char-subscripts
CFLAGS += -gdwarf-2 -std=gnu99 -DF_CPU=$(F_CPU) -Os
CFLAGS += -funsigned-char -funsigned-bitfields -fpack-struct -fshort-enums
CFLAGS += -fdata-sections -ffunction-sections
CFLAGS += -MD -MP
LDFLAGS = $(COMMON)
# disable generaton of map, because avr-gcc (GCC) 5.4.0 from Debian bullseye crashes
# LDFLAGS +=  -Wl,-Map=$(target).map
LDFLAGS += -Wl,--gc-sections
HEX_FLASH_FLAGS += -R .eeprom -R .fuse -R .lock -R .signature
HEX_EEPROM_FLAGS += -j .eeprom
HEX_EEPROM_FLAGS += --set-section-flags=.eeprom="alloc,load"
HEX_EEPROM_FLAGS += --change-section-lma .eeprom=0 --no-change-warnings


$(obj): Makefile rules.mk
dep = $(patsubst %.o,%.d,$(obj))

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

%.elf: $(obj) %.o
	 $(CC) $(LDFLAGS) $^ $(LIBDIRS) $(LIBS) -o $@

%.hex: %.elf
	avr-objcopy -O ihex $(HEX_FLASH_FLAGS) $< $@

%.eep: %.elf
	-avr-objcopy $(HEX_EEPROM_FLAGS) -O ihex $< $@ || exit 0

%.lss: %.elf
	avr-objdump -h -S $< > $@

.PHONY: %.size
%.size: %.elf
	@echo
	@avr-size -C --mcu=$(MCU) $<

.PHONY: clean
clean:
	-rm -rf $(obj) $(dep) $(foreach ext,elf hex eep lss map,$(target).$(ext))

-include $(dep)

