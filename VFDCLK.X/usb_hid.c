/**
 * =====================================================================
 *  usb_hid.c — Stack USB HID mínimo para o SIE do PIC16C745
 * =====================================================================
 *  Referências:
 *   - Datasheet PIC16C745/765 (DS41124D), seção 10 (módulo USB):
 *       registradores UIR/UIE/UEIR/UEIE/USTAT/UCTRL/UADDR/UEPn e a
 *       Buffer Descriptor Table (BDT) em 1A0h..1B7h + 40 bytes de
 *       RAM dual-port em 1B8h..1DFh (tabela 4-2).
 *   - USB 1.1, capítulo 9 (enumeração) e capítulo 5 (low-speed).
 *   - HID 1.11 (requisições de classe GET/SET_REPORT, SET_IDLE...).
 *
 *  VISÃO GERAL DO FUNCIONAMENTO
 *  ----------------------------
 *  O SIE (Serial Interface Engine) cuida sozinho da camada elétrica
 *  e de pacotes (NRZI, bit stuffing, CRC, ACK/NAK). O firmware só
 *  enxerga TRANSAÇÕES completas, via interrupção:
 *
 *   USB_RST  -> host reiniciou o barramento: zera endereço e prepara
 *               o endpoint 0 (UEP0 = 06h, exigência da seção 10.5.1.9)
 *   TOK_DNE  -> uma transação terminou: USTAT diz qual endpoint e
 *               direção; o Buffer Descriptor (BD) correspondente traz
 *               o PID (SETUP/OUT/IN) e a contagem de bytes.
 *
 *  Cada endpoint tem um BD de 4 bytes com semáforo UOWN:
 *   UOWN=0 -> BD/buffer pertencem ao firmware (SIE responde NAK)
 *   UOWN=1 -> BD/buffer pertencem ao SIE (firmware não pode tocar)
 *  O byte de status (BDnST) é sempre escrito INTEIRO (nunca BSF/BCF),
 *  como manda a advertência da seção 10.6 do datasheet.
 *
 *  TRANSFERÊNCIAS DE CONTROLE (EP0) — máquina de estados 'estado_ctl':
 *   SETUP(8 bytes) -> [dados IN em blocos de 8] -> status OUT (ZLP)
 *   SETUP(8 bytes) -> [dados OUT]               -> status IN  (ZLP)
 *   SETUP(8 bytes) ->                              status IN  (ZLP)
 *  Regras que o código respeita:
 *   - o 1º pacote de dados é sempre DATA1, alternando em seguida;
 *   - SET_ADDRESS só é efetivado DEPOIS do status (exigência do spec);
 *   - requisição desconhecida => STALL no EP0 (o host entende e
 *     recupera com o próximo SETUP);
 *   - quando o SIE recebe um SETUP ele levanta UCTRL.PKT_DIS,
 *     congelando o tráfego até o firmware limpar o bit (seção
 *     10.5.1.6) — isso é feito ao final de ep0_saida_processar().
 *
 *  BUFFERS na RAM dual-port (40 bytes disponíveis):
 *   1B8h..1BFh  EP0 OUT (recebe SETUP e dados OUT)
 *   1C0h..1C7h  EP0 IN  (respostas de controle)
 *   1C8h..1CFh  EP1 IN  (report de estado p/ o host)
 *  O acesso é feito por ponteiros absolutos (o endereço cabe no
 *  ponteiro genérico de 16 bits do XC8; FSR/IRP cuidam do resto).
 *
 *  NOTA SOBRE PROFUNDIDADE DE PILHA (importante neste PIC!)
 *  ---------------------------------------------------------
 *  O PIC16 midrange tem pilha de hardware de APENAS 8 níveis,
 *  compartilhada entre o main e a interrupção. Por isso este módulo
 *  é deliberadamente "achatado": a ISR chama no máximo UM nível de
 *  tratador (ep0_saida_processar / ep0_entrada_processar / ...), e
 *  esses tratadores chamam apenas funções-FOLHA (que não chamam
 *  nada). Pior caso: retorno-do-main + ISR + tratador + folha =
 *  4 níveis, somados aos <= 3 níveis usados pelo laço principal.
 *  Ao mexer neste arquivo, preserve essa propriedade.
 * =====================================================================
 */
#include <xc.h>
#include "board.h"
#include "usb_desc.h"
#include "usb_hid.h"

/* ------------------------------------------------------------------
 * Registradores e máscaras do módulo USB (datasheet, seção 10.5)
 * ------------------------------------------------------------------ */
/* UIR / UIE — flags e habilitação de interrupção do SIE              */
#define U_USB_RST   0x01u
#define U_UERR      0x02u
#define U_ACTIVITY  0x04u
#define U_TOK_DNE   0x08u
#define U_UIDLE     0x10u
#define U_STALL     0x20u

/* UCTRL                                                              */
#define UCTRL_SUSPND   0x02u
#define UCTRL_DEV_ATT  0x08u
#define UCTRL_PKT_DIS  0x10u

/* UEPn (bit3..bit0 = EP_CTL_DIS, EP_OUT_EN, EP_IN_EN, EP_STALL)      */
#define UEP_CONTROLE   0x06u    /* OUT+IN+SETUP (EP_CTL_DIS=0)        */
#define UEP_SOMENTE_IN 0x02u

/* Byte de status do BD (escrita pelo firmware)                       */
#define BD_UOWN     0x80u
#define BD_DATA1    0x40u
#define BD_DTS      0x08u
#define BD_BSTALL   0x04u

/* PID recebido (bits 5..2 do BD quando UOWN=0)                       */
#define BD_PID(st)  (uint8_t)(((st) >> 2) & 0x0Fu)
#define PID_SETUP   0x0Du
#define PID_OUT     0x01u
#define PID_IN      0x09u

/* USTAT: bits 4..3 = endpoint, bit 2 = direção (1=IN)                */
#define USTAT_EP(s)     (uint8_t)(((s) >> 3) & 0x03u)
#define USTAT_EH_IN(s)  (((s) & 0x04u) != 0u)

/* Buffers na RAM dual-port (tabela 4-2 do datasheet)                 */
#define EP0_SAIDA_END   0xB8u   /* byte baixo de 01B8h (p/ BD0OAL)    */
#define EP0_ENTRADA_END 0xC0u
#define EP1_ENTRADA_END 0xC8u
#define EP0_SAIDA    ((volatile uint8_t *)0x01B8)
#define EP0_ENTRADA  ((volatile uint8_t *)0x01C0)
#define EP1_ENTRADA  ((volatile uint8_t *)0x01C8)

/* ------------------------------------------------------------------
 * Requisições USB padrão (cap. 9) e de classe HID (HID 1.11, 7.2)
 * ------------------------------------------------------------------ */
#define REQ_GET_STATUS        0x00u
#define REQ_CLEAR_FEATURE     0x01u
#define REQ_SET_FEATURE       0x03u
#define REQ_SET_ADDRESS       0x05u
#define REQ_GET_DESCRIPTOR    0x06u
#define REQ_GET_CONFIGURATION 0x08u
#define REQ_SET_CONFIGURATION 0x09u
#define REQ_GET_INTERFACE     0x0Au
#define REQ_SET_INTERFACE     0x0Bu

#define DESC_DISPOSITIVO      0x01u
#define DESC_CONFIGURACAO     0x02u
#define DESC_STRING           0x03u
#define DESC_HID              0x21u
#define DESC_REPORT           0x22u

#define HID_GET_REPORT        0x01u
#define HID_GET_IDLE          0x02u
#define HID_GET_PROTOCOL      0x03u
#define HID_SET_REPORT        0x09u
#define HID_SET_IDLE          0x0Au
#define HID_SET_PROTOCOL      0x0Bu

/* Estados da transferência de controle no EP0                        */
enum {
    CTL_OCIOSO = 0,     /* esperando SETUP                            */
    CTL_TX_DADOS,       /* enviando dados ao host (etapa IN)          */
    CTL_RX_DADOS,       /* recebendo dados do host (etapa OUT)        */
    CTL_STATUS_IN,      /* ZLP IN de status pendente                  */
    CTL_STATUS_OUT      /* esperando ZLP OUT de status do host        */
};

/* ------------------------------------------------------------------
 * Estado do stack (tudo acessado pela ISR; 'volatile' onde o laço
 * principal também lê/escreve)
 * ------------------------------------------------------------------ */
static uint8_t setup_pkt[8];            /* cópia do último SETUP      */
static uint8_t estado_ctl;              /* máquina de estados do EP0  */
static const uint8_t *ctl_ptr;          /* dados IN restantes         */
static uint8_t  ctl_restante;           /* bytes ainda por enviar     */
static bool     ctl_zlp;                /* dados < wLength: ZLP final */
static bool     ctl_data1;              /* toggle DATA0/1 do EP0 IN   */
static uint8_t  endereco_pendente;      /* SET_ADDRESS adiado         */
static volatile uint8_t config_atual;   /* 0 = não configurado        */
static uint8_t  taxa_idle;              /* SET_IDLE (só armazenada)   */
static bool     ep1_data1;              /* próximo toggle do EP1 IN   */
static uint8_t  resposta_curta[2];      /* GET_STATUS & afins         */

/* Correio PC -> main: acerto de hora recebido por SET_REPORT         */
static volatile bool    ha_acerto;
static volatile uint8_t acerto_rx[USB_TAM_REPORT];

/* Cópia do report de estado (fonte para EP1 IN e GET_REPORT)         */
static volatile uint8_t relatorio_estado[USB_TAM_REPORT];

/* ==================================================================
 *  FUNÇÕES-FOLHA (não chamam nada — ver nota de pilha no cabeçalho)
 * ================================================================== */

/* Deixa o EP0 OUT pronto para receber um SETUP (sem DTS: aceita
 * qualquer toggle — SETUP é sempre DATA0 de todo modo).              */
static void ep0_armar_para_setup(void)
{
    BD0OBC = 8;
    BD0OAL = EP0_SAIDA_END;
    BD0OST = BD_UOWN;
}

/* Envia o próximo bloco (<= 8 bytes) da etapa de dados IN.           */
static void ep0_enviar_bloco(void)
{
    uint8_t n = (ctl_restante > 8u) ? 8u : ctl_restante;
    uint8_t i;

    for (i = 0; i < n; i++) {
        EP0_ENTRADA[i] = ctl_ptr[i];
    }
    ctl_ptr      += n;
    ctl_restante -= n;

    BD0IBC = n;
    BD0IAL = EP0_ENTRADA_END;
    BD0IST = (uint8_t)(BD_UOWN | BD_DTS | (ctl_data1 ? BD_DATA1 : 0u));
    ctl_data1 = !ctl_data1;
}

/* Agenda o ZLP IN da etapa de status (sempre DATA1, USB 1.1 8.5.2).  */
static void ep0_armar_status_in(void)
{
    BD0IBC = 0;
    BD0IAL = EP0_ENTRADA_END;
    BD0IST = (uint8_t)(BD_UOWN | BD_DATA1 | BD_DTS);
    estado_ctl = CTL_STATUS_IN;
}

/* Requisição inválida/não suportada: STALL nas duas direções do EP0.
 * O SIE responde STALL ao host; a recuperação acontece no tratamento
 * da interrupção U_STALL (re-arma o EP0) e no próximo SETUP.         */
static void ep0_stall(void)
{
    BD0IBC = 0;
    BD0IAL = EP0_ENTRADA_END;
    BD0IST = (uint8_t)(BD_UOWN | BD_BSTALL);
    BD0OBC = 8;
    BD0OAL = EP0_SAIDA_END;
    BD0OST = (uint8_t)(BD_UOWN | BD_BSTALL);
    estado_ctl = CTL_OCIOSO;
}

/* Registra a resposta de uma etapa de dados IN (não envia ainda!).
 * Respeita wLength (nunca envia mais do que o host pediu) e decide
 * se um ZLP final será necessário (resposta curta múltipla de 8).
 * O primeiro bloco é disparado pelo chamador (ver o "rabo" comum de
 * ep0_saida_processar), mantendo esta função como folha.             */
static void ep0_preparar_tx(const uint8_t *dados, uint8_t tam)
{
    uint16_t wlength = (uint16_t)setup_pkt[6] |
                       ((uint16_t)setup_pkt[7] << 8);

    if ((uint16_t)tam > wlength) {
        tam = (uint8_t)wlength;
        ctl_zlp = false;             /* resposta cortada: host sabe   */
    } else {
        ctl_zlp = ((uint16_t)tam < wlength) && ((tam & 7u) == 0u);
    }

    ctl_ptr      = dados;
    ctl_restante = tam;
    ctl_data1    = true;             /* 1º pacote de dados é DATA1    */
    estado_ctl   = CTL_TX_DADOS;
}

/* Recarrega o EP1 IN com o report de estado corrente.                */
static void ep1_armar(void)
{
    uint8_t i;

    for (i = 0; i < USB_TAM_REPORT; i++) {
        EP1_ENTRADA[i] = relatorio_estado[i];
    }
    BD1IBC = USB_TAM_REPORT;
    BD1IAL = EP1_ENTRADA_END;
    BD1IST = (uint8_t)(BD_UOWN | BD_DTS | (ep1_data1 ? BD_DATA1 : 0u));
    ep1_data1 = !ep1_data1;
}

/* ==================================================================
 *  TRATADORES (chamados pela ISR; só chamam funções-folha)
 * ================================================================== */

/* Reset de barramento (USB_RST): volta ao estado "Default".          */
static void reset_processar(void)
{
    UADDR = 0;                       /* endereço 0, como manda o spec */
    UEP0  = UEP_CONTROLE;            /* 06h (datasheet 10.5.1.10)     */
    UEP1  = 0;
    UEP2  = 0;

    BD0IST = 0;                      /* todos os BDs de volta ao MCU  */
    BD1OST = 0;
    BD1IST = 0;
    BD2OST = 0;
    BD2IST = 0;
    ep0_armar_para_setup();

    estado_ctl        = CTL_OCIOSO;
    config_atual      = 0;
    endereco_pendente = 0;
    ha_acerto         = false;

    UEIR  = 0;                       /* limpa flags de erro           */
    UCTRL &= (uint8_t)~UCTRL_PKT_DIS;
}

/* Transação concluída no EP0, direção OUT: pode ser um SETUP novo,
 * a etapa de dados de um SET_REPORT ou o ZLP de status do host.
 * TODO o despacho do capítulo 9/HID mora aqui, deliberadamente
 * "achatado" num único nível (ver nota de pilha no cabeçalho).       */
static void ep0_saida_processar(void)
{
    uint8_t i;

    if (BD_PID(BD0OST) != PID_SETUP) {
        /* ----- PID_OUT ------------------------------------------- */
        if (estado_ctl == CTL_RX_DADOS) {
            /* Etapa de dados do SET_REPORT: guarda no correio       */
            uint8_t n = (uint8_t)(BD0OBC & 0x0Fu);

            if (n > USB_TAM_REPORT) {
                n = USB_TAM_REPORT;
            }
            for (i = 0; i < n; i++) {
                acerto_rx[i] = EP0_SAIDA[i];
            }
            /* Aceita qualquer comando conhecido (acerto de hora ou
             * configuração de alarme); quem interpreta o byte [0] é o
             * laço principal, via usb_pegar_acerto().                */
            if ((n == USB_TAM_REPORT) &&
                (acerto_rx[0] >= USB_CMD_ACERTAR_RTC) &&
                (acerto_rx[0] <= USB_CMD_LIGA_ALARME)) {
                ha_acerto = true;    /* main consome via usb_pegar... */
            }
            ep0_armar_status_in();   /* confirma com ZLP IN           */
        } else {
            /* ZLP de status do host (fim de uma leitura) ou pacote
             * inesperado: só volta a esperar SETUP.                  */
            estado_ctl = CTL_OCIOSO;
        }
        ep0_armar_para_setup();
        return;
    }

    /* ----- PID_SETUP: nova requisição de controle ----------------- */
    for (i = 0; i < 8u; i++) {
        setup_pkt[i] = EP0_SAIDA[i];
    }

    /* O SIE pode ter deixado um IN antigo pendurado: toma o BD de
     * volta antes de montar a nova resposta (recomendação PKT_DIS,
     * seção 10.5.1.6 do datasheet).                                  */
    BD0IST = 0;
    estado_ctl = CTL_OCIOSO;
    ctl_zlp    = false;

    if ((setup_pkt[0] & 0x60u) == 0x00u) {
        /* ------------------- requisições PADRÃO ------------------- */
        switch (setup_pkt[1]) {
        case REQ_GET_DESCRIPTOR:
            switch (setup_pkt[3]) {          /* wValue alto = tipo    */
            case DESC_DISPOSITIVO:
                ep0_preparar_tx(usb_desc_dispositivo,
                                USB_TAM_DESC_DISPOSITIVO);
                break;
            case DESC_CONFIGURACAO:
                /* Conjunto completo; o wLength do host limita quando
                 * ele só quer os 9 primeiros bytes                   */
                ep0_preparar_tx(usb_desc_config, USB_TAM_DESC_CONFIG);
                break;
            case DESC_STRING:
                switch (setup_pkt[2]) {      /* wValue baixo = índice */
                case 0:
                    ep0_preparar_tx(usb_str_idiomas, usb_str_idiomas[0]);
                    break;
                case 1:
                    ep0_preparar_tx(usb_str_fabricante,
                                    usb_str_fabricante[0]);
                    break;
                case 2:
                    ep0_preparar_tx(usb_str_produto, usb_str_produto[0]);
                    break;
                default:
                    ep0_stall();
                    break;
                }
                break;
            case DESC_HID:
                /* O descritor HID mora dentro do conjunto de config. */
                ep0_preparar_tx(&usb_desc_config[USB_OFFSET_DESC_HID],
                                USB_TAM_DESC_HID);
                break;
            case DESC_REPORT:
                ep0_preparar_tx(usb_desc_report, USB_TAM_DESC_REPORT);
                break;
            default:
                /* inclui DEVICE_QUALIFIER (só existe em high-speed)  */
                ep0_stall();
                break;
            }
            break;

        case REQ_SET_ADDRESS:
            /* Só entra em vigor depois do status (bit 7 = pendente)  */
            endereco_pendente = (uint8_t)(setup_pkt[2] | 0x80u);
            ep0_armar_status_in();
            break;

        case REQ_SET_CONFIGURATION:
            config_atual = setup_pkt[2];     /* 0 ou 1                */
            if (config_atual != 0u) {
                UEP1 = UEP_SOMENTE_IN;
                ep1_data1 = false;           /* toggle volta a DATA0  */
                ep1_armar();
            } else {
                UEP1 = 0;                    /* volta a "addressed"   */
            }
            ep0_armar_status_in();
            break;

        case REQ_GET_CONFIGURATION:
            resposta_curta[0] = config_atual;
            ep0_preparar_tx(resposta_curta, 1);
            break;

        case REQ_GET_STATUS:
            /* dispositivo: bit0 = auto-alimentado; interface/EP: 0   */
            resposta_curta[0] =
                ((setup_pkt[0] & 0x1Fu) == 0x00u) ? 0x01u : 0x00u;
            resposta_curta[1] = 0;
            ep0_preparar_tx(resposta_curta, 2);
            break;

        case REQ_CLEAR_FEATURE:
        case REQ_SET_FEATURE:
            /* Única feature pertinente: ENDPOINT_HALT no EP1 IN.
             * CLEAR reinicia o toggle (USB 1.1, 9.4.5).              */
            if ((setup_pkt[1] == REQ_CLEAR_FEATURE) &&
                ((setup_pkt[0] & 0x1Fu) == 0x02u) &&
                (setup_pkt[4] == 0x81u)) {
                ep1_data1 = false;
                if (config_atual != 0u) {
                    ep1_armar();
                }
            }
            ep0_armar_status_in();
            break;

        case REQ_GET_INTERFACE:
            resposta_curta[0] = 0;           /* só existe alternate 0 */
            ep0_preparar_tx(resposta_curta, 1);
            break;

        case REQ_SET_INTERFACE:
            ep0_armar_status_in();
            break;

        default:
            ep0_stall();
            break;
        }
    } else if ((setup_pkt[0] & 0x60u) == 0x20u) {
        /* ------------------- requisições de CLASSE (HID) ---------- */
        switch (setup_pkt[1]) {
        case HID_GET_REPORT:
            /* Devolve o report de estado pelo EP0 (o cast descarta o
             * volatile: a ISR é a única escritora durante o envio)   */
            ep0_preparar_tx((const uint8_t *)relatorio_estado,
                            USB_TAM_REPORT);
            break;

        case HID_SET_REPORT:
            /* Acerto de hora: 8 bytes chegam na etapa de dados OUT   */
            if (setup_pkt[6] != 0u) {
                BD0OBC = 8;
                BD0OAL = EP0_SAIDA_END;
                /* 1º pacote de dados = DATA1, com checagem de toggle */
                BD0OST = (uint8_t)(BD_UOWN | BD_DATA1 | BD_DTS);
                estado_ctl = CTL_RX_DADOS;
            } else {
                ep0_armar_status_in();
            }
            break;

        case HID_SET_IDLE:
            taxa_idle = setup_pkt[3];
            ep0_armar_status_in();
            break;

        case HID_GET_IDLE:
            resposta_curta[0] = taxa_idle;
            ep0_preparar_tx(resposta_curta, 1);
            break;

        case HID_SET_PROTOCOL:
            ep0_armar_status_in();
            break;

        case HID_GET_PROTOCOL:
            resposta_curta[0] = 1;           /* report protocol       */
            ep0_preparar_tx(resposta_curta, 1);
            break;

        default:
            ep0_stall();
            break;
        }
    } else {
        ep0_stall();                 /* requisições vendor: não há    */
    }

    /* ----- "rabo" comum do SETUP ----------------------------------- */
    if (estado_ctl == CTL_TX_DADOS) {
        ep0_enviar_bloco();          /* dispara o 1º bloco de dados   */
    }
    if (estado_ctl != CTL_RX_DADOS) {
        /* EP0 OUT pronto p/ status OUT ou próximo SETUP (no caso
         * RX_DADOS o OUT já foi armado p/ a etapa de dados)          */
        if ((BD0OST & BD_UOWN) == 0u) {
            ep0_armar_para_setup();
        }
    }

    /* Libera o SIE para voltar a processar pacotes (o recebimento de
     * um SETUP levanta PKT_DIS automaticamente).                     */
    UCTRL &= (uint8_t)~UCTRL_PKT_DIS;
}

/* Transação concluída no EP0, direção IN (uma resposta nossa foi
 * entregue): continua a etapa de dados, ou fecha a transferência.    */
static void ep0_entrada_processar(void)
{
    if (estado_ctl == CTL_TX_DADOS) {
        if (ctl_restante != 0u) {
            ep0_enviar_bloco();      /* próximo bloco de 8            */
        } else if (ctl_zlp) {
            ctl_zlp = false;         /* ZLP final (resposta curta     */
            ep0_enviar_bloco();      /*  múltipla de 8: BC=0)         */
        } else {
            estado_ctl = CTL_STATUS_OUT;  /* host fará o ZLP OUT      */
        }
    } else if (estado_ctl == CTL_STATUS_IN) {
        /* Status concluído: hora de efetivar SET_ADDRESS, se houver  */
        if (endereco_pendente & 0x80u) {
            UADDR = (uint8_t)(endereco_pendente & 0x7Fu);
            endereco_pendente = 0;
        }
        estado_ctl = CTL_OCIOSO;
    }
}

/* ==================================================================
 *  API pública
 * ================================================================== */
void usb_iniciar(void)
{
    uint8_t i;

    for (i = 0; i < USB_TAM_REPORT; i++) {
        relatorio_estado[i] = 0;
        acerto_rx[i] = 0;
    }
    estado_ctl   = CTL_OCIOSO;
    config_atual = 0;
    ha_acerto    = false;
    taxa_idle    = 0;

    UIR  = 0;                        /* nenhuma flag pendente         */
    UEIR = 0;
    UEIE = 0;                        /* erros só via flag UERR        */

    /* Interrupções do SIE: reset, fim de token, idle (suspensão),
     * stall e erro. ACTIVITY só é habilitada dentro da suspensão.    */
    UIE = (uint8_t)(U_USB_RST | U_TOK_DNE | U_UIDLE | U_STALL | U_UERR);

    /* DEV_ATT liga o regulador de 3,3 V no pino VUSB; com o resistor
     * externo de 1k5 entre VUSB e D-, o host enxerga um dispositivo
     * LOW-SPEED conectado e inicia o reset/enumeração.               */
    UCTRL = UCTRL_DEV_ATT;

    PIR1bits.USBIF = 0;
    PIE1bits.USBIE = 1;
}

void usb_isr(void)
{
    /* --- reset de barramento ---------------------------------------*/
    if (UIR & U_USB_RST) {
        reset_processar();
        UIR = (uint8_t)~U_USB_RST;   /* escreve 0 só no bit tratado   */
    }

    /* --- erro de barramento: registra limpando (recuperação é do
     *     protocolo — host retransmite) ------------------------------*/
    if (UIR & U_UERR) {
        UEIR = 0;
        UIR  = (uint8_t)~U_UERR;
    }

    /* --- fim de transação: USTAT é um FIFO de 4 posições; limpar
     *     TOK_DNE avança o FIFO, e se houver outro evento na fila o
     *     bit volta a 1 imediatamente (seção 10.5.1.5) ---------------*/
    while (UIR & U_TOK_DNE) {
        uint8_t stat = USTAT;        /* 1º: lê o status               */
        UIR = (uint8_t)~U_TOK_DNE;   /* 2º: limpa (avança o FIFO)     */

        if (USTAT_EP(stat) == 0u) {  /* 3º: processa                  */
            if (USTAT_EH_IN(stat)) {
                ep0_entrada_processar();
            } else {
                ep0_saida_processar();
            }
        }
        /* EP1 IN concluído: nada a fazer — o laço principal re-arma
         * com dados frescos em usb_atualizar_estado() (evita disputa
         * entre ISR e main pelo BD1).                                */
    }

    /* --- SIE respondeu STALL (após ep0_stall): re-arma o EP0 para
     *     que o próximo SETUP encontre o endpoint pronto -------------*/
    if (UIR & U_STALL) {
        if (UEP0 & 0x01u) {          /* limpa EP_STALL se o SIE setou */
            UEP0 = UEP_CONTROLE;
        }
        BD0IST = 0;
        ep0_armar_para_setup();
        estado_ctl = CTL_OCIOSO;
        UIR = (uint8_t)~U_STALL;
    }

    /* --- barramento ocioso por 3 ms: host suspendeu -----------------*/
    if (UIR & U_UIDLE) {
        UIR  = (uint8_t)~U_UIDLE;    /* limpa ANTES de suspender      */
        UIE |= U_ACTIVITY;           /* acorda com atividade na linha */
        UCTRL |= UCTRL_SUSPND;
        /* O CPU segue rodando (a placa tem fonte própria); apenas o
         * transceptor USB entra em baixo consumo.                    */
    }

    /* --- atividade na linha: sai da suspensão ------------------------*/
    if (UIR & U_ACTIVITY) {
        UCTRL &= (uint8_t)~UCTRL_SUSPND;  /* 1º: sai da suspensão     */
        UIE   &= (uint8_t)~U_ACTIVITY;    /* (flags de UIR só podem   */
        UIR    = (uint8_t)~U_ACTIVITY;    /*  mudar com SUSPND=0)     */
    }
}

bool usb_configurado(void)
{
    return config_atual != 0u;
}

bool usb_pegar_acerto(uint8_t destino[USB_TAM_REPORT])
{
    uint8_t i;
    bool havia = false;

    INTCONbits.GIE = 0;              /* seção crítica curta com a ISR */
    if (ha_acerto) {
        for (i = 0; i < USB_TAM_REPORT; i++) {
            destino[i] = acerto_rx[i];
        }
        ha_acerto = false;
        havia = true;
    }
    INTCONbits.GIE = 1;

    return havia;
}

void usb_atualizar_estado(const uint8_t estado[USB_TAM_REPORT])
{
    uint8_t i;

    INTCONbits.GIE = 0;
    for (i = 0; i < USB_TAM_REPORT; i++) {
        relatorio_estado[i] = estado[i];
    }
    /* Se o EP1 está configurado e o BD é nosso (UOWN=0), recarrega o
     * report na hora; senão o dado novo entra no próximo re-arme.    */
    if ((config_atual != 0u) && ((BD1IST & BD_UOWN) == 0u)) {
        ep1_armar();
    }
    INTCONbits.GIE = 1;
}
