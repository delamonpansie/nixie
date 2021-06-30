#ifndef UART_H
#define UART_H

#define uart_init(baud) _uart_init(F_CPU/(16 * ((long)baud)) - 1)
void _uart_init(unsigned int baud);

char uart_read_would_block();

#endif
