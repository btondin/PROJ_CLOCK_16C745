/**
 * =====================================================================
 *  swi2c.c — Mestre I2C por software ("bit-bang")
 * =====================================================================
 *  Temporização: meio-período de 5 us => SCL ~100 kHz (modo standard).
 *  O DS3231 aceita até 400 kHz; 100 kHz dá margem confortável para os
 *  pull-ups de 4k7 e para a capacitância de protoboard.
 *
 *  Convenções elétricas (ver board.h):
 *   - SDA é open-drain emulado: TRIS=1 solta (pull-up leva a 1),
 *     TRIS=0 força 0. O latch do pino fica permanentemente em 0.
 *   - SCL é push-pull via "sombra" de PORTB (mestre único, e o DS3231
 *     não estica o clock fora de casos que não usamos aqui).
 *
 *  Observação sobre interrupções: a ISR de USB pode interromper
 *  qualquer ponto da transação. Isso apenas ALONGA um nível de clock,
 *  o que o I2C tolera por construção (o barramento é estático entre
 *  bordas). Não é necessário desabilitar interrupções.
 * =====================================================================
 */
#include <xc.h>
#include "board.h"
#include "swi2c.h"

/* Atalho local para o meio-período (constante exigida por __delay_us) */
#define MEIO()  __delay_us(I2C_MEIO_PERIODO_US)

void i2c_iniciar_barramento(void)
{
    I2C_SDA_SOLTAR();
    I2C_SCL_ALTO();
    MEIO();
}

void i2c_start(void)
{
    /* Repouso garantido: SDA e SCL altos                             */
    I2C_SDA_SOLTAR();
    I2C_SCL_ALTO();
    MEIO();
    /* START = borda de descida em SDA com SCL alto                   */
    I2C_SDA_BAIXO();
    MEIO();
    I2C_SCL_BAIXO();
    MEIO();
}

void i2c_stop(void)
{
    /* Garante SDA baixo antes da borda de subida com SCL alto        */
    I2C_SDA_BAIXO();
    MEIO();
    I2C_SCL_ALTO();
    MEIO();
    /* STOP = borda de subida em SDA com SCL alto                     */
    I2C_SDA_SOLTAR();
    MEIO();
}

bool i2c_escrever(uint8_t byte)
{
    uint8_t i;
    bool ack;

    /* 8 bits de dados, MSB primeiro                                  */
    for (i = 0; i < 8; i++) {
        if (byte & 0x80u) {
            I2C_SDA_SOLTAR();      /* bit 1 = linha solta (pull-up)   */
        } else {
            I2C_SDA_BAIXO();       /* bit 0 = linha forçada em 0      */
        }
        byte <<= 1;
        MEIO();
        I2C_SCL_ALTO();            /* escravo amostra SDA aqui        */
        MEIO();
        I2C_SCL_BAIXO();
    }

    /* 9º clock: ACK do escravo (ele puxa SDA para 0)                 */
    I2C_SDA_SOLTAR();              /* devolve a linha ao escravo      */
    MEIO();
    I2C_SCL_ALTO();
    MEIO();
    ack = (I2C_SDA_LER() == 0);    /* 0 = ACK, 1 = NACK               */
    I2C_SCL_BAIXO();
    MEIO();

    return ack;
}

uint8_t i2c_ler(bool enviar_ack)
{
    uint8_t i;
    uint8_t valor = 0;

    I2C_SDA_SOLTAR();              /* escravo controla SDA agora      */

    for (i = 0; i < 8; i++) {
        MEIO();
        I2C_SCL_ALTO();
        MEIO();
        valor <<= 1;
        if (I2C_SDA_LER()) {
            valor |= 1u;
        }
        I2C_SCL_BAIXO();
    }

    /* 9º clock: nosso ACK (continua) ou NACK (último byte)           */
    if (enviar_ack) {
        I2C_SDA_BAIXO();
    } else {
        I2C_SDA_SOLTAR();
    }
    MEIO();
    I2C_SCL_ALTO();
    MEIO();
    I2C_SCL_BAIXO();
    I2C_SDA_SOLTAR();              /* libera a linha ao final         */
    MEIO();

    return valor;
}
