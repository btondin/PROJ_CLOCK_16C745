/**
 * =====================================================================
 *  sht1x.c — Driver do sensor Sensirion SHT15 (temperatura + umidade)
 * =====================================================================
 *  Referências oficiais Sensirion (pasta DOCS/ do projeto):
 *   - "Datasheet SHT1x" (V5): protocolo, comandos, coeficientes V4
 *   - "SHT1x and SHT7x Sample Code" (V2.4): base desta implementação
 *   - "CRC Checksum Calculation ... SHT1x/SHT7x" (V2): algoritmo CRC-8
 *
 *  PROTOCOLO (resumo):
 *   - "Transmission Start": DATA desce com SCK alto, pulso de SCK
 *     baixo, DATA sobe com SCK alto (sequência exclusiva do Sensibus).
 *   - Comando de 8 bits (3 bits de endereço "000" + 5 de comando):
 *       00000011 (0x03) = medir temperatura (14 bits)
 *       00000101 (0x05) = medir umidade relativa (12 bits)
 *   - O sensor confirma cada byte com ACK (puxa DATA em 0 no 9º SCK).
 *   - Fim da medição: o sensor SOLTA DATA para 0 (dados prontos).
 *     Espera máx.: 320 ms para 14 bits (datasheet, seção 3.3).
 *   - Leitura: MSB, LSB (ambos com ACK do PIC) e CRC (com NACK).
 *
 *  CONVERSÃO EM PONTO FIXO (sem float, economiza ROM/RAM):
 *   Temperatura (14 bits, alimentação 5,0 V, datasheet 4.3):
 *       T[°C] = d1 + d2·SO_T,  d1 = -40,1  d2 = 0,01
 *     Em décimos de °C:
 *       T10 = (SO_T + 5)/10 - 401        (+5 = arredondamento)
 *   Umidade (12 bits, coeficientes V4, datasheet 4.1):
 *       RH_lin[%] = c1 + c2·SO + c3·SO²
 *           c1 = -2,0468  c2 = 0,0367  c3 = -1,5955e-6
 *     Em centésimos de %RH (inteiros de 32 bits):
 *       RH100 = (15032·SO)>>12  -  SO²/6268  -  205
 *         (15032/4096 = 3,66992 ≈ 100·c2 ;  1/6268 = 1,5954e-4 ≈ -100·c3)
 *   Compensação de temperatura (datasheet 4.2):
 *       RH_true[%] = (T-25)·(t1 + t2·SO) + RH_lin,  t1=0,01  t2=0,00008
 *     Em centésimos, com T em décimos:
 *       comp = ((T10-250)·(1638 + 13·SO)) >> 14
 *         (1638/16384 = 0,09998 ≈ 10·t1 ;  13/16384 = 7,93e-4 ≈ 10·t2)
 *   O erro total da aproximação é < 0,05 °C / 0,15 %RH em toda a
 *   faixa útil — validado pelo script DTCAPP/teste_conversao.py, que
 *   compara esta aritmética com as fórmulas float oficiais.
 * =====================================================================
 */
#include <xc.h>
#include "board.h"
#include "sht1x.h"

/* Comandos do SHT1x (datasheet, seção 3.2)                           */
#define SHT_CMD_MEDIR_TEMP  0x03u
#define SHT_CMD_MEDIR_UMID  0x05u

/* Espera máxima pelo fim da medição: 14 bits leva até 320 ms.
 * 4000 passos de 100 us = 400 ms de timeout.                         */
#define SHT_TIMEOUT_PASSOS  4000u

/* Atalho para o meio-período de clock (~5 us, igual ao sample code)  */
#define MEIO()  __delay_us(SHT_MEIO_PERIODO_US)

/* ------------------------------------------------------------------
 * Primitivas do barramento Sensibus
 * ------------------------------------------------------------------ */

/* "Transmission Start" — assinatura exclusiva do protocolo:
 *        _____         ________
 *  DATA:      |_______|
 *            ___     ___
 *  SCK : ___|   |___|   |______                                      */
static void sht_transmission_start(void)
{
    SHT_DATA_SOLTAR();          /* estado inicial: DATA=1, SCK=0      */
    SHT_SCK_BAIXO();
    MEIO();
    SHT_SCK_ALTO();
    MEIO();
    SHT_DATA_BAIXO();
    MEIO();
    SHT_SCK_BAIXO();
    MEIO();
    SHT_SCK_ALTO();
    MEIO();
    SHT_DATA_SOLTAR();
    MEIO();
    SHT_SCK_BAIXO();
    MEIO();
}

/* Escreve um byte (MSB primeiro) e devolve true se o sensor deu ACK. */
static bool sht_escrever_byte(uint8_t valor)
{
    uint8_t mascara;
    bool ack;

    for (mascara = 0x80u; mascara != 0u; mascara >>= 1) {
        if (valor & mascara) {
            SHT_DATA_SOLTAR();
        } else {
            SHT_DATA_BAIXO();
        }
        MEIO();
        SHT_SCK_ALTO();         /* sensor amostra DATA com SCK alto   */
        MEIO();
        SHT_SCK_BAIXO();
    }

    /* 9º clock: ACK do sensor (ele puxa DATA para 0)                 */
    SHT_DATA_SOLTAR();
    MEIO();
    SHT_SCK_ALTO();
    MEIO();
    ack = (SHT_DATA_LER() == 0);
    SHT_SCK_BAIXO();
    MEIO();

    return ack;
}

/* Lê um byte (MSB primeiro). com_ack=true -> PIC responde ACK
 * (haverá mais bytes); false -> NACK (encerra a leitura).            */
static uint8_t sht_ler_byte(bool com_ack)
{
    uint8_t i;
    uint8_t valor = 0;

    SHT_DATA_SOLTAR();          /* sensor controla DATA               */
    for (i = 0; i < 8; i++) {
        MEIO();
        SHT_SCK_ALTO();
        MEIO();
        valor <<= 1;
        if (SHT_DATA_LER()) {
            valor |= 1u;
        }
        SHT_SCK_BAIXO();
    }

    /* 9º clock: ACK do PIC (DATA em 0) ou NACK (DATA solto)          */
    if (com_ack) {
        SHT_DATA_BAIXO();
    } else {
        SHT_DATA_SOLTAR();
    }
    MEIO();
    SHT_SCK_ALTO();
    MEIO();
    SHT_SCK_BAIXO();
    MEIO();
    SHT_DATA_SOLTAR();

    return valor;
}

void sht1x_reset_comunicacao(void)
{
    uint8_t i;

    /* DATA solto + 9 ou mais clocks: limpa a interface serial do
     * sensor caso a comunicação tenha se perdido no meio de um byte. */
    SHT_DATA_SOLTAR();
    SHT_SCK_BAIXO();
    for (i = 0; i < 9; i++) {
        MEIO();
        SHT_SCK_ALTO();
        MEIO();
        SHT_SCK_BAIXO();
    }
}

/* ------------------------------------------------------------------
 * CRC-8 Sensirion — polinômio x^8 + x^5 + x^4 + 1 (0x31), MSB antes.
 * Peculiaridades (app note "CRC Checksum Calculation"):
 *   - valor inicial = nibble baixo do status register, com os bits
 *     invertidos. Status = 0x00 (padrão de fábrica) -> inicial 0x00;
 *   - cobre o byte de comando + os 2 bytes de dados (sem os ACKs);
 *   - o resultado final deve ser BIT-INVERTIDO antes de comparar com
 *     o CRC transmitido pelo sensor.
 * ------------------------------------------------------------------ */
static uint8_t crc8_passo(uint8_t crc, uint8_t dado)
{
    uint8_t i;

    crc ^= dado;
    for (i = 0; i < 8; i++) {
        if (crc & 0x80u) {
            crc = (uint8_t)((uint8_t)(crc << 1) ^ 0x31u);
        } else {
            crc = (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Inverte a ordem dos bits (b7<->b0, b6<->b1, ...)                   */
static uint8_t inverter_bits(uint8_t v)
{
    uint8_t r = 0;
    uint8_t i;

    for (i = 0; i < 8; i++) {
        r <<= 1;
        r |= (uint8_t)(v & 1u);
        v >>= 1;
    }
    return r;
}

/* ------------------------------------------------------------------
 * Medição completa com conversão em ponto fixo (ver cabeçalho)
 *
 * As duas medições (temperatura e umidade) são feitas num laço único
 * dentro desta função — e não numa sub-rotina "medir_bruto" — para
 * economizar um nível da pilha de hardware de 8 posições do PIC16
 * (compartilhada com a interrupção do USB; ver nota em usb_hid.c).
 * ------------------------------------------------------------------ */
bool sht1x_medir(sht1x_medida_t *medida)
{
    static const uint8_t comando_de[2] = {
        SHT_CMD_MEDIR_TEMP,              /* passo 0: temperatura      */
        SHT_CMD_MEDIR_UMID               /* passo 1: umidade          */
    };
    uint16_t bruto[2];  /* leituras: [0]=SO_T 14 bits, [1]=SO_RH 12   */
    uint8_t  passo;
    uint16_t i;
    uint8_t  msb, lsb, crc_sensor, crc;
    int16_t  t10;       /* temperatura em décimos de °C               */
    int32_t  rh100;     /* umidade em centésimos de %RH               */

    for (passo = 0; passo < 2u; passo++) {
        sht_transmission_start();
        if (!sht_escrever_byte(comando_de[passo])) {
            sht1x_reset_comunicacao();   /* sensor não respondeu      */
            return false;
        }

        /* O sensor sinaliza "dados prontos" soltando DATA p/ nível 0.
         * Espera com timeout de ~400 ms (14 bits leva até 320 ms).   */
        for (i = 0; i < SHT_TIMEOUT_PASSOS; i++) {
            if (SHT_DATA_LER() == 0) {
                break;
            }
            __delay_us(100);
        }
        if (SHT_DATA_LER() != 0) {
            sht1x_reset_comunicacao();   /* timeout                   */
            return false;
        }

        msb        = sht_ler_byte(true);
        lsb        = sht_ler_byte(true);
        crc_sensor = sht_ler_byte(false);    /* NACK encerra          */

        /* Verificação de integridade (ver bloco de comentário acima) */
        crc = crc8_passo(0x00u, comando_de[passo]);
        crc = crc8_passo(crc, msb);
        crc = crc8_passo(crc, lsb);
        if (inverter_bits(crc) != crc_sensor) {
            sht1x_reset_comunicacao();   /* dado corrompido           */
            return false;
        }

        bruto[passo] = (uint16_t)(((uint16_t)msb << 8) | lsb);
    }

    /* Nomes claros para o trecho de conversão                        */
    {
        uint16_t so_t  = bruto[0];
        uint16_t so_rh = bruto[1];

        /* --- temperatura: T10 = (SO_T + 5)/10 - 401 ---------------- */
        t10 = (int16_t)((so_t + 5u) / 10u) - 401;

        /* --- umidade linear em centi-%RH ---------------------------
         *   RH100 = 3,66992·SO - 1,5954e-4·SO² - 2,05                */
        rh100  = ((int32_t)15032 * so_rh) >> 12;
        rh100 -= ((int32_t)so_rh * so_rh) / 6268;
        rh100 -= 205;

        /* --- compensação de temperatura ----------------------------
         *   comp = (T-25°C)·(0,01 + 0,00008·SO) em centi-%RH         */
        rh100 += ((int32_t)(t10 - 250) *
                  (1638 + (int32_t)13 * so_rh)) >> 14;

        /* --- recorte à faixa física (0,1..100 %) e arredondamento -- */
        if (rh100 > 10000) {
            rh100 = 10000;
        }
        if (rh100 < 10) {
            rh100 = 10;
        }

        medida->temperatura_dC = t10;
        medida->umidade_dRH    = (uint16_t)((rh100 + 5) / 10);
    }

    return true;
}
