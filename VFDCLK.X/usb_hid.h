/**
 * =====================================================================
 *  usb_hid.h — Stack USB HID mínimo para o SIE do PIC16C745
 * =====================================================================
 *  Interface entre o stack (que roda todo dentro da ISR) e o laço
 *  principal (que roda o relógio). A fronteira é feita por dois
 *  "correios" protegidos contra concorrência:
 *
 *   PC -> dispositivo : report de ACERTO DE HORA (8 bytes, SET_REPORT)
 *       [0] 0x01 = comando "acertar RTC"
 *       [1] segundos BCD   [2] minutos BCD   [3] horas BCD (24 h)
 *       [4] dia da semana 1..7 (1 = segunda)
 *       [5] dia BCD        [6] mês BCD       [7] ano BCD (20xx)
 *       (mesma ordem dos registradores 00h..06h do DS3231)
 *
 *   dispositivo -> PC : report de ESTADO (8 bytes, EP1 IN/GET_REPORT)
 *       [0] flags: bit0 = hora do RTC válida, bit1 = leitura SHT ok
 *       [1] segundos BCD   [2] minutos BCD   [3] horas BCD
 *       [4] temperatura em décimos de °C, byte baixo (int16 LE)
 *       [5] temperatura, byte alto
 *       [6] umidade em décimos de %RH, byte baixo (uint16 LE)
 *       [7] umidade, byte alto
 * =====================================================================
 */
#ifndef USB_HID_H
#define USB_HID_H

#include <stdint.h>
#include <stdbool.h>

#define USB_TAM_REPORT  8u

/* Flags do byte [0] do report de estado                              */
#define USB_FLAG_HORA_VALIDA   0x01u
#define USB_FLAG_SENSOR_OK     0x02u

/* Comando do byte [0] do report de acerto                            */
#define USB_CMD_ACERTAR_RTC    0x01u

/* Liga o módulo USB, habilita interrupções do SIE e "pluga" o
 * dispositivo no barramento (DEV_ATT). Chamar com GIE ainda apagado. */
void usb_iniciar(void);

/* Tratador de interrupção do USB — chamar da ISR quando USBIF=1.
 * Faz TODO o protocolo: reset, enumeração (capítulo 9), classe HID
 * e o recebimento do report de acerto de hora.                       */
void usb_isr(void);

/* True depois que o host escolheu a configuração 1 (enumeração ok).  */
bool usb_configurado(void);

/* Se o PC enviou um acerto de hora desde a última chamada, copia os
 * 8 bytes do report para 'destino' e retorna true (uma única vez).
 * Chamar no laço principal. Protegido contra corrida com a ISR.      */
bool usb_pegar_acerto(uint8_t destino[USB_TAM_REPORT]);

/* Atualiza o report de estado que o host lê (EP1 IN / GET_REPORT).
 * Chamar no laço principal a cada mudança de segundo/medida.         */
void usb_atualizar_estado(const uint8_t estado[USB_TAM_REPORT]);

#endif /* USB_HID_H */
