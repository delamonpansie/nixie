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
        .antipoison_hour = 0xff,
};

static struct time {
        unsigned char dirty;
        unsigned char sec;
        unsigned char min;
        unsigned char hour : 6;
        unsigned char twentyfour : 1;
} time;

#define OP_RING_BITS 3
#define OP_RING_MASK (_BV(OP_RING_BITS)-1)
static volatile char op_ring[_BV(OP_RING_BITS)], opr;
static volatile char opw;

static char
pop_op()
{
        while (opr == opw);
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

        // ds3231_sync() takes about 0.2ms to execute with I2C set to 400kHz
        ds3231_sync();
}

static void
refresh() {
        if (config.antipoison_hour == 0x2) {
                const char d[] = {1, 0, 2, 9, 8, 3, 4, 7, 6, 5};
                static char j;
                paint((d[j] << 4) & 0xf, (d[j] << 4) & 0xf, (d[j] << 4) & 0xf, 0);
                j = j < 9 ? j + 1 : 0;
                return;
        }

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
config_print(struct config *cfg)
{
        printf("crc: %d\n"
               "tube_pwm_freq: %d\n"
               "tube_pwm_duty: %d\n"
               "led_red_brightness: %d\n"
               "led_green_brightness: %d\n"
               "led_blue_brightness: %d\n"
               "antipoison_hour: %d\n",
               cfg->crc,
               cfg->tube_pwm_freq,
               cfg->tube_pwm_duty,
               cfg->led_red_brightness,
               cfg->led_green_brightness,
               cfg->led_blue_brightness,
               cfg->antipoison_hour);
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


static void
update_u8(char op, uint8_t *val, uint8_t step, uint8_t lower_bound, uint8_t upper_bound)
{
        switch (op) {
        case UP:
                if (*val <= upper_bound - step)
                        *val += step;
                else
                        *val = lower_bound;
                break;
        case DOWN:
                if (*val >= lower_bound + step)
                        *val -= step;
                else
                        *val = upper_bound;
                break;
        }
}

struct param {
        unsigned char *val;
        unsigned char step;
        unsigned char lower_bound;
        unsigned char upper_bound;
} param[] = {
        {&config.led_red_brightness,	1,	0,	10},
        {&config.led_green_brightness,	1,	0,	10},
        {&config.led_blue_brightness,	1,	0,	10},
        {&config.tube_pwm_freq,		1,	10,	60},
        {&config.tube_pwm_duty,		1,	10,	99},
};

static uint8_t
dec2bcd(uint8_t x)
{
        if ((x & 0xf) > 9)
                x += 6;
        return x;
}

static void
mode()
{
        unsigned char mode = 0, count = 0;
        struct param *p = &param[mode];

        config_init();

        do {
                paint(mode, dec2bcd(*p->val), 0, 0);

                char op = pop_op();
                switch (op) {
                case MODE|LONG_PRESS:
                        count = 0xff;
                        break;
                case MODE:
                        count = 0;
                        mode++;
                        if (mode > 5)
                                mode = 0;
                        p = &param[mode];
                        break;
                case UP:
                case DOWN:
                        count = 0;
                        update_u8(op, p->val, p->step, p->lower_bound, p->upper_bound);
                        config_apply();
                        break;
                }
        } while (count < 16);
        config_write();
        push_op(REFRESH);
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
        config_print(&config);

        wdt_enable(WDTO_250MS);

	for (;;) {
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
