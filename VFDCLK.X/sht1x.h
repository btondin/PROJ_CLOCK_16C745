/**
 * =====================================================================
 *  sht1x.h — Driver do sensor Sensirion SHT15 (temperatura + umidade)
 * =====================================================================
 *  O SHT1x usa um barramento serial de 2 fios PROPRIETÁRIO da
 *  Sensirion ("Sensibus") — parecido com I2C, mas incompatível
 *  (sequência de start própria, sem endereço de 7 bits).
 *
 *  Os valores são entregues em unidades inteiras "decimais":
 *    temperatura em décimos de °C  (ex.: 234 = 23,4 °C; -15 = -1,5 °C)
 *    umidade      em décimos de %RH (ex.: 452 = 45,2 %RH)
 *  Toda a conversão é feita em PONTO FIXO (sem float) — ver sht1x.c.
 * =====================================================================
 */
#ifndef SHT1X_H
#define SHT1X_H

#include <stdint.h>
#include <stdbool.h>

/* Resultado de uma medição completa (T + RH compensada)              */
typedef struct {
    int16_t  temperatura_dC;   /* décimos de °C  (-401 .. +1237)      */
    uint16_t umidade_dRH;      /* décimos de %RH (1 .. 1000)          */
} sht1x_medida_t;

/* Reinicializa a comunicação com o sensor (sequência de "connection
 * reset" do datasheet: DATA solto + >= 9 clocks + transmission start).
 * Chamar uma vez na inicialização e após qualquer erro.              */
void sht1x_reset_comunicacao(void);

/* Executa uma medição completa: temperatura (14 bits, ~320 ms máx) e
 * umidade (12 bits, ~80 ms máx), com verificação de CRC-8 em ambas.
 * BLOQUEANTE (~0,4 s no pior caso) — a ISR de USB continua rodando.
 * Retorna false em caso de falta de ACK, timeout ou CRC inválido
 * (nesse caso os campos de saída não são alterados).                 */
bool sht1x_medir(sht1x_medida_t *medida);

#endif /* SHT1X_H */
