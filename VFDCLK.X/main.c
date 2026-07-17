/**
 * =====================================================================
 *  main.c — Relógio VFD com PIC16C745 + DS3231 + SHT15
 * =====================================================================
 *  Funções:
 *   - exibe HORA/DATA no display VFD 20x2 (IEE 036X2, modo serial),
 *     alternando com TEMPERATURA/UMIDADE (Sensirion SHT15);
 *   - mantém a hora num RTC DS3231 (I2C por software, com bateria);
 *   - ao conectar no USB do PC, o utilitário DTCAPP/dtc_sync.py envia
 *     a hora local e o firmware atualiza o RTC (dispositivo HID, sem
 *     driver). O stack USB roda inteiro por interrupção (usb_hid.c).
 *
 *  ARQUITETURA DO LAÇO PRINCIPAL (cooperativo, sem RTOS):
 *   a cada ~50 ms lê o DS3231; quando o segundo vira:
 *     - redesenha a tela corrente (hora ou clima);
 *     - alterna as telas (6 s hora / 4 s clima);
 *     - a cada 30 s dispara uma medição do SHT15 (bloqueante ~0,4 s;
 *       a ISR de USB continua atendendo o barramento normalmente);
 *     - atualiza o report de estado que o PC pode ler via USB.
 *   fora do tick, verifica se chegou acerto de hora pelo USB.
 *
 *  Bits de configuração (datasheet DS41124D, seção 12.1):
 *   FOSC=HS -> cristal HS de 24 MHz direto, sem PLL (o USB low-speed
 *              exige exatamente 24 MHz); WDT desligado; PWRT ligado.
 * =====================================================================
 */
#include <xc.h>
#include <stdint.h>
#include <stdbool.h>

#include "board.h"
#include "uart.h"
#include "vfd.h"
#include "swi2c.h"
#include "ds3231.h"
#include "sht1x.h"
#include "usb_hid.h"

/* ------------------------------------------------------------------
 * Bits de configuração (gravados junto com o programa)
 * ------------------------------------------------------------------ */
#pragma config FOSC  = HS    /* cristal de 24 MHz (HS, sem PLL)       */
#pragma config WDTE  = OFF   /* watchdog desligado                    */
#pragma config PWRTE = ON    /* power-up timer: alimentação estável   */
#pragma config CP    = OFF   /* sem proteção de código                */

/* ------------------------------------------------------------------
 * Ajustes de comportamento (fáceis de personalizar)
 * ------------------------------------------------------------------ */
#define TELA_HORA_SEGUNDOS    6u   /* tempo exibindo hora/data        */
#define TELA_CLIMA_SEGUNDOS   4u   /* tempo exibindo temp/umidade     */
#define SHT_PERIODO_SEGUNDOS  30u  /* intervalo entre medições        */
#define MSG_SYNC_SEGUNDOS     3u   /* aviso "hora sincronizada"       */
#define BRILHO_PADRAO         VFD_BRILHO_MAXIMO

/* Sombra do latch de PORTB — ver explicação em board.h               */
volatile uint8_t portb_sombra;

/* ------------------------------------------------------------------
 * Inicialização dos pinos (declarada em board.h)
 * ------------------------------------------------------------------ */
void board_iniciar_pinos(void)
{
    /* PORTA: não usado. Deixa como entrada e desliga o A/D para que
     * RA0..RA5 funcionem como digitais caso venham a ser usados.
     * (No PIC16C745, ADCON1 com PCFG=0b111 põe todos os canais
     *  em modo digital — ver seção 11 do datasheet DS41124D.)       */
    ADCON1 = 0x07;
    TRISA  = 0xFF;

    /* PORTB:
     *  RB0 (SDA) e RB2 (DATA SHT): latch 0, iniciam SOLTOS (TRIS=1)
     *  RB1 (SCL) e RB3 (SCK)     : saídas push-pull, iniciam em nível
     *                              de repouso (SCL=1 alto, SCK=0 baixo)
     *  RB4..RB7                  : entradas (livres)                 */
    portb_sombra = I2C_SCL_MASCARA;      /* SCL=1, SCK=0, SDA=DATA=0 */
    PORTB_APLICAR();
    TRISB = (uint8_t)~(I2C_SCL_MASCARA | SHT_SCK_MASCARA);
    /* = 0b11110101: SCL e SCK saídas; SDA e DATA soltos (entradas)  */

    /* PORTC: RC6 = TX (o USART assume o controle do pino quando
     * TXEN=1/SPEN=1); RC4/RC5 são do USB. Demais como entrada.       */
    TRISC = 0xFF;
}

/* ------------------------------------------------------------------
 * Estado global do relógio
 * ------------------------------------------------------------------ */
static ds3231_hora_t  hora_atual;
static sht1x_medida_t medida;              /* última leitura do SHT15 */
static bool ha_medida    = false;          /* já houve leitura boa?   */
static bool rtc_presente = false;          /* DS3231 respondeu?       */
static bool hora_valida  = false;          /* OSF limpo no DS3231?    */

/* Nomes dos dias da semana (1 = segunda ... 7 = domingo)             */
static const char * const nome_dia[7] = {
    "SEG", "TER", "QUA", "QUI", "SEX", "SAB", "DOM"
};

/* ------------------------------------------------------------------
 * Interrupção única do PIC16 midrange: despacha para o stack USB.
 * A flag PIR1.USBIF é limpa ANTES de servir as flags detalhadas do
 * UIR: se um novo evento chegar durante o atendimento, USBIF volta a
 * subir e a ISR reentra — nenhum evento é perdido.
 * ------------------------------------------------------------------ */
void __interrupt() interrupcao(void)
{
    if (PIE1bits.USBIE && PIR1bits.USBIF) {
        PIR1bits.USBIF = 0;
        usb_isr();
    }
}

/* ------------------------------------------------------------------
 * Formatação de texto (sem printf: economiza ~1,5 K words de ROM)
 * ------------------------------------------------------------------ */

/* Escreve os 2 dígitos de um valor BCD em p[0..1]                    */
static void por_bcd(char *p, uint8_t bcd)
{
    p[0] = (char)('0' + (bcd >> 4));
    p[1] = (char)('0' + (bcd & 0x0Fu));
}

/* Escreve um valor em décimos como "xx.d", alinhado à DIREITA,
 * terminando na posição 'fim' (ex.: -53 -> "-5.3").                  */
static void por_decimos(char *fim, int16_t valor)
{
    bool negativo = (valor < 0);
    uint16_t v = negativo ? (uint16_t)(-valor) : (uint16_t)valor;
    uint16_t inteiro = v / 10u;

    *fim-- = (char)('0' + (v % 10u));
    *fim-- = '.';
    do {
        *fim-- = (char)('0' + (inteiro % 10u));
        inteiro /= 10u;
    } while (inteiro != 0u);
    if (negativo) {
        *fim = '-';
    }
}

/* Preenche um buffer de linha com espaços (20 colunas + terminador)  */
static void linha_limpa(char *b)
{
    uint8_t i;

    for (i = 0; i < VFD_COLUNAS; i++) {
        b[i] = ' ';
    }
    b[VFD_COLUNAS] = '\0';
}

/* ------------------------------------------------------------------
 * Telas
 * ------------------------------------------------------------------ */

/* Tela 1 — hora e data:  |      14:35:27      |
 *                        |   QUA 16/07/2026   |                      */
static void tela_hora(void)
{
    char b[VFD_COLUNAS + 1];
    const char *dia;

    linha_limpa(b);
    if (rtc_presente) {
        por_bcd(&b[6], hora_atual.horas);
        b[8] = ':';
        por_bcd(&b[9], hora_atual.minutos);
        b[11] = ':';
        por_bcd(&b[12], hora_atual.segundos);
    } else {
        /* RTC mudo no I2C: avisa em vez de mostrar lixo              */
        b[6] = 'S'; b[7] = 'E'; b[8] = 'M'; b[10] = 'R';
        b[11] = 'T'; b[12] = 'C';
    }
    vfd_linha(0, b);

    linha_limpa(b);
    if (rtc_presente) {
        if ((hora_atual.dia_semana >= 1u) && (hora_atual.dia_semana <= 7u)) {
            dia = nome_dia[hora_atual.dia_semana - 1u];
            b[3] = dia[0]; b[4] = dia[1]; b[5] = dia[2];
        }
        por_bcd(&b[7], hora_atual.dia);
        b[9] = '/';
        por_bcd(&b[10], hora_atual.mes);
        b[12] = '/';
        b[13] = '2'; b[14] = '0';          /* século 20xx assumido    */
        por_bcd(&b[15], hora_atual.ano);
    }
    vfd_linha(1, b);
}

/* Tela 2 — clima:  |  TEMP:   23.4 C    |
 *                  |  UMID:   45.2 %    |                            */
static void tela_clima(void)
{
    char b[VFD_COLUNAS + 1];

    linha_limpa(b);
    b[2] = 'T'; b[3] = 'E'; b[4] = 'M'; b[5] = 'P'; b[6] = ':';
    if (ha_medida) {
        por_decimos(&b[13], medida.temperatura_dC);
    } else {
        /* sensor ainda mudo: "--.-"                                  */
        b[10] = '-'; b[11] = '-'; b[12] = '.'; b[13] = '-';
    }
    b[15] = 'C';
    vfd_linha(0, b);

    linha_limpa(b);
    b[2] = 'U'; b[3] = 'M'; b[4] = 'I'; b[5] = 'D'; b[6] = ':';
    if (ha_medida) {
        por_decimos(&b[13], (int16_t)medida.umidade_dRH);
    } else {
        b[10] = '-'; b[11] = '-'; b[12] = '.'; b[13] = '-';
    }
    b[15] = '%';
    vfd_linha(1, b);
}

/* Aviso enquanto o RTC não tem hora confiável (OSF setado)           */
static void tela_ajuste_pendente(void)
{
    vfd_linha(0, "  HORA NAO AJUSTADA");
    vfd_linha(1, " CONECTE AO PC (USB)");
}

/* Confirmação após receber acerto pelo USB                           */
static void tela_sincronizada(void)
{
    vfd_linha(0, "  HORA SINCRONIZADA");
    vfd_linha(1, "      VIA USB");
}

/* ------------------------------------------------------------------
 * Monta o report de estado para o host USB (função-FOLHA: quem envia
 * é o main, chamando usb_atualizar_estado logo em seguida — dois
 * ramos rasos em vez de uma cadeia funda, por causa da pilha de
 * hardware de 8 níveis; ver nota em usb_hid.c)
 * ------------------------------------------------------------------ */
static void montar_estado_usb(uint8_t r[USB_TAM_REPORT])
{
    r[0] = 0;
    if (hora_valida) {
        r[0] |= USB_FLAG_HORA_VALIDA;
    }
    if (ha_medida) {
        r[0] |= USB_FLAG_SENSOR_OK;
    }
    r[1] = hora_atual.segundos;
    r[2] = hora_atual.minutos;
    r[3] = hora_atual.horas;
    r[4] = (uint8_t)(medida.temperatura_dC & 0xFF);
    r[5] = (uint8_t)((uint16_t)medida.temperatura_dC >> 8);
    r[6] = (uint8_t)(medida.umidade_dRH & 0xFF);
    r[7] = (uint8_t)(medida.umidade_dRH >> 8);
}

/* ------------------------------------------------------------------
 * Validação leve do acerto recebido do PC (evita gravar lixo no RTC
 * se um report malformado chegar)
 * ------------------------------------------------------------------ */
static bool bcd_ok(uint8_t v, uint8_t maximo_bcd)
{
    if (((v & 0x0Fu) > 9u) || ((v >> 4) > 9u)) {
        return false;                       /* nibbles não decimais   */
    }
    return v <= maximo_bcd;
}

static bool acerto_valido(const uint8_t a[USB_TAM_REPORT])
{
    return bcd_ok(a[1], 0x59u) &&           /* segundos               */
           bcd_ok(a[2], 0x59u) &&           /* minutos                */
           bcd_ok(a[3], 0x23u) &&           /* horas (24 h)           */
           (a[4] >= 1u) && (a[4] <= 7u) &&  /* dia da semana          */
           bcd_ok(a[5], 0x31u) && (a[5] != 0u) &&  /* dia             */
           bcd_ok(a[6], 0x12u) && (a[6] != 0u) &&  /* mês             */
           bcd_ok(a[7], 0x99u);             /* ano                    */
}

/* ------------------------------------------------------------------
 * Programa principal
 * ------------------------------------------------------------------ */
void main(void)
{
    /* ================================================================
     *  TESTE DE BRING-UP (heartbeat de LED) — REMOVER PARA USO NORMAL
     * ----------------------------------------------------------------
     *  Pisca um LED a 1 Hz e NÃO avança para o restante do programa.
     *  Serve para confirmar na protoboard que o PIC está "vivo": se o
     *  LED pisca a ~1 Hz, então o cristal de 24 MHz (modo HS),
     *  a alimentação e o reset estão OK. Se ficar apagado, aceso fixo,
     *  ou piscar em ritmo errado, o problema está no clock/alimentação
     *  (o período do pisca vem direto do relógio de instruções).
     *
     *  LED:  RC0 (pino 11) ──►|──[ ~330 Ω ]── GND   (acende em nível 1)
     *
     *  Para voltar à operação normal do relógio, basta APAGAR este
     *  bloco (o resto do código permanece inalterado).
     *
     *  NOTA: o laço é guardado por uma flag 'volatile' sempre igual a 1
     *  (ou seja, é um while(1) que nunca sai) em vez de um "while(1)"
     *  literal DE PROPÓSITO: assim o compilador não trata o resto do
     *  main como código morto, preserva a sobreposição de RAM das
     *  variáveis locais e o binário continua cabendo no BANK0.
     * ================================================================ */
    static volatile uint8_t heartbeat_ligado = 1u;

    TRISCbits.TRISC0 = 0;            /* RC0 como saída digital          */
    while (heartbeat_ligado) {       /* sempre verdadeiro = while(1)    */
        PORTCbits.RC0 = 1;           /* LED aceso                       */
        __delay_ms(500);             /* 500 ms                          */
        PORTCbits.RC0 = 0;           /* LED apagado                     */
        __delay_ms(500);             /* 500 ms  → período 1 s = 1 Hz    */
    }

    uint8_t seg_anterior   = 0xFFu;  /* força 1º redesenho            */
    uint8_t cont_tela      = 0;      /* segundos na tela corrente     */
    bool    mostrando_hora = true;   /* qual tela está ativa          */
    uint8_t cont_sht       = SHT_PERIODO_SEGUNDOS; /* mede já no boot */
    uint8_t cont_msg_sync  = 0;      /* aviso de sincronização        */
    uint8_t acerto[USB_TAM_REPORT];
    uint8_t estado[USB_TAM_REPORT];  /* report montado a cada tick    */

    /* ---- inicialização de hardware -------------------------------- */
    board_iniciar_pinos();
    uart_iniciar();
    i2c_iniciar_barramento();
    sht1x_reset_comunicacao();

    /* USB primeiro (com GIE ainda apagado), depois interrupções: o
     * host começa a enumerar em paralelo com o resto do boot.        */
    usb_iniciar();
    INTCONbits.PEIE = 1;
    INTCONbits.GIE  = 1;

    /* O display exige 500 ms de power-up (a espera está dentro de
     * vfd_iniciar); a enumeração USB acontece durante essa espera.   */
    vfd_iniciar();
    vfd_brilho(BRILHO_PADRAO);
    vfd_linha(0, "       VFDCLK");
    vfd_linha(1, "  PIC16C745 v1.0");

    hora_valida = ds3231_hora_valida();
    __delay_ms(1000);                /* splash rapidinho              */

    /* ---- laço principal ------------------------------------------- */
    for (;;) {
        /* 1) Acerto de hora vindo do PC? (o stack USB deixa no
         *    "correio"; consumimos aqui, fora da ISR)                */
        if (usb_pegar_acerto(acerto)) {
            if (acerto_valido(acerto)) {
                ds3231_hora_t nova;

                nova.segundos   = acerto[1];
                nova.minutos    = acerto[2];
                nova.horas      = acerto[3];
                nova.dia_semana = acerto[4];
                nova.dia        = acerto[5];
                nova.mes        = acerto[6];
                nova.ano        = acerto[7];

                if (ds3231_gravar(&nova)) {
                    hora_valida   = true;
                    cont_msg_sync = MSG_SYNC_SEGUNDOS;
                    tela_sincronizada();
                    seg_anterior  = 0xFFu;   /* redesenha ao sair     */
                }
            }
        }

        /* 2) Lê o RTC e detecta a virada do segundo                  */
        rtc_presente = ds3231_ler(&hora_atual);

        if (rtc_presente && (hora_atual.segundos != seg_anterior)) {
            seg_anterior = hora_atual.segundos;

            /* 2a) Medição periódica do SHT15 (bloqueia ~0,4 s; USB
             *     continua vivo pela ISR)                            */
            cont_sht++;
            if (cont_sht >= SHT_PERIODO_SEGUNDOS) {
                cont_sht = 0;
                if (sht1x_medir(&medida)) {
                    ha_medida = true;
                }
                /* Em falha mantém a última medida boa; 'ha_medida'
                 * só é falso até a primeira leitura bem-sucedida.    */
            }

            /* 2b) Enquanto o aviso de sincronização está na tela,
             *     só decrementa o contador                           */
            if (cont_msg_sync != 0u) {
                cont_msg_sync--;
                montar_estado_usb(estado);
                usb_atualizar_estado(estado);
                continue;
            }

            /* 2c) Alternância das telas                              */
            cont_tela++;
            if (mostrando_hora &&
                (cont_tela >= TELA_HORA_SEGUNDOS)) {
                mostrando_hora = false;
                cont_tela = 0;
            } else if (!mostrando_hora &&
                       (cont_tela >= TELA_CLIMA_SEGUNDOS)) {
                mostrando_hora = true;
                cont_tela = 0;
            }

            /* 2d) Desenho                                            */
            if (mostrando_hora) {
                if (hora_valida) {
                    tela_hora();
                } else {
                    tela_ajuste_pendente();
                }
            } else {
                tela_clima();
            }

            /* 2e) Report USB sempre fresco para o PC                 */
            montar_estado_usb(estado);
            usb_atualizar_estado(estado);
        } else if (!rtc_presente && (seg_anterior != 0xFEu)) {
            /* RTC sumiu do barramento: mostra o aviso uma única vez  */
            seg_anterior = 0xFEu;
            tela_hora();
        }

        /* 3) Cadência do laço: ~20 leituras de RTC por segundo dão
         *    precisão de ±50 ms na virada do segundo exibido.        */
        __delay_ms(50);
    }
}
