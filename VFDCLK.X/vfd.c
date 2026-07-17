/**
 * =====================================================================
 *  vfd.c — Driver do display VFD IEE Century Series 036X2 (20x2)
 * =====================================================================
 *  Referência: "Specification, VFD, Century Series, Dot Matrix
 *  Modules" (IEE S036X2, rev. N):
 *    - seção 3.2.2.1: interface serial (9600 8N1)
 *    - seção 3.3    : tempos de execução (tabela 3-5) e power-up 500 ms
 *    - seção 4      : códigos de programação usados aqui:
 *        14h  reset por software (limpa tela, modos e atributos)
 *        15h  limpa a tela e leva o cursor ao início ("home")
 *        0Eh  cursor invisível
 *        1Bh  posiciona cursor (2 bytes: 1Bh + ID da posição 0..39)
 *        19h  "Set Address Bit 0 High": em modo serial, equivale a
 *             levantar a linha A0 SOMENTE para o próximo byte — é o
 *             mecanismo para alcançar os códigos 30h..3Fh (seção 4.6)
 *        30h  brilho (3 bytes: 30h, coluna [FFh = todas], nível 0..7)
 *
 *  Por que não há esperas por BUSY: em serial cada byte ocupa ~1,04 ms
 *  de linha (9600 baud) e o comando mais lento do modelo 20x2 leva
 *  <0,9 ms — o próprio baud rate serve de flow control.
 *
 *  A transmissão usa a macro UART_TX (e não a função uart_enviar)
 *  para não gastar níveis da pilha de hardware de 8 posições — as
 *  funções deste driver são chamadas pelo main já no 3º nível.
 * =====================================================================
 */
#include <xc.h>
#include "board.h"
#include "uart.h"
#include "vfd.h"

/* Códigos de controle do IEE 036X2 (A0/RS baixo, seção 4.1)          */
#define VFD_CMD_CURSOR_OFF      0x0Eu
#define VFD_CMD_RESET_SW        0x14u
#define VFD_CMD_LIMPAR_HOME     0x15u
#define VFD_CMD_POSICIONAR      0x1Bu
#define VFD_CMD_PREFIXO_A0      0x19u   /* A0=1 só para o próximo byte */

/* Códigos de controle com A0 alto (seção 4.5, exigem prefixo 19h)    */
#define VFD_CMD_BRILHO          0x30u
#define VFD_BRILHO_TODAS_COLS   0xFFu

void vfd_iniciar(void)
{
    /* O spec (3.3) exige >= 500 ms após a alimentação antes do
     * primeiro comando ("Processor Power-up Cycle").                 */
    __delay_ms(500);

    /* Reset por software: tela limpa, cursor no início, modos padrão
     * (rolagem vertical, charset europeu, brilho máximo).            */
    UART_TX(VFD_CMD_RESET_SW);
    __delay_ms(2);                  /* reset executa em <= 0,9 ms     */

    UART_TX(VFD_CMD_CURSOR_OFF);    /* relógio não precisa de cursor  */
}

void vfd_limpar(void)
{
    UART_TX(VFD_CMD_LIMPAR_HOME);
}

void vfd_cursor(uint8_t linha, uint8_t coluna)
{
    /* Posições numeradas 0..39: linha 0 = 0..19, linha 1 = 20..39.   */
    UART_TX(VFD_CMD_POSICIONAR);
    UART_TX((uint8_t)(linha * VFD_COLUNAS + coluna));
}

void vfd_escrever_char(char c)
{
    /* Caracteres < 20h são códigos de controle: filtra por segurança
     * para nunca disparar um comando sem querer no meio de um texto. */
    if ((uint8_t)c < 0x20u) {
        c = ' ';
    }
    UART_TX((uint8_t)c);
}

void vfd_escrever_texto(const char *texto)
{
    while (*texto != '\0') {
        vfd_escrever_char(*texto);
        texto++;
    }
}

void vfd_linha(uint8_t linha, const char *texto)
{
    uint8_t coluna = 0;
    uint8_t c;

    /* Posicionamento inline (função-folha: não chama nada)           */
    UART_TX(VFD_CMD_POSICIONAR);
    UART_TX((uint8_t)(linha * VFD_COLUNAS));

    while ((*texto != '\0') && (coluna < VFD_COLUNAS)) {
        c = (uint8_t)*texto;
        if (c < 0x20u) {            /* nunca vazar código de controle */
            c = ' ';
        }
        UART_TX(c);
        texto++;
        coluna++;
    }
    /* Completa com espaços: apaga restos da escrita anterior sem
     * precisar de "clear" (que faria a tela piscar).                 */
    while (coluna < VFD_COLUNAS) {
        UART_TX(' ');
        coluna++;
    }
}

void vfd_brilho(uint8_t nivel)
{
    if (nivel > VFD_BRILHO_MINIMO) {
        nivel = VFD_BRILHO_MINIMO;
    }
    /* Em serial, o código 30h (A0 alto) é alcançado com o prefixo
     * 19h (seção 4.6). Os parâmetros seguem como bytes normais.      */
    UART_TX(VFD_CMD_PREFIXO_A0);
    UART_TX(VFD_CMD_BRILHO);
    UART_TX(VFD_BRILHO_TODAS_COLS);       /* FFh = tela inteira       */
    UART_TX(nivel);                       /* 0 = máximo .. 7 = mínimo */
}
