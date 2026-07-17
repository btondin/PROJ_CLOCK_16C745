/**
 * =====================================================================
 *  uart.h — USART do PIC16C745 em modo assíncrono, somente transmissão
 * =====================================================================
 *  Usado exclusivamente para alimentar a entrada serial do display VFD
 *  (IEE 036X2, 9600 baud, 8N1). Não há recepção neste projeto.
 * =====================================================================
 */
#ifndef UART_H
#define UART_H

#include <xc.h>
#include <stdint.h>

/* Configura o USART para 9600 baud, 8 bits, sem paridade, 1 stop. */
void uart_iniciar(void);

/* Transmite um byte, bloqueando até o registrador TXREG liberar.
 * A 9600 baud cada byte leva ~1,04 ms na linha.                     */
void uart_enviar(uint8_t byte);

/* Versão em MACRO da transmissão, para uso dentro do driver do VFD:
 * não consome nível da pilha de hardware (o PIC16 tem só 8 níveis,
 * compartilhados com a interrupção do USB — ver nota em usb_hid.c). */
#define UART_TX(b)                                   \
    do {                                             \
        while (!PIR1bits.TXIF) { ; }                 \
        TXREG = (uint8_t)(b);                        \
    } while (0)

#endif /* UART_H */
