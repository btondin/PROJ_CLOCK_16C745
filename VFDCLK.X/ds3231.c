/**
 * =====================================================================
 *  ds3231.c — Driver do RTC Maxim DS3231 (via I2C por software)
 * =====================================================================
 *  Referência: datasheet DS3231 (Maxim/Analog Devices, 19-5170):
 *    - mapa de registradores (tabela "Timekeeping Registers"):
 *        00h seg (BCD) | 01h min (BCD) | 02h hora (BCD; bit6=0 -> 24h)
 *        03h dia da semana (1..7, definição livre do usuário)
 *        04h dia (BCD) | 05h mês (BCD; bit7 = século) | 06h ano (BCD)
 *    - 0Eh controle: EOSC(7) BBSQW(6) CONV(5) RS2 RS1 INTCN A2IE A1IE
 *        EOSC=0 mantém o oscilador rodando também na bateria.
 *    - 0Fh status : OSF(7) ... — OSF=1 indica que o oscilador parou
 *        (hora não confiável); só pode ser escrito para 0.
 *
 *  Protocolo I2C (7 bits, endereço fixo 1101000b = 0x68):
 *    escrita: START, 0xD0, ponteiro, dados..., STOP
 *    leitura: START, 0xD0, ponteiro, RESTART, 0xD1, dados..., STOP
 * =====================================================================
 */
#include <xc.h>
#include "board.h"
#include "swi2c.h"
#include "ds3231.h"

#define DS3231_END_ESCRITA  0xD0u    /* 0x68 << 1 | 0 (write)         */
#define DS3231_END_LEITURA  0xD1u    /* 0x68 << 1 | 1 (read)          */

#define DS3231_REG_SEGUNDOS 0x00u
#define DS3231_REG_ALARME1  0x07u    /* 07h..0Ah: seg/min/hora/dia    */
#define DS3231_REG_CONTROLE 0x0Eu
#define DS3231_REG_STATUS   0x0Fu

#define DS3231_STATUS_OSF   0x80u    /* Oscillator Stop Flag (bit 7)  */
#define DS3231_STATUS_A1F   0x01u    /* Alarme 1 disparou (bit 0)     */

#define DS3231_CTRL_A1IE    0x01u    /* habilita o Alarme 1           */
#define DS3231_CTRL_INTCN   0x04u    /* INT/SQW em modo interrupção   */
#define DS3231_CTRL_EOSC    0x80u    /* 0 = oscilador ativo em VBAT   */

/* Máscara do Alarme 1 para disparo DIÁRIO em hora:minuto:segundo:
 * A1M1=A1M2=A1M3=0 (casa segundos, minutos e horas) e A1M4=1
 * (ignora dia/data) — ver tabela "Alarm Mask Bits" do datasheet.     */
#define DS3231_A1M4_IGNORA_DIA  0x80u

/* ------------------------------------------------------------------
 * API pública
 *
 * Observação de implementação: as transações I2C são escritas
 * "abertas" (start/escreve/stop em sequência), sem sub-rotinas
 * auxiliares de leitura/escrita de registrador — cada nível de
 * chamada economizado importa na pilha de hardware de 8 posições
 * do PIC16, compartilhada com a ISR do USB (ver nota em usb_hid.c).
 * ------------------------------------------------------------------ */
bool ds3231_ler(ds3231_hora_t *hora)
{
    bool ok;

    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_SEGUNDOS);   /* ponteiro -> 00h     */
    i2c_start();
    ok &= i2c_escrever(DS3231_END_LEITURA);

    if (!ok) {
        /* Escravo não respondeu: aborta sem consumir bytes.          */
        i2c_stop();
        return false;
    }

    /* Rajada de 7 bytes (00h..06h). O DS3231 congela uma cópia dos
     * registradores no START, então a leitura é consistente mesmo
     * que a hora vire durante a transferência.                       */
    hora->segundos   = i2c_ler(I2C_ACK);
    hora->minutos    = i2c_ler(I2C_ACK);
    hora->horas      = (uint8_t)(i2c_ler(I2C_ACK) & 0x3Fu);
                       /* mascara bits 12/24 e AM/PM: modo 24 h       */
    hora->dia_semana = i2c_ler(I2C_ACK);
    hora->dia        = i2c_ler(I2C_ACK);
    hora->mes        = (uint8_t)(i2c_ler(I2C_ACK) & 0x1Fu);
                       /* descarta bit de século                      */
    hora->ano        = i2c_ler(I2C_NACK);
    i2c_stop();

    return true;
}

bool ds3231_gravar(const ds3231_hora_t *hora)
{
    bool ok;
    uint8_t controle;
    uint8_t status;

    /* Escreve 00h..06h em rajada (auto-incremento do ponteiro).      */
    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_SEGUNDOS);
    ok &= i2c_escrever(hora->segundos);
    ok &= i2c_escrever(hora->minutos);
    ok &= i2c_escrever((uint8_t)(hora->horas & 0x3Fu));  /* bit6=0: 24h */
    ok &= i2c_escrever(hora->dia_semana);
    ok &= i2c_escrever(hora->dia);
    ok &= i2c_escrever((uint8_t)(hora->mes & 0x1Fu));
    ok &= i2c_escrever(hora->ano);
    i2c_stop();

    if (!ok) {
        return false;
    }

    /* Controle (0Eh) e status (0Fh) por LEITURA-MODIFICAÇÃO-ESCRITA:
     * é essencial preservar o bit A1IE, senão acertar a hora
     * desabilitaria silenciosamente o alarme do usuário.
     * - controle: força EOSC=0 (oscilador ativo também em VBAT) e
     *   INTCN=1 (INT/SQW em modo interrupção), preservando A1IE/A2IE;
     * - status  : zera OSF (a hora passa a ser confiável). OSF só
     *   aceita escrita de 0, então reescrever o valor lido é seguro.
     * O ponteiro auto-incrementa de 0Eh para 0Fh: uma transação só.  */
    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_CONTROLE);
    i2c_start();
    ok &= i2c_escrever(DS3231_END_LEITURA);
    controle = i2c_ler(I2C_ACK);
    status   = i2c_ler(I2C_NACK);
    i2c_stop();

    if (!ok) {
        return false;
    }

    controle = (uint8_t)((controle & ~DS3231_CTRL_EOSC) | DS3231_CTRL_INTCN);
    status   = (uint8_t)(status & ~DS3231_STATUS_OSF);

    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_CONTROLE);
    ok &= i2c_escrever(controle);    /* 0Eh                           */
    ok &= i2c_escrever(status);      /* 0Fh                           */
    i2c_stop();

    return ok;
}

bool ds3231_hora_valida(void)
{
    uint8_t status;
    bool ok;

    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_STATUS);
    i2c_start();                     /* repeated START                */
    ok &= i2c_escrever(DS3231_END_LEITURA);
    status = i2c_ler(I2C_NACK);      /* único byte -> NACK encerra    */
    i2c_stop();

    if (!ok) {
        return false;                /* sem resposta = não confiável  */
    }
    return (status & DS3231_STATUS_OSF) == 0;
}

/* ==================================================================
 *  ALARME DIÁRIO (Alarme 1)
 * ================================================================== */

bool ds3231_alarme_gravar(uint8_t horas_bcd, uint8_t minutos_bcd)
{
    bool ok;

    /* Registradores 07h..0Ah em rajada:
     *   07h = segundos 00 com A1M1=0   -> casa "segundo == 00"
     *   08h = minutos  com A1M2=0      -> casa os minutos
     *   09h = horas    com A1M3=0      -> casa as horas (bit6=0: 24h)
     *   0Ah = A1M4=1                   -> ignora dia/data
     * Resultado: dispara uma vez por dia, em hh:mm:00.               */
    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_ALARME1);
    ok &= i2c_escrever(0x00u);
    ok &= i2c_escrever((uint8_t)(minutos_bcd & 0x7Fu));
    ok &= i2c_escrever((uint8_t)(horas_bcd & 0x3Fu));
    ok &= i2c_escrever(DS3231_A1M4_IGNORA_DIA);
    i2c_stop();

    return ok;
}

bool ds3231_alarme_ler(uint8_t *horas_bcd, uint8_t *minutos_bcd,
                       bool *habilitado)
{
    bool ok;
    uint8_t controle;

    /* 1) horário programado (07h..0Ah)                               */
    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_ALARME1);
    i2c_start();
    ok &= i2c_escrever(DS3231_END_LEITURA);

    if (!ok) {
        i2c_stop();
        return false;
    }

    (void)i2c_ler(I2C_ACK);                            /* 07h seg     */
    *minutos_bcd = (uint8_t)(i2c_ler(I2C_ACK) & 0x7Fu); /* 08h min    */
    *horas_bcd   = (uint8_t)(i2c_ler(I2C_ACK) & 0x3Fu); /* 09h hora   */
    (void)i2c_ler(I2C_NACK);                           /* 0Ah máscara */
    i2c_stop();

    /* 2) habilitação: bit A1IE do registrador de controle            */
    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_CONTROLE);
    i2c_start();
    ok &= i2c_escrever(DS3231_END_LEITURA);
    controle = i2c_ler(I2C_NACK);
    i2c_stop();

    *habilitado = ((controle & DS3231_CTRL_A1IE) != 0u);
    return ok;
}

bool ds3231_alarme_habilitar(bool ligar)
{
    bool ok;
    uint8_t controle;
    uint8_t status;

    /* Controle (0Eh) e status (0Fh) são adjacentes: uma leitura em
     * rajada pega os dois, e uma escrita em rajada devolve os dois.
     * (Aberto "na mão", sem sub-rotinas, para não gastar níveis da
     *  pilha de hardware — ver nota no topo deste arquivo.)          */
    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_CONTROLE);
    i2c_start();
    ok &= i2c_escrever(DS3231_END_LEITURA);
    controle = i2c_ler(I2C_ACK);
    status   = i2c_ler(I2C_NACK);
    i2c_stop();

    if (!ok) {
        return false;
    }

    /* Mexe só no A1IE, preservando EOSC, RS, A2IE...                 */
    if (ligar) {
        controle |= DS3231_CTRL_A1IE;
    } else {
        controle = (uint8_t)(controle & ~DS3231_CTRL_A1IE);
    }
    controle |= DS3231_CTRL_INTCN;   /* garante modo interrupção      */

    /* IMPORTANTE: o datasheet diz que A1F é setado a CADA casamento
     * de horário, INDEPENDENTE de A1IE — o bit A1IE só decide se o
     * pino INT/SQW também é acionado. Ou seja, um alarme "desligado"
     * continua deixando A1F em 1 quando seu horário passa.
     * Se esse resto não fosse limpo aqui, habilitar o alarme mais
     * tarde faria o firmware enxergar um disparo obsoleto e tocar na
     * hora errada. Trocar o estado do alarme sempre começa do zero.  */
    status = (uint8_t)(status & ~DS3231_STATUS_A1F);

    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_CONTROLE);
    ok &= i2c_escrever(controle);    /* 0Eh                           */
    ok &= i2c_escrever(status);      /* 0Fh                           */
    i2c_stop();

    return ok;
}

bool ds3231_alarme_disparou(void)
{
    bool ok;
    uint8_t status;

    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_STATUS);
    i2c_start();
    ok &= i2c_escrever(DS3231_END_LEITURA);
    status = i2c_ler(I2C_NACK);
    i2c_stop();

    if (!ok) {
        return false;
    }
    return (status & DS3231_STATUS_A1F) != 0u;
}

bool ds3231_alarme_reconhecer(void)
{
    bool ok;
    uint8_t status;

    /* Zera apenas A1F. Reescrever OSF com o valor lido é inofensivo:
     * esse bit só aceita escrita de 0, logo escrever 1 não o altera. */
    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_STATUS);
    i2c_start();
    ok &= i2c_escrever(DS3231_END_LEITURA);
    status = i2c_ler(I2C_NACK);
    i2c_stop();

    if (!ok) {
        return false;
    }

    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_STATUS);
    ok &= i2c_escrever((uint8_t)(status & ~DS3231_STATUS_A1F));
    i2c_stop();

    return ok;
}
