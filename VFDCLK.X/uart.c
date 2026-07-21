/**
 * =====================================================================
 *  uart.c — USART do PIC16C745 em modo assíncrono, somente transmissão
 * =====================================================================
 *  Referência: datasheet PIC16C745/765 (DS41124D), seção 9 (USART).
 *
 *  Cálculo do baud rate (modo assíncrono, alta velocidade BRGH=1):
 *      Baud = FOSC / (16 * (SPBRG + 1))
 *      SPBRG = 24.000.000 / (16 * 19200) - 1 = 77,125  ->  77
 *      Baud real = 24 MHz / (16 * 78) = 19231 (erro de +0,16%, ok)
 *
 *  19200 é o baud MÁXIMO do display (spec 3.2.2.1) e foi escolhido
 *  para cortar pela metade o tempo de qualquer escrita na tela: a
 *  521 us por caractere, repintar uma tela de 2x20 leva ~23 ms em vez
 *  dos ~46 ms de 9600 — o que reduz a piscada visível ao redesenhar.
 *  O jumper de personalidade do display precisa estar em 19200.
 *
 *  O sinal TX (RC6) idle em nível ALTO (mark UART padrão). O display
 *  VFD espera níveis e polaridade EIA-232 (repouso = mark = tensão
 *  negativa), por isso o hardware usa um MAX232 entre RC6 (T1IN) e a
 *  entrada serial J1-14 do display (T1OUT) — ver HARDWARE/pinagem.md.
 *  Para o firmware isso é transparente: transmite-se normalmente.
 * =====================================================================
 */
#include <xc.h>
#include "board.h"
#include "uart.h"

void uart_iniciar(void)
{
    SPBRG = 77;             /* 19200 baud @ 24 MHz (ver cálculo acima) */

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
