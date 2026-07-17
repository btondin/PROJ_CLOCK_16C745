/**
 * =====================================================================
 *  usb_desc.h — Descritores USB do dispositivo (HID, low-speed)
 * =====================================================================
 *  O dispositivo se apresenta como HID genérico (classe 3), o que
 *  dispensa driver no Windows/Linux/macOS. A troca de dados usa:
 *    - report de SAÍDA (PC -> dispositivo, via SET_REPORT no EP0):
 *        acerto de hora/data — 8 bytes
 *    - report de ENTRADA (dispositivo -> PC, EP1 interrupt IN e/ou
 *        GET_REPORT): estado atual (hora + temperatura + umidade)
 *
 *  VID/PID: par de TESTE do projeto pid.codes (VID 0x1209 / PID
 *  0x0001). Serve para desenvolvimento e uso pessoal; para publicar
 *  um produto, solicite um PID próprio (https://pid.codes).
 * =====================================================================
 */
#ifndef USB_DESC_H
#define USB_DESC_H

#include <stdint.h>

#define USB_VID  0x1209u   /* pid.codes                               */
#define USB_PID  0x0001u   /* PID de teste — NÃO usar em produto      */

/* Tamanhos (o compilador confere com os arrays em usb_desc.c)        */
#define USB_TAM_DESC_DISPOSITIVO  18u
#define USB_TAM_DESC_CONFIG       34u
#define USB_TAM_DESC_HID           9u
#define USB_TAM_DESC_REPORT       25u

/* Deslocamento do descritor HID dentro do conjunto de configuração
 * (config 9 bytes + interface 9 bytes = 18)                          */
#define USB_OFFSET_DESC_HID       18u

extern const uint8_t usb_desc_dispositivo[USB_TAM_DESC_DISPOSITIVO];
extern const uint8_t usb_desc_config[USB_TAM_DESC_CONFIG];
extern const uint8_t usb_desc_report[USB_TAM_DESC_REPORT];

/* Strings (formato descritor: [tamanho, 0x03, UTF-16LE...])          */
extern const uint8_t usb_str_idiomas[];
extern const uint8_t usb_str_fabricante[];
extern const uint8_t usb_str_produto[];

#endif /* USB_DESC_H */
