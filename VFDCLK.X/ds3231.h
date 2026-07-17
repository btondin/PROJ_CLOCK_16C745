/**
 * =====================================================================
 *  ds3231.h — Driver do RTC Maxim DS3231 (via I2C por software)
 * =====================================================================
 *  O DS3231 guarda hora/data em BCD nos registradores 00h..06h.
 *  Este driver mantém os valores EM BCD de ponta a ponta: o display
 *  e o protocolo USB também trabalham em BCD, evitando conversões.
 *
 *  Convenção de dia da semana (registrador 03h, faixa livre 1..7 do
 *  DS3231): 1 = segunda ... 7 = domingo (o app do PC envia assim).
 * =====================================================================
 */
#ifndef DS3231_H
#define DS3231_H

#include <stdint.h>
#include <stdbool.h>

/* Hora/data como armazenadas no DS3231 (tudo BCD, exceto dia_semana) */
typedef struct {
    uint8_t segundos;    /* BCD 00..59                                */
    uint8_t minutos;     /* BCD 00..59                                */
    uint8_t horas;       /* BCD 00..23 (modo 24 h forçado no driver)  */
    uint8_t dia_semana;  /* binário 1..7 (1 = segunda ... 7 = domingo)*/
    uint8_t dia;         /* BCD 01..31                                */
    uint8_t mes;         /* BCD 01..12                                */
    uint8_t ano;         /* BCD 00..99 (século 20xx assumido)         */
} ds3231_hora_t;

/* Lê hora/data (07 registradores em rajada, leitura atômica garantida
 * pelo buffer interno do DS3231). Retorna false se o chip não deu ACK
 * (chip ausente / barramento com problema).                          */
bool ds3231_ler(ds3231_hora_t *hora);

/* Grava hora/data e limpa a flag OSF (a hora passa a ser "válida").
 * Também garante modo 24 h e oscilador habilitado.                   */
bool ds3231_gravar(const ds3231_hora_t *hora);

/* Retorna true se a hora do RTC é confiável (flag OSF do registrador
 * de status 0Fh em 0). OSF=1 indica que o oscilador parou em algum
 * momento (ex.: primeira energização, bateria esgotada) — a hora
 * exibida seria lixo até uma sincronização via USB.                  */
bool ds3231_hora_valida(void);

#endif /* DS3231_H */
