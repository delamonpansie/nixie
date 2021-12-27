#ifndef UART_H
#define UART_H

#include <math.h>
#define uart_init(baud) uart_init_ubrr(fabs(F_CPU/(16 * (double)baud) - 1) + 0.5)
void uart_init_ubrr(unsigned int ubrr0);

char uart_read_would_block();

#endif
