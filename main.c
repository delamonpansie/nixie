#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <avr/interrupt.h>
#include "avr/io.h"
#include "avr/wdt.h"
#include "avr/eeprom.h"
#include <util/delay.h>
#include <util/twi.h>
#include "util/crc16.h"

#include "usart/uart.h"
#include "nixie.h"

void __attribute__((naked,section(".init3")))
watchdog_disable(void)
{
        MCUSR = 0;
        wdt_disable();
}

struct config config = {
        .tube_pwm_freq = 15, // 150Hz
        .tube_pwm_duty = 90,
        .antipoison_start = 0,
        .antipoison_duration = 24,
};

static struct time {
        unsigned char dirty;
        unsigned char sec;
        unsigned char min;
        unsigned char hour : 6;
        unsigned char twentyfour : 1;
        unsigned char day;
        unsigned char date;
        unsigned char month;
        unsigned char year;

} time;

static uint8_t
bin2bcd(uint8_t bin)
{
        if (bin == 0)
                return 0;

        uint8_t bit = 0x40; //  99 max binary
        while ((bin & bit) == 0) // skip to MSB
                bit >>= 1;

        uint8_t bcd = 0;
        uint8_t carry = 0;
        while (1) {
                bcd <<= 1;
                bcd += carry; // carry 6s to next BCD digits (10 + 6 = 0x10 = LSB of next BCD digit)
                if (bit & bin)
                        bcd |= 1;
                bit >>= 1;
                if (bit == 0)
                        return bcd;
                carry = ((bcd + 0x33) & 0x88) >> 1; // carrys: 8s -> 4s
                carry += carry >> 1; // carrys 6s
        }
        return bcd;
}

static uint8_t
bcd2bin(uint8_t bcd)
{
        return (bcd >> 4) * 10 + (bcd & 0xf);
}


#define OP_RING_BITS 3
#define OP_RING_MASK (_BV(OP_RING_BITS)-1)
static volatile char op_ring[_BV(OP_RING_BITS)], opr;
static volatile char opw;

static char
pop_op()
{
        if (opr == opw) return NOP;
        enum op op = op_ring[opr];
        opr = (opr + 1) & OP_RING_MASK;
        return op;
}

static void
push_op(char op)
{
        if (((opw + 1) & OP_RING_MASK) == opr) // ring buffer is full
                return;
        op_ring[opw] = op;
        opw = (opw + 1) & OP_RING_MASK;
}

static void
i2c_init()
{
        // DS3231 supports upto 400kHz I2C
        TWBR = (F_CPU / 400000UL - 16) / 2;
}

#define DS3231_ADDR 0x68

static void
ds3231_sync()
{
        wdt_reset();

#define i2c_op(bit, cond)                                               \
        TWCR = bit|_BV(TWEN)|_BV(TWINT);                                \
        loop_until_bit_is_set(TWCR, TWINT);                             \
        if ((TWSR & 0xf8) != cond) {                                    \
                printf("I2C error: expected "#cond" = 0x%02x, got 0x%02x\n", cond, TWSR & 0xf8); \
                goto out;                                               \
        }
        // reset TW state
        TWCR = 0;

        // Send start condition
        i2c_op(_BV(TWSTA), TW_START);

        // Send slave address, I2C Write
        TWDR = DS3231_ADDR << 1;
        i2c_op(0, TW_MT_SLA_ACK);

        // Position to the register 00h
        TWDR = 0;
        i2c_op(0, TW_MT_DATA_ACK);

        unsigned char *w = &time.sec;
        if (time.dirty) {
                for (char i = 0; i < sizeof time - 1; i++) {
                        TWDR = *w++;
                        i2c_op(_BV(TWEA), TW_MT_DATA_ACK);
                }
                time.dirty = 0;
        } else {
                // Send repeated start
                i2c_op(_BV(TWSTA), TW_REP_START);

                // Send slave address, I2C read
                TWDR = (DS3231_ADDR << 1) + 1;
                i2c_op(0, TW_MR_SLA_ACK);

                for (char i = 0; i < sizeof time - 1; i++) {
                        i2c_op(_BV(TWEA), TW_MR_DATA_ACK);
                        *w++ = TWDR;
                }
                // Last byte is nack
                i2c_op(0, TW_MR_DATA_NACK);
        }
out:
        // Send stop
        TWCR = _BV(TWEN)|_BV(TWINT)|_BV(TWSTO);
#undef i2c_op

        static char prev_sec;
        if (prev_sec != time.sec) {
                prev_sec = time.sec;
                push_op(REFRESH);
        }
}

#define LONG_PRESS _BV(7)

struct button_state {
        char counter;
        char pressed;
        char short_press;
        char long_press;
};
static void
button_decode(unsigned char mask, struct button_state *button)
{
        const char button_max = 10;

        if (mask) {
                if (button->counter < button_max)
                        button->counter++;
                if (button->counter == button_max) {
                        if (button->pressed < 0xff)
                                button->pressed++;
                        if (button->pressed == 53)
                                button->long_press = 1; // push_op((MODE + i) | LONG_PRESS);
                }
        } else {
                if (button->counter > 0)
                        button->counter--;
                if (button->counter == 0 && button->pressed) {
                        if (button->pressed < 50)
                                button->short_press = 1; //push_op(MODE + i);
                        button->pressed = 0;
                }
        }
}

void
button_scan()
{
        if (!uart_read_would_block()) {
                switch (getchar()) {
                case 'u':
                        push_op(UP);
                        break;
                case 'd':
                        push_op(DOWN);
                        break;
                case 'm':
                        push_op(MODE);
                        break;
                case 'M':
                        push_op(MODE|LONG_PRESS);
                        break;
                }
        }

        unsigned char button_mask = button_read();
        static struct button_state mode, up, down;
        button_decode(button_mask & MODE, &mode);
        button_decode(button_mask & UP, &up);
        button_decode(button_mask & DOWN, &down);

        if (mode.long_press) {
                push_op(MODE|LONG_PRESS);
                mode.long_press = 0;
        } else if (up.long_press && down.long_press) {
                push_op(MODE|LONG_PRESS);
                up.long_press = 0;
                down.long_press = 0;
        } else if (up.long_press) {
                push_op(UP|LONG_PRESS);
                up.long_press = 0;
                up.pressed -= 3;
        } else if (down.long_press) {
                push_op(DOWN|LONG_PRESS);
                down.long_press = 0;
                down.pressed -= 3;
        } else if (mode.short_press) {
                push_op(MODE);
                mode.short_press = 0;
        } else if ((up.short_press && down.pressed) || (down.short_press && up.pressed)) {
                push_op(MODE);
                up.short_press = 0;
                down.short_press = 0;
                up.pressed = 0;
                down.pressed = 0;
        } else if (up.short_press) {
                push_op(UP);
                up.short_press = 0;
        } else if (down.short_press) {
                push_op(DOWN);
                down.short_press = 0;
        }

        // ds3231_sync() takes less than 1ms to execute with I2C set to 400kHz
        ds3231_sync();
}

static void
refresh() {
        paint(time.hour, time.min, time.sec, time.sec & 1);
}

static void
time_up(struct time *time)
{
        time->min++;
        if ((time->min & 0x0f) > 9)
                time->min += 6;
        if (time->min >= 0x60) {
                time->hour++;
                time->min = 0;
        }
        if ((time->hour & 0x0f) > 9)
                time->hour += 6;
        if (time->hour >= 0x24)
                time->hour = 0;
        time->sec = 0;
        time->dirty = 1;

}

static void
time_down(struct time *time)
{
        time->min--;
        if (time->min == 0xff) {
                time->min = 0x59;
                time->hour--;
        }
        if ((time->min & 0x0f) > 9)
                time->min -= 6;
        if (time->hour == 0x3f)
                time->hour = 0x23;
        if ((time->hour & 0x0f) > 9)
                time->hour -= 6;
        time->sec = 0;
        time->dirty = 1;
}


static void
config_print()
{
        printf("configuration:\n");
        printf("  tube_pwm_freq:        %d\n", config.tube_pwm_freq);
        printf("  tube_pwm_duty:        %d\n", config.tube_pwm_duty);
        printf("  led_red_brightness:   %d\n", config.led_red_brightness);
        printf("  led_green_brightness: %d\n", config.led_green_brightness);
        printf("  led_blue_brightness:  %d\n", config.led_blue_brightness);
        printf("  antipoison_start:     %d\n", config.antipoison_start);
        printf("  antipoison_duration:  %d\n", config.antipoison_duration);
}

static uint8_t
config_crc(const struct config *cfg)
{
        uint8_t crc = 0, *ptr = (uint8_t *)cfg;
        for (char i = 1; i < sizeof config; i++)
                crc = _crc8_ccitt_update(crc, ptr[i]);
        return crc;
}

static void
config_init()
{
        eeprom_busy_wait();

        struct config tmp = { .crc = 0 };
        eeprom_read_block(&tmp, (void *)13, sizeof config);
        if (0 && tmp.crc == config_crc(&tmp))
                memcpy(&config, &tmp, sizeof config);
}

static void
config_write()
{
        config.crc = config_crc(&config);
        eeprom_write_block(&config, (void *)13, sizeof config);
}

struct param {
        unsigned char id;
        unsigned char *val;
        unsigned char flags;
        unsigned char lower_bound;
        unsigned char upper_bound;
        const char *descr;
} param[] = {
        {0x01, &config.led_red_brightness,	0,	0,	10, "red led pwm"},
        {0x02, &config.led_green_brightness,	0,	0,	10, "green led pwm"},
        {0x03, &config.led_blue_brightness,	0,	0,	10, "blue led pwm"},
        {0x04, &config.tube_pwm_freq,		0,	10,	60, "tube pwm"},
        {0x05, &config.tube_pwm_duty,		0,	10,	99, "tube duty"},
        {0x06, &config.antipoison_start,	0,	0,	24, "antipoison start"},
        {0x07, &config.antipoison_duration,	0,	0,	24, "antipoison duration"},
        {0xff, NULL, 				0, 	0, 	0,  NULL},
};


static void
update_u8(char op, uint8_t *val, uint8_t flags, uint8_t lower_bound, uint8_t upper_bound)
{
        switch (op) {
        case UP:
                if (*val <= upper_bound - 1)
                        (*val)++;
                break;
        case DOWN:
                if (*val >= lower_bound + 1)
                        (*val)--;
                break;
        }
}

static void
mode()
{
        unsigned char count = 0;
        struct param *p = param;

        config_init();

        do {
                paint(p->id, bin2bcd(*p->val), 0, 0);

                char op = pop_op();
                count = op == REFRESH ? count + 1 : 0;
                switch (op) {
                case MODE|LONG_PRESS:
                        goto out;
                case MODE:
                        p++;
                        if (p->val == NULL)
                                goto out;
                        break;
                case UP:
                case DOWN:
                        update_u8(op, p->val, p->flags, p->lower_bound, p->upper_bound);
                        config_apply();
                        break;
                }
        } while (count < 10);
out:
        config_write();
        config_print();
        push_op(REFRESH);
}

static char
attention_requested()
{
        switch (pop_op()) {
        case NOP:
        case REFRESH:
                return 0;
        default:
                return 1;
        }
}

static void
antipoison()
{
        if (config.antipoison_duration == 0)
                return;


        static char disabled_today;
        if (disabled_today == time.date)
                return;
        disabled_today = -1;

        while (1) {
                char hour = bcd2bin(time.hour),
                    start = config.antipoison_start,
                      end = config.antipoison_start + config.antipoison_duration;

                if (hour < start || hour >= end)
                        return;

                if (attention_requested() ) {
                        disabled_today = time.date;
                        return;
                }

                static char j;
                const char d[] = {1, 0, 2, 9, 8, 3, 4, 7, 6, 5};
                char x = (d[j] << 4) | d[j];
                paint(x, x, x, 0);
                j = j < 9 ? j + 1 : 0;
                _delay_ms(500);
         }
}

int
main()
{
        config_init();
	sei();
        uart_init(9600);
        i2c_init();
        board_init();

        printf("\n\nloading\n\n\n");

        config_apply();
        config_print();

        wdt_enable(WDTO_250MS);

	for (;;) {
                antipoison();

                char op = pop_op();
                switch (op & 0x7f) {
                case REFRESH:
                        refresh();
                        break;
                case UP:
                        time_up(&time);
                        refresh();
                        break;
                case DOWN:
                        time_down(&time);
                        refresh();
                        break;
                case MODE:
                        if ((op & LONG_PRESS) == 0)
                                mode();
                        break;
                }
	}
}
