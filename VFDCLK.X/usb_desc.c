/**
 * =====================================================================
 *  usb_desc.c — Descritores USB do dispositivo (HID, low-speed)
 * =====================================================================
 *  Referências:
 *   - USB 1.1, capítulo 9 (descritores padrão)
 *   - "Device Class Definition for HID 1.11" (descritor HID e report)
 *
 *  Restrições de LOW-SPEED relevantes (USB 1.1, cap. 5):
 *   - EP0 com pacote máximo de 8 bytes
 *   - endpoints de interrupção com pacote máximo de 8 bytes
 *   - bInterval mínimo de 10 ms
 *  Por isso os reports têm exatamente 8 bytes.
 * =====================================================================
 */
#include "usb_desc.h"

/* ------------------------------------------------------------------
 * Descritor de dispositivo (18 bytes)
 * ------------------------------------------------------------------ */
const uint8_t usb_desc_dispositivo[USB_TAM_DESC_DISPOSITIVO] = {
    18,                     /* bLength                                */
    0x01,                   /* bDescriptorType = DEVICE               */
    0x10, 0x01,             /* bcdUSB = 1.10                          */
    0x00,                   /* bDeviceClass  (definida na interface)  */
    0x00,                   /* bDeviceSubClass                        */
    0x00,                   /* bDeviceProtocol                        */
    8,                      /* bMaxPacketSize0 = 8 (low-speed)        */
    (uint8_t)(USB_VID & 0xFFu), (uint8_t)(USB_VID >> 8),   /* idVendor  */
    (uint8_t)(USB_PID & 0xFFu), (uint8_t)(USB_PID >> 8),   /* idProduct */
    0x00, 0x01,             /* bcdDevice = 1.00                       */
    1,                      /* iManufacturer (string 1)               */
    2,                      /* iProduct      (string 2)               */
    0,                      /* iSerialNumber (nenhum)                 */
    1                       /* bNumConfigurations                     */
};

/* ------------------------------------------------------------------
 * Conjunto de configuração: CONFIG + INTERFACE + HID + ENDPOINT
 * (o host pede tudo de uma vez com GET_DESCRIPTOR(CONFIGURATION))
 * ------------------------------------------------------------------ */
const uint8_t usb_desc_config[USB_TAM_DESC_CONFIG] = {
    /* --- configuração (9 bytes) ------------------------------------ */
    9,                      /* bLength                                */
    0x02,                   /* bDescriptorType = CONFIGURATION        */
    USB_TAM_DESC_CONFIG, 0, /* wTotalLength = 34                      */
    1,                      /* bNumInterfaces                         */
    1,                      /* bConfigurationValue                    */
    0,                      /* iConfiguration                         */
    0xC0,                   /* bmAttributes: auto-alimentado          */
    10,                     /* bMaxPower = 20 mA (unidades de 2 mA;   */
                            /*  a placa é alimentada por fonte própria,*/
                            /*  o USB alimenta só o transceptor)      */

    /* --- interface 0: HID (9 bytes) --------------------------------- */
    9,                      /* bLength                                */
    0x04,                   /* bDescriptorType = INTERFACE            */
    0,                      /* bInterfaceNumber                       */
    0,                      /* bAlternateSetting                      */
    1,                      /* bNumEndpoints (só EP1 IN)              */
    0x03,                   /* bInterfaceClass = HID                  */
    0x00,                   /* bInterfaceSubClass (não-boot)          */
    0x00,                   /* bInterfaceProtocol                     */
    0,                      /* iInterface                             */

    /* --- descritor HID (9 bytes) — offset 18, ver USB_OFFSET_DESC_HID */
    9,                      /* bLength                                */
    0x21,                   /* bDescriptorType = HID                  */
    0x11, 0x01,             /* bcdHID = 1.11                          */
    0,                      /* bCountryCode                           */
    1,                      /* bNumDescriptors                        */
    0x22,                   /* bDescriptorType = REPORT               */
    USB_TAM_DESC_REPORT, 0, /* wDescriptorLength                      */

    /* --- endpoint EP1 IN interrupt (7 bytes) ------------------------ */
    7,                      /* bLength                                */
    0x05,                   /* bDescriptorType = ENDPOINT             */
    0x81,                   /* bEndpointAddress = EP1, IN             */
    0x03,                   /* bmAttributes = interrupt               */
    8, 0,                   /* wMaxPacketSize = 8                     */
    10                      /* bInterval = 10 ms (mínimo low-speed)   */
};

/* ------------------------------------------------------------------
 * Report descriptor HID (25 bytes)
 * Vendor-defined: um report de entrada de 8 bytes e um de saída de
 * 8 bytes, sem Report ID (há apenas um formato de cada direção).
 * ------------------------------------------------------------------ */
const uint8_t usb_desc_report[USB_TAM_DESC_REPORT] = {
    0x06, 0x00, 0xFF,       /* Usage Page (Vendor Defined 0xFF00)     */
    0x09, 0x01,             /* Usage (0x01)                           */
    0xA1, 0x01,             /* Collection (Application)               */
    0x15, 0x00,             /*   Logical Minimum (0)                  */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255)                */
    0x75, 0x08,             /*   Report Size (8 bits)                 */
    0x95, 0x08,             /*   Report Count (8 campos)              */
    0x09, 0x02,             /*   Usage (0x02)                         */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute)     */
    0x09, 0x03,             /*   Usage (0x03)                         */
    0x91, 0x02,             /*   Output (Data, Variable, Absolute)    */
    0xC0                    /* End Collection                         */
};

/* ------------------------------------------------------------------
 * Strings (UTF-16LE, como exige o USB)
 * ------------------------------------------------------------------ */
/* String 0: lista de idiomas — 0x0409 = inglês/EUA (padrão de fato)  */
const uint8_t usb_str_idiomas[] = {
    4, 0x03, 0x09, 0x04
};

/* String 1: fabricante                                               */
const uint8_t usb_str_fabricante[] = {
    2 + 2 * 6, 0x03,
    'V', 0, 'F', 0, 'D', 0, 'C', 0, 'L', 0, 'K', 0
};

/* String 2: produto (o app do PC procura por este nome)              */
const uint8_t usb_str_produto[] = {
    2 + 2 * 13, 0x03,
    'V', 0, 'F', 0, 'D', 0, 'C', 0, 'L', 0, 'K', 0, ' ', 0,
    '1', 0, '6', 0, 'C', 0, '7', 0, '4', 0, '5', 0
};
