/**
 * =====================================================================
 *  swi2c.h — Mestre I2C por software ("bit-bang")
 * =====================================================================
 *  O PIC16C745 NÃO possui módulo MSSP/I2C em hardware, portanto o
 *  barramento do DS3231 é gerado por software nos pinos definidos em
 *  board.h (RB0 = SDA, RB1 = SCL), a ~100 kHz.
 *
 *  Implementação mínima de mestre único: START, STOP, escrita de byte
 *  com leitura de ACK e leitura de byte com envio de ACK/NACK.
 * =====================================================================
 */
#ifndef SWI2C_H
#define SWI2C_H

#include <stdint.h>
#include <stdbool.h>

#define I2C_ACK   true
#define I2C_NACK  false

/* Coloca o barramento em repouso (SDA e SCL soltos = nível alto).    */
void i2c_iniciar_barramento(void);

/* Condição de START (SDA desce com SCL alto).                        */
void i2c_start(void);

/* Condição de STOP (SDA sobe com SCL alto).                          */
void i2c_stop(void);

/* Transmite um byte (MSB primeiro). Retorna true se o escravo
 * respondeu ACK (SDA em 0 no 9º pulso de clock).                     */
bool i2c_escrever(uint8_t byte);

/* Recebe um byte (MSB primeiro). Se enviar_ack = I2C_ACK, responde
 * ACK (mais bytes serão lidos); com I2C_NACK encerra a leitura.      */
uint8_t i2c_ler(bool enviar_ack);

#endif /* SWI2C_H */
