/**
 * =====================================================================
 *  botoes.h — Leitura dos botões com debounce e toque curto/longo
 * =====================================================================
 *  Dois botões ligados entre o pino e o GND, com pull-up externo de
 *  10 kΩ (portanto nível 0 = pressionado):
 *
 *    RA0 (pino 2) — BOTÃO 1: NAVEGAR — abre o menu de configuração e
 *                            percorre as opções (alarme, brilho)
 *    RA1 (pino 3) — BOTÃO 2: ALTERAR o valor da opção mostrada
 *                            (com o alarme TOCANDO, qualquer botão cala)
 *
 *  O menu usa qualquer toque (curto ou longo) como um "clique"; a
 *  distinção curto/longo continua disponível na API para uso futuro.
 *
 *  Os botões ficam no PORTA de propósito: o PORTB é reservado aos dois
 *  barramentos bit-bang (I2C e Sensibus), e mantê-lo livre de outras
 *  escritas evita qualquer interferência de read-modify-write.
 *
 *  USO: chamar botoes_processar() UMA vez por volta do laço principal
 *  (~50 ms) e depois consumir os eventos com botao_evento().
 * =====================================================================
 */
#ifndef BOTOES_H
#define BOTOES_H

#include <stdint.h>

/* Evento devolvido por botao_evento()                                */
typedef enum {
    BTN_NADA = 0,   /* nada aconteceu desde a última consulta         */
    BTN_CURTO,      /* pressionado e solto antes do tempo longo       */
    BTN_LONGO       /* mantido pressionado por ~2 s                   */
} btn_evento_t;

/* Índice dos botões (usado em botao_evento)                          */
#define BTN_TELA     0u
#define BTN_ALARME   1u
#define BTN_QUANTOS  2u

/* Amostra os botões e atualiza a máquina de estados do debounce.
 * Chamar a cada ~50 ms, no laço principal.                           */
void botoes_processar(void);

/* Consome (e limpa) o evento pendente do botão indicado.
 * Devolve BTN_NADA se não houver evento novo.                        */
btn_evento_t botao_evento(uint8_t qual);

#endif /* BOTOES_H */
