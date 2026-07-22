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
 *     - alterna o carrossel automático (6 s hora / 4 s clima);
 *     - a cada 30 s dispara uma medição do SHT15 (bloqueante ~0,4 s;
 *       a ISR de USB continua atendendo o barramento normalmente);
 *     - atualiza o report de estado que o PC pode ler via USB.
 *   fora do tick, verifica os botões e se chegou acerto pelo USB.
 *
 *  INTERFACE DE BOTÕES (menu de configuração):
 *   - BOTÃO 1 (RA0) = NAVEGAR: abre o menu e percorre as opções
 *                     (por ora: ALARME e BRILHO). Nunca altera valor.
 *   - BOTÃO 2 (RA2) = ALTERAR: muda o valor da opção mostrada
 *                     (alarme liga/desliga; brilho sobe até o máximo e
 *                     volta ao mínimo). Fora do menu, o 1º toque já abre
 *                     no alarme e alterna, atendendo o gesto mais comum.
 *   Sem tocar em botão por alguns segundos, o menu fecha sozinho e o
 *   carrossel volta. Com o alarme TOCANDO, qualquer botão silencia.
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
#define SHT_PERIODO_SEGUNDOS  30u  /* intervalo entre medições        */
#define MSG_SYNC_SEGUNDOS     3u   /* aviso "hora sincronizada"       */
#define ALARME_MAX_SEGUNDOS   60u  /* silencia sozinho após ~1 min    */
#define REFRESCO_SEGUNDOS     900u /* redesenho completo de auto-correção
                                    * (15 min): repõe cursor e reescreve
                                    * a tela toda, curando qualquer
                                    * desalinhamento por ruído          */
#define CONFIG_TIMEOUT_SEGUNDOS 8u /* menu fecha sozinho após ocioso   */
#define BRILHO_PADRAO         VFD_BRILHO_MAXIMO

/* Bipe do alarme e pisca sincronizado. Base de tempo REAL vinda do TMR1
 * (não das voltas do laço), então o ritmo não "engasga" quando uma volta
 * demora mais. Período de 500 ms; buzzer e texto LIGADOS nos primeiros
 * 200 ms de cada período (200 ms ON / 300 ms OFF).
 *
 * O TMR1 usa o pino RC0 (T1OSO/T1CKI); por isso o LED de heartbeat foi
 * movido para RA2 (ver board.h) — ligar o TMR1 tirava o LED do RC0.     */
#define ALARME_PERIODO_MS   500u
#define ALARME_BIPE_ON_MS   200u

/* TMR1 livre: clock = Fosc/4 = 6 MHz, prescaler 1:8 -> 750 kHz -> 750
 * ticks por milissegundo. Overflow do contador de 16 bits em ~87 ms,
 * bem acima da volta do laço (~50 ms), então o delta por volta é sempre
 * capturado corretamente com uma subtração de 16 bits.                 */
#define TMR1_TICKS_POR_MS   750u

/* Telas do CARROSSEL automático. Só hora e clima se alternam sozinhas;
 * o alarme SAIU do rodízio — agora é uma opção do menu de configuração
 * (ver tela_config e o tratamento dos botões no laço principal).      */
#define TELA_HORA        0u
#define TELA_CLIMA       1u
#define CARROSSEL_TELAS  2u

/* Opções do menu de configuração (botão 1 escolhe, botão 2 altera).
 * Estender é simples: acrescente uma opção aqui, trate-a no botão 2 e
 * em tela_config(), e aumente CFG_NUM_OPCOES.                         */
#define CFG_ALARME       0u
#define CFG_BRILHO       1u
#define CFG_NUM_OPCOES   2u

/* Sombra do latch de PORTB — ver explicação em board.h               */
volatile uint8_t portb_sombra;

/* ------------------------------------------------------------------
 * Inicialização dos pinos (declarada em board.h)
 * ------------------------------------------------------------------ */
void board_iniciar_pinos(void)
{
    /* PORTA: RA0 = botão 1 (navegar), RA1 = botão 2 (alterar), ambos
     * entradas para GND com pull-up externo de 10k. RA2 = LED de
     * heartbeat (SAÍDA). ADCON1 com PCFG=0b111 põe TODOS os canais em
     * modo digital — indispensável para ler RA0/RA1 como entradas e
     * acionar RA2 como saída digital (ver seção 11 do DS41124D).      */
    ADCON1 = 0x07;
    LED_HB_DESLIGAR();                   /* RA2=0 no latch antes de saída */
    TRISA  = (uint8_t)~LED_HB_MASCARA;   /* RA2 saída; RA0/RA1 entradas   */

    /* PORTB:
     *  RB5 (SDA) e RB2 (DATA SHT): latch 0, iniciam SOLTOS (TRIS=1)
     *  RB4 (SCL) e RB3 (SCK)     : saídas push-pull, iniciam em nível
     *                              de repouso (SCL=1 alto, SCK=0 baixo)
     *  RB0 (INT/SQW do DS3231)   : ENTRADA (INT0; uso futuro, ver board.h)
     *  RB1, RB6, RB7             : entradas (RB1 livre; RB6/7 = ICSP)  */
    portb_sombra = I2C_SCL_MASCARA;      /* SCL=1, SCK=0, SDA=DATA=0 */
    PORTB_APLICAR();
    TRISB = (uint8_t)~(I2C_SCL_MASCARA | SHT_SCK_MASCARA);
    /* = 0b11100111: SCL(RB4) e SCK(RB3) saídas; demais entradas —
     *   SDA/DATA soltos (open-drain), RB0=INT/SQW, RB1 livre, RB6/7 ICSP */

    /* PORTC: RC6 = TX (o USART assume o controle do pino quando
     * TXEN=1/SPEN=1); RC4/RC5 são do USB; RC2 = buzzer é a única saída.
     * RC0 (T1OSO/T1CKI) fica como ENTRADA, livre para o TMR1. Demais
     * como entrada. (O LED de heartbeat mudou para RA2 — ver board.h.) */
    BUZZER_DESLIGAR();                  /* estado definido ANTES de   */
                                        /* virar saída                */
    TRISC = (uint8_t)~BUZZER_MASCARA;   /* só RC2 (buzzer) saída; RC0
                                         * fica entrada, livre p/ o TMR1 */
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

/* Brilho corrente do display (hardware: 0 = máximo ... 7 = mínimo).
 * O PIC16C745 não tem EEPROM e o DS3231 não tem SRAM livre, então o
 * brilho NÃO é retido no desligamento: volta ao padrão a cada boot.   */
static uint8_t brilho_nivel = BRILHO_PADRAO;

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

/* Escreve os 2 dígitos de um valor BCD em p[0..1]                    */
static void por_bcd(char *p, uint8_t bcd)
{
    p[0] = (char)('0' + (bcd >> 4));
    p[1] = (char)('0' + (bcd & 0x0Fu));
}

/* Copia uma string para o buffer a partir da coluna 'col', sem passar
 * do fim da linha. Usada para montar as telas de forma legível.      */
static void por_texto(char *b, uint8_t col, const char *s)
{
    while ((*s != '\0') && (col < VFD_COLUNAS)) {
        b[col] = *s;
        col++;
        s++;
    }
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
 *
 * TODAS as telas terminam chamando vfd_quadro() (ou a variante
 * piscante): as duas linhas vão ao display num ÚNICO fluxo de 40
 * caracteres, sem posicionamento no meio — é o que faz a 2ª linha
 * funcionar de verdade (ver a nota grande em vfd.c/vfd_quadro).
 * A única exceção é a atualização por segundo da tela de hora, que
 * reescreve só o dígito que mudou (vfd_campo_bcd), toda na 1ª linha.
 * ------------------------------------------------------------------ */

/* Tela 1 — hora e data:  |      14:35:27      |
 *                        |   QUA 16/07/2026   |                      */
static void tela_hora(bool completo)
{
    char b0[VFD_COLUNAS + 1];
    char b1[VFD_COLUNAS + 1];
    const char *dia;

    /* Atualização incremental: reescreve SÓ os segundos (1 campo, na 1ª
     * linha) quando apenas eles mudaram — é o que elimina a piscada.
     *
     * IMPORTANTE (era o bug do ":" virando "0" na virada de minuto):
     * quando minuto/hora TAMBÉM mudam, sairiam DUAS posições 1Bh seguidas
     * (minutos + segundos) e o display, ocupado após a 1ª, derrubava um
     * byte e desalinhava os segundos por cima do ":". Por isso, se minuto
     * ou hora mudou, NÃO usamos incremental: caímos no redesenho completo
     * via vfd_quadro (uma posição só, 40 chars seguidos, sem drop). Isso
     * acontece no máximo 1×/minuto e não pisca (vfd_quadro sobrescreve). */
    if (!completo && rtc_presente
        && (hora_atual.horas   == exibido_horas)
        && (hora_atual.minutos == exibido_minutos)) {
        vfd_campo_bcd(0, 12, hora_atual.segundos, exibido_segundos);
        exibido_segundos = hora_atual.segundos;
        return;
    }

    /* Redesenho completo: o espelho passa a valer a partir de agora  */
    exibido_horas    = hora_atual.horas;
    exibido_minutos  = hora_atual.minutos;
    exibido_segundos = hora_atual.segundos;

    linha_limpa(b0);
    linha_limpa(b1);
    if (rtc_presente) {
        por_bcd(&b0[6], hora_atual.horas);
        b0[8] = ':';
        por_bcd(&b0[9], hora_atual.minutos);
        b0[11] = ':';
        por_bcd(&b0[12], hora_atual.segundos);

        if ((hora_atual.dia_semana >= 1u) && (hora_atual.dia_semana <= 7u)) {
            dia = nome_dia[hora_atual.dia_semana - 1u];
            b1[3] = dia[0]; b1[4] = dia[1]; b1[5] = dia[2];
        }
        por_bcd(&b1[7], hora_atual.dia);
        b1[9] = '/';
        por_bcd(&b1[10], hora_atual.mes);
        b1[12] = '/';
        b1[13] = '2'; b1[14] = '0';          /* século 20xx assumido    */
        por_bcd(&b1[15], hora_atual.ano);
    } else {
        /* RTC mudo no I2C: avisa em vez de mostrar lixo              */
        por_texto(b0, 6, "SEM RTC");
    }
    vfd_quadro(b0, b1);
}

/* Tela 2 — clima:  |  TEMP:   23.4 C    |
 *                  |  UMID:   45.2 %    |                            */
static void tela_clima(void)
{
    char b0[VFD_COLUNAS + 1];
    char b1[VFD_COLUNAS + 1];

    linha_limpa(b0);
    linha_limpa(b1);

    por_texto(b0, 2, "TEMP:");
    if (ha_medida) {
        por_decimos(&b0[13], medida.temperatura_dC);
    } else {
        b0[10] = '-'; b0[11] = '-'; b0[12] = '.'; b0[13] = '-';
    }
    b0[15] = 'C';

    por_texto(b1, 2, "UMID:");
    if (ha_medida) {
        por_decimos(&b1[13], (int16_t)medida.umidade_dRH);
    } else {
        b1[10] = '-'; b1[11] = '-'; b1[12] = '.'; b1[13] = '-';
    }
    b1[15] = '%';

    vfd_quadro(b0, b1);
}

/* Menu de configuração (botão 1 escolhe a opção, botão 2 altera).
 *   CFG_ALARME:  |  ALARME       07:00|
 *               |      ATIVADO       | / |     DESATIVADO     |
 *   CFG_BRILHO:  |   BRILHO DA TELA   |
 *               |      NIVEL 8       |  (8 = mais claro ... 1 = escuro)
 * "ATIVADO/DESATIVADO" (e não "LIGADO/DESLIGADO") deixa claro que se
 * refere ao ALARME habilitado, não à energia do aparelho.            */
static void tela_config(uint8_t opcao)
{
    char b0[VFD_COLUNAS + 1];
    char b1[VFD_COLUNAS + 1];
    uint8_t nivel;

    linha_limpa(b0);
    linha_limpa(b1);

    if (opcao == CFG_ALARME) {
        por_texto(b0, 2, "ALARME");
        por_bcd(&b0[14], alarme_horas);
        b0[16] = ':';
        por_bcd(&b0[17], alarme_minutos);
        if (alarme_ligado) {
            por_texto(b1, 6, "ATIVADO");
        } else {
            por_texto(b1, 5, "DESATIVADO");
        }
    } else {   /* CFG_BRILHO */
        por_texto(b0, 3, "BRILHO DA TELA");
        /* Mostra um nível "amigável" que SOBE com o brilho: 1 (mais
         * escuro) a 8 (mais claro). O hardware é o inverso (0 = máx.). */
        nivel = (uint8_t)(VFD_BRILHO_MINIMO + 1u - brilho_nivel);  /* 1..8 */
        por_texto(b1, 6, "NIVEL");
        b1[12] = (char)('0' + nivel);
    }

    vfd_quadro(b0, b1);
}

/* Tela do alarme DISPARADO:
 *   |     ALARME! HH:MM  |   <- pisca em sincronia com o bipe
 *   |   APERTE UM BOTAO   |   <- fixo (qualquer botão silencia)
 *
 * O pisca é feito POR FIRMWARE (não pelo atributo 31h/32h do display,
 * cujo relógio interno nunca casaria com o bipe): quando 'texto_visivel'
 * é falso, a 1ª linha sai em branco. O laço principal chama esta função
 * com 'texto_visivel' derivado da MESMA fase de tempo do buzzer, então o
 * texto pisca junto com o som. A 2ª linha ("APERTE UM BOTAO") não cabe
 * como "aperte qualquer botão" (21 > 20 colunas), mas diz o mesmo.     */
static void tela_alarme_tocando(bool texto_visivel)
{
    char b0[VFD_COLUNAS + 1];
    char b1[VFD_COLUNAS + 1];

    linha_limpa(b0);
    linha_limpa(b1);

    if (texto_visivel) {
        por_texto(b0, 3, "ALARME!");
        por_bcd(&b0[11], alarme_horas);
        b0[13] = ':';
        por_bcd(&b0[14], alarme_minutos);
    }
    por_texto(b1, 2, "APERTE UM BOTAO");

    vfd_quadro(b0, b1);
}

/* Aviso enquanto o RTC não tem hora confiável (OSF setado / nunca
 * sincronizado). NÃO detecta a conexão USB — sinaliza que a HORA está
 * inválida; a ação do usuário é rodar o app de acerto (DTCAPP). Por
 * isso a mensagem fala em "atualizar", não em "conectar".            */
static void tela_ajuste_pendente(void)
{
    vfd_quadro("ATUALIZE HORA E DATA", "      PELO USB");
}

/* Confirmação após receber acerto pelo USB                           */
static void tela_sincronizada(void)
{
    vfd_quadro("  HORA SINCRONIZADA", "      VIA USB");
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
    uint8_t seg_anterior   = 0xFFu;  /* força 1º redesenho            */
    uint8_t cont_tela      = 0;      /* segundos na tela corrente     */
    uint8_t tela           = TELA_HORA;            /* tela ativa      */
    uint8_t cont_sht       = SHT_PERIODO_SEGUNDOS; /* mede já no boot */
    uint8_t cont_msg_sync  = 0;      /* aviso de sincronização        */
    uint16_t tmr1_ant      = 0;      /* última leitura do TMR1 (base tempo) */
    uint16_t acum_ticks    = 0;      /* ticks do TMR1 acumulados (<750/ms)  */
    uint16_t fase_ms       = 0;      /* fase 0..499 ms do bipe/pisca alarme */
    bool    redesenhar     = false;  /* forçado por botão/comando     */
    uint8_t tela_desenhada = 0xFFu;  /* qual tela está pintada        */
    uint8_t hb_contador    = 0;      /* divisor do heartbeat (LED)    */
    bool    hb_estado      = false;  /* estado atual do LED           */
    uint16_t cont_refresco = 0;      /* segundos até o refresco total */
    bool    alarme_desenhado = false;/* tela de alarme já pintada     */
    bool    alarme_visivel   = false;/* estado atual do pisca (texto on?) */
    bool    modo_config    = false;  /* menu de configuração aberto?  */
    uint8_t opcao_config   = CFG_ALARME;  /* opção mostrada no menu   */
    uint8_t cont_config    = 0;      /* segundos ocioso no menu        */
    bool    btn1;                    /* botão 1 (navegar) tocado?     */
    bool    btn2;                    /* botão 2 (alterar) tocado?     */
    uint8_t acerto[USB_TAM_REPORT];
    uint8_t estado[USB_TAM_REPORT];  /* report montado a cada tick    */

    /* Duração de cada tela no carrossel automático (índice = tela)   */
    static const uint8_t tela_duracao[CARROSSEL_TELAS] = {
        TELA_HORA_SEGUNDOS, TELA_CLIMA_SEGUNDOS
    };

    /* ---- inicialização de hardware -------------------------------- */
    board_iniciar_pinos();
    uart_iniciar();
    i2c_iniciar_barramento();
    sht1x_reset_comunicacao();

    /* TMR1 como contador LIVRE (sem interrupção), base de tempo real do
     * bipe/pisca do alarme. T1CON: TMR1CS=0 (clock Fosc/4), T1OSCEN=0
     * (oscilador desligado), T1CKPS=11 (prescaler 1:8), TMR1ON=1 -> 0x31.
     * Usa o pino RC0 (por isso o LED foi para RA2 — ver board.h).       */
    T1CON = 0x31u;

    /* USB primeiro (com GIE ainda apagado), depois interrupções: o
     * host começa a enumerar em paralelo com o resto do boot.        */
    usb_iniciar();
    INTCONbits.PEIE = 1;
    INTCONbits.GIE  = 1;

    /* O display exige 500 ms de power-up (a espera está dentro de
     * vfd_iniciar); a enumeração USB acontece durante essa espera.   */
    vfd_iniciar();
    /* Restaura o brilho salvo (área de config do DS3231); se nunca foi
     * gravado, brilho_nivel mantém o padrão (BRILHO_PADRAO).          */
    {
        uint8_t b;
        if (ds3231_config_ler(&b)) {
            brilho_nivel = b;
        }
    }
    vfd_brilho(brilho_nivel);
    vfd_quadro("       VFDCLK", "      VER. 1.0");

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
         *    por isso botoes_processar() vem SEMPRE, uma vez por volta.
         *    Qualquer toque (curto ou longo) conta como um "clique".  */
        botoes_processar();
        btn1 = (botao_evento(BTN_TELA)   != BTN_NADA);
        btn2 = (botao_evento(BTN_ALARME) != BTN_NADA);

        /* 0a) Alarme TOCANDO: qualquer botão silencia (e rearma para
         *     amanhã). É o gesto natural — ouviu, apertou, parou.     */
        if (alarme_tocando && (btn1 || btn2)) {
            alarme_tocando = false;
            (void)ds3231_alarme_reconhecer();
            BUZZER_DESLIGAR();
            redesenhar = true;
            btn1 = false;                 /* consumidos pelo silenciar  */
            btn2 = false;
        }

        /* 0b) Botão 1 — NAVEGAR: abre o menu ou avança a opção. Nunca
         *     altera valor.                                            */
        if (btn1) {
            if (!modo_config) {
                modo_config  = true;
                opcao_config = CFG_ALARME;
            } else {
                opcao_config = (uint8_t)((opcao_config + 1u) % CFG_NUM_OPCOES);
            }
            cont_config = 0;
            redesenhar  = true;
        }

        /* 0c) Botão 2 — ALTERAR o valor da opção atual. Se o menu ainda
         *     não estava aberto, abre na 1ª opção (alarme) e já alterna,
         *     dando resposta imediata ao gesto mais comum.            */
        if (btn2) {
            if (!modo_config) {
                modo_config  = true;
                opcao_config = CFG_ALARME;
            }
            if (opcao_config == CFG_ALARME) {
                alarme_ligado = !alarme_ligado;
                (void)ds3231_alarme_habilitar(alarme_ligado);
            } else {   /* CFG_BRILHO: sobe o brilho; do máximo, volta ao
                        * mínimo. Hardware 0 = máx.; subir brilho = N--. */
                if (brilho_nivel == VFD_BRILHO_MAXIMO) {
                    brilho_nivel = VFD_BRILHO_MINIMO;
                } else {
                    brilho_nivel--;
                }
                vfd_brilho(brilho_nivel);
                /* Persiste na área de config do DS3231 (só quando muda,
                 * não a cada volta) para sobreviver ao desligamento.   */
                (void)ds3231_config_gravar(brilho_nivel);
            }
            cont_config = 0;
            redesenhar  = true;
        }

        /* 0d) Base de tempo REAL a partir do TMR1 livre. A fase anda
         *     conforme o tempo de VERDADE decorrido (delta de ticks do
         *     TMR1), não conforme as voltas do laço — então o bipe não
         *     "engasga" quando uma volta demora mais. Leitura em laço
         *     (reamostra enquanto o byte alto mudar) para não pegar o
         *     contador no meio de um rollover.                          */
        {
            uint8_t  hi, lo;
            uint16_t agora;
            uint16_t delta;

            do {
                hi = TMR1H;
                lo = TMR1L;
            } while (hi != TMR1H);
            agora = (uint16_t)(((uint16_t)hi << 8) | lo);
            delta = (uint16_t)(agora - tmr1_ant);   /* tolera 1 overflow */
            tmr1_ant = agora;

            acum_ticks = (uint16_t)(acum_ticks + delta);
            while (acum_ticks >= TMR1_TICKS_POR_MS) {
                acum_ticks = (uint16_t)(acum_ticks - TMR1_TICKS_POR_MS);
                if (++fase_ms >= ALARME_PERIODO_MS) {
                    fase_ms = 0;
                }
            }
        }

        /* 0e) Bipe + pisca do alarme, ambos derivados da MESMA fase:
         *     LIGADOS nos primeiros ALARME_BIPE_ON_MS de cada período
         *     (200 ms ON / 300 ms OFF). Como texto e som saem da mesma
         *     base de tempo, piscam juntos. A tela do alarme é desenhada
         *     AQUI (não no tick de 1 s), pois pisca mais rápido que isso.
         *     Fora do alarme: NÃO mexe no pino do buzzer toda volta (já
         *     foi desligado ao silenciar) — evita read-modify-write no
         *     PORTC à toa.                                              */
        if (alarme_tocando) {
            bool visivel = (fase_ms < ALARME_BIPE_ON_MS);

            if (visivel) { BUZZER_LIGAR(); } else { BUZZER_DESLIGAR(); }

            if (!alarme_desenhado || (visivel != alarme_visivel)) {
                tela_alarme_tocando(visivel);
                alarme_visivel   = visivel;
                alarme_desenhado = true;
            }
        } else {
            alarme_desenhado = false;   /* força repintar a tela de baixo */
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
                modo_config  = true;         /* mostra o alarme como    */
                opcao_config = CFG_ALARME;   /* confirmação             */
                cont_config  = 0;
                redesenhar   = true;
            } else if (acerto[0] == USB_CMD_LIGA_ALARME) {
                alarme_ligado = (acerto[1] != 0u);
                (void)ds3231_alarme_habilitar(alarme_ligado);
                modo_config  = true;
                opcao_config = CFG_ALARME;
                cont_config  = 0;
                redesenhar   = true;
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
                /* Reinicia a fase do bipe/pisca para o 1º bipe sair
                 * completo (200 ms), em vez de começar no meio do período. */
                fase_ms    = 0;
                acum_ticks = 0;
                /* O alarme toma a tela: fecha um menu meio-editado e zera
                 * um aviso de sync pendente, para o retorno (ao silenciar)
                 * ser limpo ao relógio. Os botões só silenciam enquanto
                 * toca (bloco 0a), então nada é alterado por engano.     */
                modo_config   = false;
                cont_config   = 0;
                cont_msg_sync = 0;
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
                    redesenhar     = true;   /* força repintar JÁ (senão a
                                              * tela ALARME! ficava presa) */
                }
            }

            /* 2c) Medição periódica do SHT15 (bloqueia ~0,4 s; USB
             *     continua vivo pela ISR). Enquanto o alarme TOCA, adia:
             *     o bloqueio de 0,4 s congelaria o PISCA do display por
             *     esse tempo (o bipe já é imune, regido pelo TMR1). Mede
             *     assim que o alarme parar; a temperatura não muda no
             *     ~1 min de alarme.                                      */
            cont_sht++;
            if (cont_sht >= SHT_PERIODO_SEGUNDOS) {
                if (alarme_tocando) {
                    cont_sht = SHT_PERIODO_SEGUNDOS;  /* segura sem estourar */
                } else {
                    cont_sht = 0;
                    if (sht1x_medir(&medida)) {
                        ha_medida  = true;
                        redesenhar = true;   /* valores novos na tela clima */
                    }
                    /* Em falha mantém a última medida boa; 'ha_medida'
                     * só é falso até a primeira leitura bem-sucedida.    */
                }
            }

            /* 2d) Desenho da tela quando o alarme NÃO está tocando —
             *     se estiver, o bloco 0e já cuida da tela do alarme (com
             *     prioridade). Ordem: aviso de sync > menu de config >
             *     carrossel automático (hora/clima).                   */
            if (!alarme_tocando) {
                if (cont_msg_sync != 0u) {
                    cont_msg_sync--;         /* mantém o aviso de sync */
                    if (cont_msg_sync == 0u) {
                        tela_desenhada = 0xFFu;  /* repinta ao voltar  */
                    }
                } else if (modo_config) {
                    /* Menu aberto: conta o ocioso e fecha sozinho. O
                     * desenho é dirigido por evento (fast-path do botão);
                     * aqui só repinta se 'redesenhar' pediu.          */
                    if (++cont_config >= CONFIG_TIMEOUT_SEGUNDOS) {
                        modo_config    = false;
                        tela_desenhada = 0xFFu;  /* cai no carrossel abaixo */
                    } else if (redesenhar) {
                        tela_config(opcao_config);
                    }
                }

                /* Carrossel — quando não há aviso de sync nem menu (inclui
                 * o mesmo tick em que o menu acabou de expirar acima).   */
                if ((cont_msg_sync == 0u) && !modo_config) {
                    bool completo = redesenhar;

                    cont_tela++;
                    if (cont_tela >= tela_duracao[tela]) {
                        tela = (uint8_t)((tela + 1u) % CARROSSEL_TELAS);
                        cont_tela = 0;
                    }
                    if (tela != tela_desenhada) {
                        tela_desenhada = tela;
                        completo = true;     /* mudou de tela: repinta */
                    }

                    if (tela == TELA_CLIMA) {
                        if (completo) {
                            tela_clima();    /* valores mudam a cada 30 s */
                        }
                    } else if (hora_valida) {
                        tela_hora(completo); /* só os dígitos, se possível */
                    } else if (completo) {
                        tela_ajuste_pendente();
                    }
                }
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
            /* Botão/comando pediu redesenho fora do tick de 1 s:
             * responde na hora, para a resposta parecer instantânea.
             * (Se o alarme estiver tocando, quem desenha é o bloco 0e.) */
            redesenhar = false;
            if (!alarme_tocando) {
                if (modo_config) {
                    tela_config(opcao_config);
                } else {
                    tela_desenhada = tela;
                    if (tela == TELA_CLIMA) {
                        tela_clima();
                    } else if (hora_valida) {
                        tela_hora(true);
                    } else {
                        tela_ajuste_pendente();
                    }
                }
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
