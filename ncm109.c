#include <stdint.h>
#include <stdio.h>

#include <avr/interrupt.h>
#include "avr/io.h"

#include "nixie.h"


/*
  Buttons
    PC0, PC1, PC2

  LED outputs
     Red: PD5, PWM OC0B
     Green: PD6, PWM OC0A
     Blue: PD3, PWM OC2B

     All PWMs are configured to 1kHz

     green PWM also configured to trigger TIMER0_COMPA interrupt.
     It is used for periodic button scanning & ds3231 refresh

   Tube
      mux connected via SPI
        MOSI: PB3
        SCK: PB5

      LE(tube enable): PB2, PWM OC1B
        PWM freq & duty are dynamically configured via config

        PWM configured to trigger TIMER1_COMPB_vect interrupt at the begging of OFF cycle
        TIMER1_COMPB_vect writes framebufer to tube mux.
        Because tubes are off at the moment, writing would not cause flicker.
 */

unsigned char
button_read()
{
        char mask = 0;
        if ((PINC & _BV(PC0)) == 0)
                mask |= MODE;
        if ((PINC & _BV(PC1)) == 0)
                mask |= UP;
        if ((PINC & _BV(PC2)) == 0)
                mask |= DOWN;
        return mask;
}

static void
led_brightness(char r, char g, char b)
{
        // Set PWM duty
        OCR0B = r;
        OCR0A = g;
        OCR2B = b;

        // Enable/disable PWM output pin
        //  Configure "Compare Output Mode" to non-inverting mode:
        //  Clear OCxA/OCxB output pin on compare match, set OCxA/OCxB output pin at BOTTOM
        if (r) TCCR0A |= _BV(COM0B1);
        else TCCR0A &= ~_BV(COM0B1);

        if (g) TCCR0A |= _BV(COM0A1);
        else TCCR0A &= ~_BV(COM0A1);

        if (b) TCCR2A |= _BV(COM2B1);
        else TCCR2A &= ~_BV(COM2B1);
}

static void
tube_pwm_config()
{
        // Set PWM freq & duty for tubes
        ICR1 =  (F_CPU / 64 / (config.tube_pwm_freq * 10)) - 1;
        OCR1B = (uint32_t)ICR1 * config.tube_pwm_duty / 100;
}

void
config_apply()
{
        tube_pwm_config();
        led_brightness(config.led_red_brightness * 25,
                       config.led_green_brightness * 25,
                       config.led_blue_brightness * 25);
}

ISR(TIMER0_OVF_vect)
{
        static char tick;
        if (tick++ == 9) { // 976Hz/10 ~ 97Hz, button scan roughly 100 times per sec
                tick = 0;
                button_scan();
        }
}


static char * volatile framebuf;
// TIMER1_COMPB interrupt will be executed right after clearing OC1B, tubes will be off at this moment.
ISR(TIMER1_COMPB_vect)
{
        if (framebuf == NULL)
                return;

        // datasheet table 3-1 suggests to disable LE when HV5122
        // LE is connected to PB2 and will be low if PWM is enabled, becase COMPB executed after clearing OC1B (PB2)
        // however, turn down PB2 anyway, in case of PWM is not running
       PORTB &= ~_BV(PB2);

       // 8 bytes at 4MHz is ~ 22us
        for (signed char i = 7; i >= 0; i--) {
                SPDR = framebuf[i];
                loop_until_bit_is_set(SPSR, SPIF);
        }

        PORTB |= _BV(PB2);
        framebuf = NULL;
}

void
paint(char x, char y, char z, char q)
{
        static uint32_t buf[2];
        buf[0] = (uint32_t)!!q << 31 | (uint32_t)!!q << 30;
        buf[1] = (uint32_t)!!q << 31 | (uint32_t)!!q << 30;
        buf[0] |= 1UL << (x >> 4);
        buf[0] |= 1UL << (10 + (x & 0xf));
        buf[0] |= 1UL << (20 + (y >> 4));
        buf[1] |= 1UL << (y & 0xf);
        buf[1] |= 1UL << (10 + (z >> 4));
        buf[1] |= 1UL << (20 + (z & 0xf));

        // wait for previous framebuf write cycle to complete
        while (framebuf != NULL);
        // memory barrier: writes to buf[] should happen before assigment to framebuf
        __sync_synchronize();
        framebuf = (char *)buf;
}

static void
led_init()
{
        // Waveform Generation Mode
        // Fast PWM , TOP=0xFF
        TCCR0A |= _BV(WGM01)|_BV(WGM00); // TIMER0_OVF used as button scan interrupt
        TCCR2A |= _BV(WGM21)|_BV(WGM20);

        TCCR0B |= _BV(CS01)|_BV(CS00); // clk_IO/64 ~ 976Hz
        TCCR2B |= _BV(CS22);           // clk_IO/64 ~ 976Hz

        // Set LED PWM pins as outputs
        DDRD |= _BV(PD3)|_BV(PD5)|_BV(PD6);
}

static void
tube_init()
{
        // PWM
        TCCR1B |= _BV(CS11)|_BV(CS10); // clk_IO/64

        // Fast PWM, 16-bit, TOP=ICR1
        // WGM bits must be configured before configuring ICR1
        TCCR1A |= _BV(WGM11);
        TCCR1B |= _BV(WGM13)|_BV(WGM12);

        tube_pwm_config();

        // Configure LE as OUTPUT
        // LE is also SPI ^SS, so it must be configured as output all the time
        DDRB |= _BV(PB2);

        // Enable LE (tube enable) PWM output
        //  Configure "Compare Output Mode" to non-inverting mode:
        //  Clear OC1B output pin on compare match, set OC1B output pin at BOTTOM
        TCCR1A |= _BV(COM1B1);

        // Configure SPI pins. Set MOSI and SCK as output.
        DDRB |= _BV(PB3)|_BV(PB5);
        // Enable SPI, Master, set clock rate fck/4 = 4MHz, MODE=2, MSB transmitted first
        // HV5122 supports clocks up to 8MHz
        SPCR = _BV(SPE)|_BV(MSTR)|_BV(CPOL);

        // Clear tubes
        for (char i = 0; i < 8; i++) {
                SPDR = 0;
                loop_until_bit_is_set(SPSR, SPIF);
        }

        // Enable tube update interrupt
        TIMSK1 |= _BV(OCIE1B);
}

static void
button_init()
{
        // Enable button scan interrupt
        TIMSK0 |= _BV(TOIE0);

        // Enable pullups
        PORTC |= _BV(PC0)|_BV(PC1)|_BV(PC2);
}

void
board_init()
{
        led_init();
        tube_init();
        button_init();
}
