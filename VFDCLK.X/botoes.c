/**
 * =====================================================================
 *  botoes.c — Debounce e detecção de toque curto/longo
 * =====================================================================
 *  O laço principal já roda a cada ~50 ms, então o debounce sai de
 *  graça: basta contar quantos ciclos consecutivos o botão ficou
 *  pressionado. Não é preciso timer nem interrupção.
 *
 *  Máquina de estados (por botão):
 *
 *    solto ──pressiona──> contando ──2 ciclos (100 ms)──> válido
 *                             │                              │
 *                             │                        40 ciclos (2 s)
 *                        solta antes                         │
 *                             │                              v
 *                             v                         evento LONGO
 *                       evento CURTO                  (dispara ainda
 *                     (dispara ao soltar)              com o botão preso)
 *
 *  O evento LONGO dispara no instante em que o tempo é atingido — dá
 *  retorno imediato ao usuário, sem precisar soltar o botão. Como o
 *  contador satura em LONGO_CICLOS, ele não se repete, e ao soltar
 *  não gera um CURTO indevido.
 * =====================================================================
 */
#include <xc.h>
#include "board.h"
#include "botoes.h"

/* Tempos em múltiplos do período do laço principal (~50 ms)          */
#define DEBOUNCE_CICLOS   2u    /* 100 ms — filtra o repique dos contatos */
#define LONGO_CICLOS     40u    /* 2,0 s  — limiar do toque longo         */

static uint8_t      contador[BTN_QUANTOS];  /* ciclos pressionado     */
static btn_evento_t pendente[BTN_QUANTOS];  /* evento aguardando      */

void botoes_processar(void)
{
    uint8_t i;
    uint8_t pressionado;
    uint8_t ciclos;

    for (i = 0; i < BTN_QUANTOS; i++) {
        /* Nível 0 = pressionado (botão para GND, pull-up externo)    */
        pressionado = (i == BTN_TELA) ? (uint8_t)(BTN_TELA_LER() == 0)
                                      : (uint8_t)(BTN_ALARME_LER() == 0);

        if (pressionado) {
            if (contador[i] < LONGO_CICLOS) {
                contador[i]++;
                if (contador[i] == LONGO_CICLOS) {
                    pendente[i] = BTN_LONGO;   /* dispara já, preso   */
                }
            }
        } else {
            ciclos = contador[i];
            contador[i] = 0;
            /* Só é toque curto se passou do debounce E não chegou a
             * virar toque longo (senão o LONGO já foi entregue).     */
            if ((ciclos >= DEBOUNCE_CICLOS) && (ciclos < LONGO_CICLOS)) {
                pendente[i] = BTN_CURTO;
            }
        }
    }
}

btn_evento_t botao_evento(uint8_t qual)
{
    btn_evento_t evento = pendente[qual];

    pendente[qual] = BTN_NADA;      /* consome: só é entregue 1 vez   */
    return evento;
}
