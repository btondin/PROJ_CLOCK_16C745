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
#include "botoes.h"

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
#define TELA_ALARME_SEGUNDOS  3u   /* tempo exibindo o alarme         */
#define SHT_PERIODO_SEGUNDOS  30u  /* intervalo entre medições        */
#define MSG_SYNC_SEGUNDOS     3u   /* aviso "hora sincronizada"       */
#define ALARME_MAX_SEGUNDOS   61u /* silencia sozinho após 2 min     */
#define REFRESCO_SEGUNDOS     900u /* redesenho completo de auto-correção
                                    * (15 min): repõe cursor e reescreve
                                    * a tela toda, curando qualquer
                                    * desalinhamento por ruído          */
#define BRILHO_PADRAO         VFD_BRILHO_MAXIMO

/* Telas do carrossel (o botão 1 avança manualmente)                  */
#define TELA_HORA     0u
#define TELA_CLIMA    1u
#define TELA_ALARME   2u
#define TELA_QUANTAS  3u

/* Sombra do latch de PORTB — ver explicação em board.h               */
volatile uint8_t portb_sombra;

/* ------------------------------------------------------------------
 * Inicialização dos pinos (declarada em board.h)
 * ------------------------------------------------------------------ */
void board_iniciar_pinos(void)
{
    /* PORTA: entradas. RA0 = botão de tela, RA1 = botão de alarme
     * (ambos para GND, com pull-up externo de 10k).
     * ADCON1 com PCFG=0b111 põe TODOS os canais em modo digital —
     * indispensável para ler RA0/RA1 como entradas comuns
     * (ver seção 11 do datasheet DS41124D).                         */
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
     * TXEN=1/SPEN=1); RC4/RC5 são do USB; RC2 = buzzer e RC0 = LED de
     * heartbeat são as saídas. Demais como entrada.                  */
    BUZZER_DESLIGAR();                  /* estados definidos ANTES de */
    LED_HB_DESLIGAR();                  /* virarem saída              */
    TRISC = (uint8_t)~(BUZZER_MASCARA | LED_HB_MASCARA);
}

/* ------------------------------------------------------------------
 * Estado global do relógio
 * ------------------------------------------------------------------ */
static ds3231_hora_t  hora_atual;
static sht1x_medida_t medida;              /* última leitura do SHT15 */
static bool ha_medida    = false;          /* já houve leitura boa?   */
static bool rtc_presente = false;          /* DS3231 respondeu?       */
static bool hora_valida  = false;          /* OSF limpo no DS3231?    */

/* Estado do alarme. O horário e a habilitação VIVEM NO DS3231 (na
 * bateria); estas variáveis são só um espelho para exibir sem ter de
 * reler o RTC a cada redesenho.                                      */
static uint8_t alarme_horas    = 0x07u;    /* BCD (padrão 07:00)      */
static uint8_t alarme_minutos  = 0x00u;    /* BCD                     */
static bool    alarme_ligado   = false;    /* espelho do bit A1IE     */
static bool    alarme_tocando  = false;    /* disparou e não foi calado */
static uint8_t alarme_segundos = 0;        /* há quanto tempo toca    */

/* Espelho do que está ESCRITO na tela de hora, para atualizar só os
 * dígitos que mudarem (ver tela_hora). 0xFF = desconhecido, força a
 * primeira escrita.                                                  */
static uint8_t exibido_horas    = 0xFFu;
static uint8_t exibido_minutos  = 0xFFu;
static uint8_t exibido_segundos = 0xFFu;

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

/* ------------------------------------------------------------------
 * Heartbeat: pisca o LED de RC0 a ~1 Hz enquanto espera 'ms'
 * milissegundos. Usado nos trechos que ficam parados num delay (o
 * teste do VFD, o splash), para o LED continuar sinalizando vida.
 * O estado é 'static': o pisca continua de onde parou entre chamadas.
 * ------------------------------------------------------------------ */
static void esperar_piscando(uint16_t ms)
{
    static bool    led  = false;
    static uint8_t cont = 0;

    while (ms >= 50u) {
        if (++cont >= 10u) {           /* 10 x 50 ms = 500 ms -> 1 Hz */
            cont = 0;
            led = !led;
            if (led) { LED_HB_LIGAR(); } else { LED_HB_DESLIGAR(); }
        }
        __delay_ms(50);
        ms -= 50u;
    }
}

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
static void tela_hora(bool completo)
{
    char b[VFD_COLUNAS + 1];
    const char *dia;

    /* Atualização incremental: compara com o que JÁ ESTÁ na tela e
     * reescreve só os dígitos diferentes. De 10:23:41 para 10:23:42
     * muda um único caractere — os dois pontos e a data ficam
     * intocados. É isso que elimina a piscada.                       */
    if (!completo && rtc_presente) {
        vfd_campo_bcd(0, 6,  hora_atual.horas,    exibido_horas);
        vfd_campo_bcd(0, 9,  hora_atual.minutos,  exibido_minutos);
        vfd_campo_bcd(0, 12, hora_atual.segundos, exibido_segundos);
        exibido_horas    = hora_atual.horas;
        exibido_minutos  = hora_atual.minutos;
        exibido_segundos = hora_atual.segundos;
        return;
    }

    /* Redesenho completo: o espelho passa a valer a partir de agora  */
    exibido_horas    = hora_atual.horas;
    exibido_minutos  = hora_atual.minutos;
    exibido_segundos = hora_atual.segundos;

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

/* Tela 3 — alarme:  |     ALARME 07:00   |
 *                   |      ATIVADO       |
 * "ATIVADO/DESATIVADO" (e não "LIGADO/DESLIGADO") para deixar claro que
 * se refere ao ALARME estar habilitado, não à energia do aparelho.    */
static void tela_alarme(void)
{
    char b[VFD_COLUNAS + 1];

    linha_limpa(b);
    b[4] = 'A'; b[5] = 'L'; b[6] = 'A'; b[7] = 'R'; b[8] = 'M'; b[9] = 'E';
    por_bcd(&b[11], alarme_horas);
    b[13] = ':';
    por_bcd(&b[14], alarme_minutos);
    vfd_linha(0, b);

    linha_limpa(b);
    if (alarme_ligado) {
        /* "ATIVADO" centralizado (7 letras)                          */
        b[7] = 'A'; b[8] = 'T'; b[9] = 'I'; b[10] = 'V';
        b[11] = 'A'; b[12] = 'D'; b[13] = 'O';
    } else {
        /* "DESATIVADO" centralizado (10 letras)                      */
        b[5] = 'D'; b[6] = 'E'; b[7] = 'S'; b[8] = 'A'; b[9] = 'T';
        b[10] = 'I'; b[11] = 'V'; b[12] = 'A'; b[13] = 'D'; b[14] = 'O';
    }
    vfd_linha(1, b);
}

/* Tela do alarme DISPARADO — a 1ª linha pisca usando o atributo do
 * próprio display (códigos 31h/32h), sem custo de tráfego serial.    */
static void tela_alarme_tocando(void)
{
    char b[VFD_COLUNAS + 1];

    linha_limpa(b);
    b[3] = '*'; b[4] = '*'; b[5] = '*';
    b[7] = 'A'; b[8] = 'L'; b[9] = 'A'; b[10] = 'R'; b[11] = 'M'; b[12] = 'E';
    b[14] = '*'; b[15] = '*'; b[16] = '*';
    vfd_piscar_inicio(VFD_BLINK_2HZ);
    vfd_linha(0, b);
    vfd_piscar_fim();

    linha_limpa(b);
    por_bcd(&b[5], alarme_horas);
    b[7] = ':';
    por_bcd(&b[8], alarme_minutos);
    b[12] = '('; b[13] = 'B'; b[14] = 'T'; b[15] = 'N'; b[16] = '2'; b[17] = ')';
    vfd_linha(1, b);
}

/* Aviso enquanto o RTC não tem hora confiável (OSF setado / nunca
 * sincronizado). NÃO detecta a conexão USB — sinaliza que a HORA está
 * inválida; a ação do usuário é rodar o app de acerto (DTCAPP). Por
 * isso a mensagem fala em "atualizar", não em "conectar".            */
static void tela_ajuste_pendente(void)
{
    vfd_linha(0, "ATUALIZE HORA E DATA");
    vfd_linha(1, "      PELO USB");
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
    if (alarme_ligado) {
        r[0] |= USB_FLAG_ALARME_ON;
    }
    if (alarme_tocando) {
        r[0] |= USB_FLAG_ALARME_TOCA;
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
     *  TESTE DE PREENCHIMENTO DO VFD — REMOVER PARA USO NORMAL
     * ----------------------------------------------------------------
     *  Exercita SÓ o caminho PIC -> USART -> SP232 -> display, sem
     *  RTC, USB, alarme ou botões. Reproduz, a partir do PIC, o mesmo
     *  que funcionou no teste manual pelo PL2303: limpa a tela (0x15) e
     *  escreve padrões conhecidos nas duas linhas, repetindo a cada 3 s.
     *
     *  Como ler o resultado:
     *   - padrões LIMPOS e nas posições certas -> o driver do display
     *     está OK; o "quebrado" vem da lógica de cima (relógio/RTC) ou
     *     do software-reset 0x14;
     *   - padrões EMBARALHADOS -> é baud/temporização do USART (conferir
     *     o cristal de 24 MHz e o SPBRG = 77);
     *   - cada redesenho (a cada 3 s) DEVE ser idêntico ao anterior; se
     *     variar, há bytes sendo perdidos (temporização).
     *
     *  De propósito NÃO chama vfd_iniciar(): usa só 0x15 (clear) e 0x0E
     *  (cursor off), SEM o software-reset 0x14 — assim, se o vilão for o
     *  0x14 (ex.: jumper de self-test), este teste passa e o aponta.
     *
     *  A guarda 'volatile' faz um while(1) que preserva a alocação de
     *  RAM do resto do main (ver histórico: mesmo truque do heartbeat).
     * ================================================================ */
    {
        static volatile uint8_t teste_vfd = 0u;
        uint8_t i;

        board_iniciar_pinos();
        uart_iniciar();
        esperar_piscando(500);           /* power-up de 500 ms do VFD  */
        UART_TX(0x0Eu);                  /* cursor invisível           */

        while (teste_vfd) {
            /* Padrão A — régua de posições: dígitos em cima, letras
             * embaixo. Mostra se algum caractere cai fora do lugar.   */
            UART_TX(0x15u);                          /* clear + home   */
            __delay_ms(5);
            for (i = 0; i < VFD_COLUNAS; i++) {
                UART_TX((uint8_t)('0' + (i % 10u))); /* 0123456789...  */
            }
            UART_TX(0x1Bu);
            UART_TX(VFD_COLUNAS);                    /* -> linha 1, col 0 */
            __delay_ms(3);                           /* respiro pós-posição */
            for (i = 0; i < VFD_COLUNAS; i++) {
                UART_TX((uint8_t)('A' + i));         /* ABCDE...T      */
            }
            esperar_piscando(3000);

            /* Padrão B — 'U' (0x55, bits alternados = pior caso de
             * baud) em cima; '8' (acende quase todos os pontos) embaixo. */
            UART_TX(0x15u);
            __delay_ms(5);
            for (i = 0; i < VFD_COLUNAS; i++) {
                UART_TX('U');
            }
            UART_TX(0x1Bu);
            UART_TX(VFD_COLUNAS);
            __delay_ms(3);                           /* respiro pós-posição */
            for (i = 0; i < VFD_COLUNAS; i++) {
                UART_TX('8');
            }
            esperar_piscando(3000);
        }
    }

    uint8_t seg_anterior   = 0xFFu;  /* força 1º redesenho            */
    uint8_t cont_tela      = 0;      /* segundos na tela corrente     */
    uint8_t tela           = TELA_HORA;            /* tela ativa      */
    uint8_t cont_sht       = SHT_PERIODO_SEGUNDOS; /* mede já no boot */
    uint8_t cont_msg_sync  = 0;      /* aviso de sincronização        */
    uint8_t buzzer_fase    = 0;      /* padrão intermitente do buzzer */
    bool    redesenhar     = false;  /* forçado por botão             */
    uint8_t tela_desenhada = 0xFFu;  /* qual tela está pintada        */
    uint8_t hb_contador    = 0;      /* divisor do heartbeat (LED)    */
    bool    hb_estado      = false;  /* estado atual do LED           */
    uint16_t cont_refresco = 0;      /* segundos até o refresco total */
    bool    alarme_desenhado = false;/* tela de alarme já pintada     */
    btn_evento_t evento;
    uint8_t acerto[USB_TAM_REPORT];
    uint8_t estado[USB_TAM_REPORT];  /* report montado a cada tick    */

    /* Duração de cada tela no carrossel automático (índice = tela)   */
    static const uint8_t tela_duracao[TELA_QUANTAS] = {
        TELA_HORA_SEGUNDOS, TELA_CLIMA_SEGUNDOS, TELA_ALARME_SEGUNDOS
    };

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
    vfd_linha(1, "      VER. 1.0");

    hora_valida = ds3231_hora_valida();
    /* Recupera o alarme guardado na bateria do RTC (o PIC não tem
     * EEPROM, então o DS3231 é a única memória não-volátil do
     * projeto — ver comentário em ds3231.h).                         */
    (void)ds3231_alarme_ler(&alarme_horas, &alarme_minutos, &alarme_ligado);
    /* Descarta um disparo antigo: o RTC continua casando o horário na
     * bateria com o aparelho desligado, então A1F pode estar setado de
     * um alarme que já passou. Sem isto, ligar o relógio depois da
     * hora do alarme faria ele tocar imediatamente.                  */
    (void)ds3231_alarme_reconhecer();
    __delay_ms(1000);                /* splash rapidinho              */

    /* ---- laço principal ------------------------------------------- */
    for (;;) {
        /* 0) Botões: o debounce conta voltas do laço (~50 ms cada),
         *    por isso botoes_processar() vem SEMPRE, uma vez por volta */
        botoes_processar();

        /* 0a) Botão 1 — avança a tela na hora, reiniciando o rodízio  */
        if (botao_evento(BTN_TELA) != BTN_NADA) {
            tela = (uint8_t)((tela + 1u) % TELA_QUANTAS);
            cont_tela  = 0;
            redesenhar = true;
        }

        /* 0b) Botão 2 — curto silencia; longo liga/desliga o alarme   */
        evento = botao_evento(BTN_ALARME);
        if (evento == BTN_CURTO) {
            if (alarme_tocando) {
                alarme_tocando = false;      /* cala e rearma p/ amanhã */
                (void)ds3231_alarme_reconhecer();
                BUZZER_DESLIGAR();
                redesenhar = true;
            }
        } else if (evento == BTN_LONGO) {
            alarme_ligado = !alarme_ligado;
            (void)ds3231_alarme_habilitar(alarme_ligado);
            if (!alarme_ligado && alarme_tocando) {
                alarme_tocando = false;      /* desligar também cala    */
                (void)ds3231_alarme_reconhecer();
                BUZZER_DESLIGAR();
            }
            tela       = TELA_ALARME;        /* mostra o novo estado    */
            cont_tela  = 0;
            redesenhar = true;
        }

        /* 0c) Padrão sonoro: 200 ms ligado / 300 ms desligado         */
        if (alarme_tocando) {
            buzzer_fase = (uint8_t)((buzzer_fase + 1u) % 10u);
            if (buzzer_fase < 4u) {
                BUZZER_LIGAR();
            } else {
                BUZZER_DESLIGAR();
            }
        } else {
            buzzer_fase = 0;
        }

        /* 1) Comando vindo do PC (o stack USB deixa no "correio";
         *    consumimos aqui, fora da ISR)                            */
        if (usb_pegar_acerto(acerto)) {
            if ((acerto[0] == USB_CMD_ACERTAR_RTC) && acerto_valido(acerto)) {
                ds3231_hora_t nova;

                nova.segundos   = acerto[1];
                nova.minutos    = acerto[2];
                nova.horas      = acerto[3];
                nova.dia_semana = acerto[4];
                nova.dia        = acerto[5];
                nova.mes        = acerto[6];
                nova.ano        = acerto[7];

                if (ds3231_gravar(&nova)) {
                    hora_valida    = true;
                    cont_msg_sync  = MSG_SYNC_SEGUNDOS;
                    tela_sincronizada();
                    seg_anterior   = 0xFFu;  /* redesenha ao sair     */
                    tela_desenhada = 0xFFu;
                }
            } else if ((acerto[0] == USB_CMD_CONFIG_ALARME) &&
                       bcd_ok(acerto[1], 0x23u) &&
                       bcd_ok(acerto[2], 0x59u)) {
                alarme_horas   = acerto[1];
                alarme_minutos = acerto[2];
                alarme_ligado  = (acerto[3] != 0u);
                (void)ds3231_alarme_gravar(alarme_horas, alarme_minutos);
                (void)ds3231_alarme_habilitar(alarme_ligado);
                tela       = TELA_ALARME;    /* confirma na tela       */
                cont_tela  = 0;
                redesenhar = true;
            } else if (acerto[0] == USB_CMD_LIGA_ALARME) {
                alarme_ligado = (acerto[1] != 0u);
                (void)ds3231_alarme_habilitar(alarme_ligado);
                tela       = TELA_ALARME;
                cont_tela  = 0;
                redesenhar = true;
            }
        }

        /* 2) Lê o RTC e detecta a virada do segundo                  */
        rtc_presente = ds3231_ler(&hora_atual);

        if (rtc_presente && (hora_atual.segundos != seg_anterior)) {
            seg_anterior = hora_atual.segundos;

            /* 2·) Refresco periódico de AUTO-CORREÇÃO: a cada
             *     REFRESCO_SEGUNDOS reafirma o estado do display
             *     (cursor no início/invisível) e força um redesenho
             *     completo. Se um byte se perdeu por ruído e a tela
             *     "desalinhou", isto conserta sozinho, sem piscar
             *     (o conteúdo é reescrito por cima, não há clear).     */
            if (++cont_refresco >= REFRESCO_SEGUNDOS) {
                cont_refresco = 0;
                vfd_reafirmar();
                tela_desenhada   = 0xFFu;   /* força redesenho completo */
                exibido_horas    = 0xFFu;   /* e reescrita dos dígitos  */
                exibido_minutos  = 0xFFu;
                exibido_segundos = 0xFFu;
            }

            /* 2a) O alarme disparou? O flag A1F é LATCHED no DS3231,
             *     então basta consultá-lo 1x por segundo — nenhum
             *     disparo se perde e o pino INT/SQW fica dispensável. */
            if (alarme_ligado && !alarme_tocando &&
                ds3231_alarme_disparou()) {
                alarme_tocando  = true;
                alarme_segundos = 0;
            }

            /* 2b) Segurança: para de tocar sozinho após alguns
             *     minutos, caso não haja ninguém para apertar o botão */
            if (alarme_tocando) {
                alarme_segundos++;
                if (alarme_segundos >= ALARME_MAX_SEGUNDOS) {
                    alarme_tocando = false;
                    (void)ds3231_alarme_reconhecer();
                    BUZZER_DESLIGAR();
                    tela_desenhada = 0xFFu;  /* repinta ao voltar      */
                }
            }

            /* 2c) Medição periódica do SHT15 (bloqueia ~0,4 s; USB
             *     continua vivo pela ISR)                            */
            cont_sht++;
            if (cont_sht >= SHT_PERIODO_SEGUNDOS) {
                cont_sht = 0;
                if (sht1x_medir(&medida)) {
                    ha_medida  = true;
                    redesenhar = true;   /* valores novos na tela clima */
                }
                /* Em falha mantém a última medida boa; 'ha_medida'
                 * só é falso até a primeira leitura bem-sucedida.    */
            }

            /* 2d) Escolha e desenho da tela.
             * 'completo' controla se a tela é repintada por inteiro.
             * Repintar só quando algo estrutural muda (troca de tela,
             * nova medição, alarme) elimina a piscada de 1 em 1 s.   */
            if (alarme_tocando) {
                /* O alarme tem prioridade sobre todo o carrossel     */
                if (!alarme_desenhado) {
                    tela_alarme_tocando();
                    alarme_desenhado = true;
                }
            } else if (cont_msg_sync != 0u) {
                cont_msg_sync--;         /* mantém o aviso de sync     */
                if (cont_msg_sync == 0u) {
                    tela_desenhada = 0xFFu;  /* repinta ao voltar      */
                }
            } else {
                bool completo = redesenhar;

                /* Carrossel automático                               */
                cont_tela++;
                if (cont_tela >= tela_duracao[tela]) {
                    tela = (uint8_t)((tela + 1u) % TELA_QUANTAS);
                    cont_tela = 0;
                }
                if (tela != tela_desenhada) {
                    tela_desenhada = tela;
                    completo = true;     /* mudou de tela: repinta     */
                }

                if (tela == TELA_CLIMA) {
                    if (completo) {
                        tela_clima();    /* valores mudam a cada 30 s  */
                    }
                } else if (tela == TELA_ALARME) {
                    if (completo) {
                        tela_alarme();   /* estático até algo mudar    */
                    }
                } else if (hora_valida) {
                    tela_hora(completo); /* só os dígitos, se possível */
                } else if (completo) {
                    tela_ajuste_pendente();
                }
            }
            if (!alarme_tocando) {
                alarme_desenhado = false;
            }
            redesenhar = false;

            /* 2e) Report USB sempre fresco para o PC                 */
            montar_estado_usb(estado);
            usb_atualizar_estado(estado);
        } else if (!rtc_presente && (seg_anterior != 0xFEu)) {
            /* RTC sumiu do barramento: mostra o aviso uma única vez  */
            seg_anterior = 0xFEu;
            tela_hora(true);
        } else if (redesenhar) {
            /* Botão/comando pediu troca de tela fora do tick de 1 s:
             * redesenha na hora, para a resposta parecer instantânea */
            redesenhar = false;
            tela_desenhada = tela;
            if (alarme_tocando) {
                tela_alarme_tocando();
                alarme_desenhado = true;
            } else if (tela == TELA_CLIMA) {
                tela_clima();
            } else if (tela == TELA_ALARME) {
                tela_alarme();
            } else if (hora_valida) {
                tela_hora(true);
            } else {
                tela_ajuste_pendente();
            }
        }

        /* 3) Heartbeat: pisca o LED de RC0 a ~1 Hz. Como está atrelado
         *    à cadência do laço, ele também SINALIZA a saúde do laço —
         *    se travar (ex.: I2C preso), o LED congela e você vê.     */
        if (++hb_contador >= 10u) {   /* 10 x 50 ms = 500 ms -> 1 Hz  */
            hb_contador = 0;
            hb_estado = !hb_estado;
            if (hb_estado) { LED_HB_LIGAR(); } else { LED_HB_DESLIGAR(); }
        }

        /* 4) Cadência do laço: ~20 leituras de RTC por segundo dão
         *    precisão de ±50 ms na virada do segundo exibido.        */
        __delay_ms(50);
    }
}
