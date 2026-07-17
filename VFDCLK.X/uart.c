/**
 * =====================================================================
 *  uart.c — USART do PIC16C745 em modo assíncrono, somente transmissão
 * =====================================================================
 *  Referência: datasheet PIC16C745/765 (DS41124D), seção 9 (USART).
 *
 *  Cálculo do baud rate (modo assíncrono, alta velocidade BRGH=1):
 *      Baud = FOSC / (16 * (SPBRG + 1))
 *      SPBRG = 24.000.000 / (16 * 9600) - 1 = 155,25  ->  155
 *      Baud real = 24 MHz / (16 * 156) = 9615 (erro de +0,16%, ok)
 *
 *  O sinal TX (RC6) idle em nível ALTO (mark UART padrão). O display
 *  VFD espera polaridade EIA-232 (repouso = mark = tensão baixa), por
 *  isso o hardware usa um inversor de 1 transistor NPN entre RC6 e a
 *  entrada serial J1-14 do display — ver HARDWARE/pinagem.md.
 *  Para o firmware isso é transparente: transmite-se normalmente.
 * =====================================================================
 */
#include <xc.h>
#include "board.h"
#include "uart.h"

void uart_iniciar(void)
{
    SPBRG = 155;            /* 9600 baud @ 24 MHz (ver cálculo acima) */

    /* TXSTA: CSRC=0, TX9=0 (8 bits), TXEN=1 (habilita transmissor),
     *        SYNC=0 (assíncrono), BRGH=1 (alta velocidade)           */
    TXSTA = 0x24;

    /* RCSTA: SPEN=1 entrega os pinos RC6/RC7 ao USART. A recepção
     *        (CREN) fica desligada — não é usada neste projeto.      */
    RCSTA = 0x80;
}

void uart_enviar(uint8_t byte)
{
    /* TXIF=1 indica TXREG vazio (pronto para receber novo byte).
     * Espera ocupada simples: a ISR de USB pode rodar livremente
     * durante a espera sem afetar a transmissão em curso, pois o
     * shift register do USART é independente do CPU.                 */
    while (!PIR1bits.TXIF) {
        ;
    }
    TXREG = byte;
}
