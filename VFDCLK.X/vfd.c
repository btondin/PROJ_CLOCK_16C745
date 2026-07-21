/**
 * =====================================================================
 *  vfd.c — Driver do display VFD IEE Century Series 036X2 (20x2)
 * =====================================================================
 *  Referência: "Specification, VFD, Century Series, Dot Matrix
 *  Modules" (IEE S036X2, rev. N):
 *    - seção 3.2.2.1: interface serial (19200 8N1)
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
 *  Por que não há esperas por BUSY: em serial cada byte ocupa 521 us
 *  de linha (19200 baud) e os comandos usados em regime ("Send
 *  Character" e "Cursor Locate") levam ~170 us — o próprio baud rate
 *  serve de flow control. Os comandos lentos (Clear, Reset) só ocorrem
 *  em vfd_iniciar(), que aguarda explicitamente.
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
#define VFD_CMD_BLINK_INICIO    0x31u   /* + código de taxa (2 bytes)  */
#define VFD_CMD_BLINK_FIM       0x32u

void vfd_iniciar(void)
{
    /* O spec (3.3) exige >= 500 ms após a alimentação antes do
     * primeiro comando ("Processor Power-up Cycle").                 */
    __delay_ms(500);

    /* Reset por software: tela limpa, cursor no início, modos padrão
     * (rolagem vertical, charset europeu, brilho máximo).
     * A tabela 3-5 diz que o reset executa em ~0,8 ms, mas damos
     * folga larga: é uma operação única no boot, e um reset ainda em
     * curso engoliria os comandos seguintes.                         */
    UART_TX(VFD_CMD_RESET_SW);
    __delay_ms(100);

    /* Limpa e leva o cursor ao início (estado conhecido)             */
    UART_TX(VFD_CMD_LIMPAR_HOME);
    __delay_ms(10);

    /* Cursor invisível. ATENÇÃO: o padrão de fábrica é cursor LIGADO
     * piscando a 4 Hz (spec 4.2, código 0Fh) — sem este comando fica
     * um caractere piscando na tela, parecendo um editor de texto.   */
    UART_TX(VFD_CMD_CURSOR_OFF);
    __delay_ms(10);
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

void vfd_escrever_em(uint8_t linha, uint8_t coluna, const char *texto)
{
    uint8_t c;

    /* Escreve só o trecho pedido, SEM completar a linha com espaços:
     * usado para atualizar campos que mudam (ex.: os dígitos da hora)
     * sem repintar tudo — repintar as 2 linhas inteiras a cada segundo
     * são ~44 bytes = ~46 ms de escrita contínua, e isso aparece como
     * uma piscada na tela.                                           */
    UART_TX(VFD_CMD_POSICIONAR);
    UART_TX((uint8_t)(linha * VFD_COLUNAS + coluna));

    while (*texto != '\0') {
        c = (uint8_t)*texto;
        if (c < 0x20u) {
            c = ' ';
        }
        UART_TX(c);
        texto++;
    }
}

void vfd_campo_bcd(uint8_t linha, uint8_t coluna, uint8_t novo, uint8_t antigo)
{
    uint8_t base = (uint8_t)(linha * VFD_COLUNAS + coluna);

    /* Reescreve SÓ os dígitos que realmente mudaram. Num relógio, de
     * um segundo para o outro quase sempre muda apenas a unidade dos
     * segundos: 1 caractere em vez de 8. Menos escrita = menos tempo
     * com a tela sendo alterada = sem piscada perceptível.
     *
     * Função-FOLHA de propósito (só usa a macro UART_TX, não chama
     * ninguém): economiza um nível da pilha de hardware de 8 níveis,
     * que o main divide com a ISR do USB.                            */
    if (novo == antigo) {
        return;                      /* nada mudou: nem posiciona     */
    }

    if ((novo >> 4) != (antigo >> 4)) {
        /* A dezena mudou: manda os dois dígitos de uma vez (4 bytes,
         * mais barato que dois posicionamentos separados).           */
        UART_TX(VFD_CMD_POSICIONAR);
        UART_TX(base);
        UART_TX((uint8_t)('0' + (novo >> 4)));
        UART_TX((uint8_t)('0' + (novo & 0x0Fu)));
    } else {
        /* Só a unidade mudou: 1 caractere (3 bytes no total).        */
        UART_TX(VFD_CMD_POSICIONAR);
        UART_TX((uint8_t)(base + 1u));
        UART_TX((uint8_t)('0' + (novo & 0x0Fu)));
    }
}

void vfd_piscar_inicio(uint8_t taxa)
{
    /* Seção 4.5: 31h marca o INÍCIO do trecho piscante — os caracteres
     * escritos DEPOIS deste comando é que piscam, até virem um 32h ou
     * serem sobrescritos por texto normal.                           */
    UART_TX(VFD_CMD_PREFIXO_A0);
    UART_TX(VFD_CMD_BLINK_INICIO);
    UART_TX(taxa);
}

void vfd_piscar_fim(void)
{
    UART_TX(VFD_CMD_PREFIXO_A0);
    UART_TX(VFD_CMD_BLINK_FIM);
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
