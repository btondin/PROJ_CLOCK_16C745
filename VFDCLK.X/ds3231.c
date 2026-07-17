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
#define DS3231_REG_CONTROLE 0x0Eu
#define DS3231_REG_STATUS   0x0Fu

#define DS3231_STATUS_OSF   0x80u    /* Oscillator Stop Flag (bit 7)  */

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

    /* Controle (0Eh): EOSC=0 (oscilador sempre ativo, inclusive em
     * VBAT), sem onda quadrada nem alarmes (INTCN=1 -> SQW inativo).
     * Status (0Fh): zera OSF — a partir de agora a hora é válida
     * (OSF só aceita escrita de 0; os demais bits zerados são
     * inócuos, alarmes não são usados neste projeto).
     * O ponteiro auto-incrementa de 0Eh para 0Fh: uma transação só.  */
    i2c_start();
    ok  = i2c_escrever(DS3231_END_ESCRITA);
    ok &= i2c_escrever(DS3231_REG_CONTROLE);
    ok &= i2c_escrever(0x04u);       /* 0Eh: INTCN=1                  */
    ok &= i2c_escrever(0x00u);       /* 0Fh: OSF=0                    */
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
