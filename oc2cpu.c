#include <stdint.h>
#include <stdio.h>

#include <avr/interrupt.h>
#include "avr/io.h"

#include "nixie.h"

/*
  Buttons:
    PD4, PD7

  Tube enable outputs:
    PD3, PD5, PD6

  Tube mux outputs:
    PC0, PC1, PC2, PC3
    PB0, PB1, PB2, PB3

*/

unsigned char
button_read()
{
        unsigned char mask = 0;
        if ((PIND & _BV(PD4)) == 0)
                mask |= UP;
        if ((PIND & _BV(PD7)) == 0)
                mask |= DOWN;
        return mask;
}

ISR(TIMER0_OVF_vect)
{
        button_scan();
}


ISR(TIMER1_OVF_vect)
{
        // Clear mux outputs
        PORTC &= 0xf0;
        PORTB &= 0xf0;

        // Turn off all tubes
        PORTD &= ~_BV(PD3);
        PORTD &= ~_BV(PD5);
        PORTD &= ~_BV(PD6);
}

static volatile char output[6] = { 0xf, 0xf, 0xf, 0xf, 0xf, 0xf };
static volatile char frame_sync;
ISR(TIMER1_COMPB_vect)
{
        static char ix;

        PORTC |= output[ix];
        PORTB |= output[ix + 3];

        switch (ix) {
        case 0:
                PORTD |= _BV(PD6); // [@_:_@:__]
                ix = 1;
                break;
        case 1:
                PORTD |= _BV(PD5); // [_@:__:@_]
                ix = 2;
                break;
        case 2:
                PORTD |= _BV(PD3); // [__:@_:_@]
                ix = 0;
                frame_sync = 0;
                break;
        }
}

void
wait_frame_sync()
{
        frame_sync = 1;
        while (frame_sync);
}

void
paint(char x, char y, char z, char q __attribute__((unused)))
{
        const char translate[16] = { 2, 8, 9, 0, 1, 5, 4, 6, 7, 3,
                                     0xf, 0xf, 0xf, 0xf, 0xf, 0xf};

        if ((x >> 4)  != 0xf) output[0] = translate[x >> 4];
        if ((x & 0xf) != 0xf) output[1] = translate[x & 0xf];
        if ((y >> 4)  != 0xf) output[2] = translate[y >> 4];
        if ((y & 0xf) != 0xf) output[3] = translate[y & 0xf];
        if ((z >> 4)  != 0xf) output[4] = translate[z >> 4];
        if ((z & 0xf) != 0xf) output[5] = translate[z & 0xf];
}

void
config_apply()
{
        // Set PWM freq & duty for tubes
        ICR1 =  (F_CPU / 64 / (config.tube_pwm_freq * 10)) - 1;
        // tube is enabled _after_ OC match, thus PWM is inverted
        OCR1B = (uint32_t)ICR1 * (100 - config.tube_pwm_duty) / 100;
}


static void
tube_init()
{
        TCCR1B |= _BV(CS11)|_BV(CS10); // clk_IO/64

        // Fast PWM, 16-bit, TOP=ICR1
        // WGM bits must be configured before configuring ICR1
        TCCR1A |= _BV(WGM11);
        TCCR1B |= _BV(WGM13)|_BV(WGM12);

        config_apply();

        // Set tube enable pins as outputs
        DDRD |= _BV(PD3)|_BV(PD5)|_BV(PD6);
        // Set mux pins as outputs
        DDRC |= _BV(PC0)|_BV(PC1)|_BV(PC2)|_BV(PC3);
        DDRB |= _BV(PB0)|_BV(PB1)|_BV(PB2)|_BV(PB3);

        // Enable tube clear & tube update interrupt
        TIMSK1 |= _BV(TOIE1)|_BV(OCIE1B);
}

static void
button_init()
{
        // Waveform Generation Mode
        // Fast PWM , TOP=OCRxA
        TCCR0A |= _BV(WGM01)|_BV(WGM00);
        TCCR0B |= _BV(CS02)|_BV(CS00); // clk_IO/1024
        OCR0A = 155; // 16MHz/((155 +1) * 1024) ~~ 100Hz

        // Enable button scan interrupt
        TIMSK0 |= _BV(TOIE0);

        // Pullups not needed: board has them already.
}

void
board_init()
{
        tube_init();
        button_init();
}
