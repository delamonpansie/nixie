#include <util/delay.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdlib.h>
#include <stdio.h>

#include "uart.h"
typedef unsigned char u8;
typedef unsigned int u16;


#ifndef RXD_PORT
#define RXD_DDR   DDRD
#define RXD_PORT  PORTD
#define RXD_PIN   PIND
#define RXD_BIT   0
#endif

#ifndef TX_RING_BITS
#define TX_RING_BITS 7
#endif
#define RING_MASK (_BV(TX_RING_BITS) - 1)

static u8 tx_ring[_BV(TX_RING_BITS)], tx_end,
	  rx_ring[_BV(TX_RING_BITS)], rx_start;
static volatile u8 tx_start, rx_end;

ISR(USART_UDRE_vect)
{
	u8 s = tx_start;
	UDR0 = tx_ring[s];
	s = (s + 1) & RING_MASK;
	if (s == tx_end)
		UCSR0B &= ~_BV(UDRIE0);
	tx_start = s;
}


int
uart_putchar(char c, FILE *stream __attribute__((unused)))
{
#ifdef UART_CONVERT_NL
	if (c == '\n')
		uart_putchar('\r', NULL);
#endif
 	while (((tx_end + 1) & RING_MASK) == tx_start);

	tx_ring[tx_end] = c;
	tx_end = (tx_end + 1) & RING_MASK;
	UCSR0B |= _BV(UDRIE0);
	return 0;
}

ISR(USART_RX_vect)
{
	char c = UDR0;
	u8 e = rx_end;
	if (((e + 1) & RING_MASK) == rx_start)
		return;
#if UART_ECHO
	uart_putchar(c, NULL);
#endif
	rx_ring[e] = c;
	rx_end = (e + 1) & RING_MASK;
}

char
uart_read_would_block()
{
	return rx_start == rx_end;
}

static int
uart_getchar(FILE *stream __attribute__((unused)))
{
#ifdef UART_READ_NONBLOCK
	if (rx_start == rx_end)
		return _FDEV_EOF;
#else
	while (rx_start == rx_end);
#endif
	u8 s = rx_ring[rx_start];
	rx_start = (rx_start + 1) & RING_MASK;
	return s;
}

void
_uart_init(u16 baud)
{
	UBRR0H = baud >> 8;
	UBRR0L = baud;

	RXD_PORT |= _BV(RXD_BIT); /* Enable pullup on RX line */
	UCSR0B = _BV(RXEN0)|_BV(TXEN0)|_BV(RXCIE0);
	fdevopen(uart_putchar, uart_getchar);
}

