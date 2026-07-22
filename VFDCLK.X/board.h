/**
 * =====================================================================
 *  board.h — Definições de hardware da placa (Relógio VFD)
 * =====================================================================
 *  Projeto : VFDCLK — Relógio/termo-higrômetro com display VFD 20x2
 *  MCU     : PIC16C745 (28 pinos, USB 1.1 low-speed nativo)
 *  Clock   : cristal de 24 MHz em modo HS, sem PLL (FOSC = HS)
 *            -> ciclo de instrução = 24 MHz / 4 = 6 MHz (166,7 ns)
 *  Compilador: MPLAB XC8 (modo C99)
 *
 *  Este arquivo concentra TODO o mapeamento de pinos e as macros de
 *  acesso, para que os drivers não contenham números de pino soltos.
 *
 *  ------------------------------------------------------------------
 *  MAPA DE PINOS (DIP-28)
 *  ------------------------------------------------------------------
 *   Pino  Nome        Função no projeto
 *   ----  ----------  ------------------------------------------------
 *    9    OSC1        cristal 24 MHz
 *    10   OSC2        cristal 24 MHz
 *    14   VUSB        saída do regulador 3,3 V interno do USB
 *                     (capacitor de desacoplamento + resistor de
 *                      1k5 de VUSB para D- = identificação low-speed)
 *    15   RC4/D-      USB D-  (dedicado, não é GPIO neste projeto)
 *    16   RC5/D+      USB D+  (dedicado)
 *    17   RC6/TX      TX do USART -> entrada serial do VFD (J1-14)
 *                     ATENÇÃO: através de um MAX232 (T1IN -> T1OUT), que
 *                     converte TTL<->EIA-232 e inverte a polaridade
 *                     (o VFD repousa em mark = tensão negativa).
 *    21   RB0         SDA do DS3231 (open-drain via TRIS, pull-up 4k7)
 *    22   RB1         SCL do DS3231 (push-pull via sombra de PORTB)
 *    23   RB2         DATA do SHT15 (open-drain via TRIS, pull-up 10k)
 *    24   RB3         SCK  do SHT15 (push-pull via sombra de PORTB)
 *    2    RA0         BOTÃO 1 (navegar menu)  — p/ GND, pull-up 10k
 *    3    RA1         BOTÃO 2 (alterar)       — p/ GND, pull-up 10k
 *    4    RA2         LED de heartbeat (liveness) — série c/ ~330R p/ GND
 *    13   RC2         BUZZER (via transistor NPN; nível 1 = tocando)
 *    11   RC0         livre (é T1OSO/T1CKI do Timer1; NÃO usar como I/O
 *                     enquanto o TMR1 estiver ligado — ver LED em RA2)
 *    1    MCLR        pull-up 10k (reset externo opcional)
 *   demais RA/RB/RC   livres
 *
 *  ------------------------------------------------------------------
 *  TÉCNICA DE ESCRITA EM PORTB ("sombra" contra read-modify-write)
 *  ------------------------------------------------------------------
 *  Nos PIC16 clássicos, instruções BSF/BCF em PORTx fazem
 *  LEITURA-modificação-escrita do registrador: o valor LIDO é o nível
 *  dos PINOS, não o latch. Com pinos em modo entrada (caso das linhas
 *  open-drain SDA/DATA), um BSF em outro bit copiaria o nível externo
 *  para o latch e corromperia o estado. Solução canônica:
 *   - manter uma variável "portb_sombra" com o valor desejado do latch;
 *   - alterar a sombra e escrever o byte INTEIRO em PORTB.
 *  As linhas open-drain (SDA/DATA) ficam com latch fixo em 0 e são
 *  comandadas apenas por TRIS: TRIS=1 solta a linha (pull-up leva a 1),
 *  TRIS=0 força 0. É o jeito padrão de emular open-drain em PIC.
 * =====================================================================
 */
#ifndef BOARD_H
#define BOARD_H

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>

/* Frequência do oscilador do CPU (24 MHz) — usada por __delay_ms/us   */
#define _XTAL_FREQ  24000000UL

/* ------------------------------------------------------------------
 * Sombra de PORTB (definida em main.c) — ver explicação no cabeçalho
 * ------------------------------------------------------------------ */
extern volatile uint8_t portb_sombra;

/* Grava a sombra no latch físico. Sempre escreva o byte inteiro!     */
#define PORTB_APLICAR()     do { PORTB = portb_sombra; } while (0)

/* ------------------------------------------------------------------
 * I2C por software (DS3231)  —  RB0 = SDA, RB1 = SCL
 * ------------------------------------------------------------------
 * SDA: open-drain emulado (latch 0 fixo; TRIS comanda a linha)
 * SCL: push-pull pela sombra (o DS3231 não usa clock-stretching em
 *      leituras simples, e somos o único mestre do barramento)
 * ------------------------------------------------------------------ */
#define I2C_SDA_MASCARA     0x01u               /* RB0                */
#define I2C_SCL_MASCARA     0x02u               /* RB1                */

#define I2C_SDA_SOLTAR()    do { TRISB |=  I2C_SDA_MASCARA; } while (0)
#define I2C_SDA_BAIXO()     do { TRISB &= (uint8_t)~I2C_SDA_MASCARA; } while (0)
#define I2C_SDA_LER()       (PORTBbits.RB0)

#define I2C_SCL_ALTO()      do { portb_sombra |=  I2C_SCL_MASCARA; PORTB_APLICAR(); } while (0)
#define I2C_SCL_BAIXO()     do { portb_sombra &= (uint8_t)~I2C_SCL_MASCARA; PORTB_APLICAR(); } while (0)

/* Meio-período do clock I2C. 5 us -> SCL ~100 kHz (DS3231 aceita até
 * 400 kHz; 100 kHz dá margem folgada para os pull-ups de 4k7).       */
#define I2C_MEIO_PERIODO_US 5

/* ------------------------------------------------------------------
 * Barramento Sensirion (SHT15) — RB2 = DATA, RB3 = SCK
 * ------------------------------------------------------------------
 * DATA: open-drain emulado (exigência do protocolo: o sensor também
 *       controla a linha; pull-up externo de 10k)
 * SCK : push-pull pela sombra (só o PIC gera clock)
 * ------------------------------------------------------------------ */
#define SHT_DATA_MASCARA    0x04u               /* RB2                */
#define SHT_SCK_MASCARA     0x08u               /* RB3                */

#define SHT_DATA_SOLTAR()   do { TRISB |=  SHT_DATA_MASCARA; } while (0)
#define SHT_DATA_BAIXO()    do { TRISB &= (uint8_t)~SHT_DATA_MASCARA; } while (0)
#define SHT_DATA_LER()      (PORTBbits.RB2)

#define SHT_SCK_ALTO()      do { portb_sombra |=  SHT_SCK_MASCARA; PORTB_APLICAR(); } while (0)
#define SHT_SCK_BAIXO()     do { portb_sombra &= (uint8_t)~SHT_SCK_MASCARA; PORTB_APLICAR(); } while (0)

/* Meio-período do clock do SHT15 (datasheet: fSCK max 10 MHz a 5 V;
 * 5 us por nível é conservador e igual ao sample code oficial).      */
#define SHT_MEIO_PERIODO_US 5

/* ------------------------------------------------------------------
 * Botões (PORTA) e buzzer (PORTC)
 * ------------------------------------------------------------------
 * Os botões ficam no PORTA para não gerar escritas no PORTB, que é
 * reservado aos barramentos bit-bang. RA0/RA1 já saem digitais porque
 * board_iniciar_pinos() escreve ADCON1 = 0x07.
 *
 * O buzzer é o único pino de saída do PORTC (RC6/TX pertence ao USART
 * e RC4/RC5 ao módulo USB), então um simples bit-set/clear em PORTC
 * não corre risco de read-modify-write: os demais bits são entradas
 * ou estão sob controle de periférico, e o latch deles é ignorado.
 * ------------------------------------------------------------------ */
#define BTN_TELA_MASCARA    0x01u               /* RA0                */
#define BTN_ALARME_MASCARA  0x02u               /* RA1                */

#define BTN_TELA_LER()      (PORTAbits.RA0)     /* 0 = pressionado    */
#define BTN_ALARME_LER()    (PORTAbits.RA1)     /* 0 = pressionado    */

#define BUZZER_MASCARA      0x04u               /* RC2                */
#define BUZZER_LIGAR()      do { PORTCbits.RC2 = 1; } while (0)
#define BUZZER_DESLIGAR()   do { PORTCbits.RC2 = 0; } while (0)

/* ------------------------------------------------------------------
 * LED de HEARTBEAT (liveness) — RA2 (pino 4)
 * ------------------------------------------------------------------
 * Pisca a ~1 Hz sempre que o firmware está executando um laço. É um
 * sinal visual permanente de que o PIC está VIVO: se o display ficar
 * apagado mas o LED continuar piscando, o problema está no caminho do
 * display, não no processador. LED + resistor ~330 Ω de RA2 para GND.
 *
 * POR QUE RA2 E NÃO RC0: o RC0 é também T1OSO/T1CKI, o pino do Timer1.
 * Ao ligar o TMR1 (base de tempo do bipe do alarme), este exemplar
 * deixou de acionar o LED em RC0 — mesmo com clock interno e o
 * oscilador do TMR1 desligado. RA2 é I/O digital PURO (com ADCON1=0x07,
 * todos os canais digitais), sem função de timer ou periférico, então
 * convive com o TMR1 ligado sem conflito de pino.
 * ------------------------------------------------------------------ */
#define LED_HB_MASCARA      0x04u               /* RA2                */
#define LED_HB_LIGAR()      do { PORTAbits.RA2 = 1; } while (0)
#define LED_HB_DESLIGAR()   do { PORTAbits.RA2 = 0; } while (0)

/* Inicialização dos pinos — implementada em main.c (chamada única).  */
void board_iniciar_pinos(void);

#endif /* BOARD_H */
