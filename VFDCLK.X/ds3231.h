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

/* ------------------------------------------------------------------
 * ALARME DIÁRIO (usa o "Alarme 1" do DS3231)
 * ------------------------------------------------------------------
 * Por que o alarme mora no RTC e não no PIC: o PIC16C745 é OTP e NÃO
 * tem EEPROM de dados ("EEPROM space: None available"), ou seja, não
 * há onde guardar configuração que sobreviva ao desligamento. Já os
 * registradores de alarme do DS3231 (07h..0Ah) e o bit de habilitação
 * A1IE (controle 0Eh) são mantidos pela bateria — então o alarme
 * persiste sozinho, sem nenhum código de armazenamento.
 * ------------------------------------------------------------------ */

/* Programa o alarme para disparar TODO DIA em horas:minutos (BCD).
 * Não altera a habilitação — use ds3231_alarme_habilitar().          */
bool ds3231_alarme_gravar(uint8_t horas_bcd, uint8_t minutos_bcd);

/* Lê o horário programado e se o alarme está habilitado.             */
bool ds3231_alarme_ler(uint8_t *horas_bcd, uint8_t *minutos_bcd,
                       bool *habilitado);

/* Liga/desliga o alarme (bit A1IE do registrador de controle).       */
bool ds3231_alarme_habilitar(bool ligar);

/* True se o alarme disparou (flag A1F do status). O flag é LATCHED:
 * fica em 1 até ser reconhecido, então é impossível perder um disparo
 * por polling — não é preciso fiar o pino INT/SQW.                   */
bool ds3231_alarme_disparou(void);

/* Reconhece o disparo limpando A1F (o alarme rearma sozinho para o
 * dia seguinte).                                                     */
bool ds3231_alarme_reconhecer(void);

#endif /* DS3231_H */
