11não# Hardware — Relógio VFD (PIC16C745)

Documento de ligação elétrica do projeto. A pinagem do MCU foi conferida
contra o arquivo de definição oficial do dispositivo (Microchip DFP
`PIC16C745.PIC`) e o datasheet DS41124D.

> A simulação em Proteus está em [`VFD_CLOCK.pdsprj`](VFD_CLOCK.pdsprj)
> (abrir com o Proteus 8). Ela reproduz a fiação descrita abaixo.

---

## 1. Pinagem do PIC16C745 (encapsulamento DIP-28 / SOIC-28)

```
                    ┌───────────────┐
        MCLR/VPP  1 ┤●              ├ 28  RB7/ICSPDAT
         RA0/AN0  2 ┤ ── BOTÃO 1    ├ 27  RB6/ICSPCLK
         RA1/AN1  3 ┤ ── BOTÃO 2    ├ 26  RB5   ── SDA  (DS3231)
         RA2/AN2  4 ┤ ── LED (HB)   ├ 25  RB4   ── SCL  (DS3231)
    RA3/AN3/VREF  5 ┤               ├ 24  RB3   ── SCK  (SHT15)
     RA4/T0CKI    6 ┤               ├ 23  RB2   ── DATA (SHT15)
        RA5/AN4   7 ┤   PIC16C745   ├ 22  RB1   (livre)
            VSS   8 ┤   (GND)       ├ 21  RB0   ── INT/SQW (DS3231)
      OSC1/CLKIN  9 ┤   ── xtal     ├ 20  VDD   (+5 V)
     OSC2/CLKOUT 10 ┤   ── xtal     ├ 19  VSS   (GND)
   RC0/T1OSO     11 ┤ (TMR1, livre) ├ 18  RC7/RX/DT
   RC1/T1OSI     12 ┤               ├ 17  RC6/TX ── serial p/ VFD (via MAX232)
      RC2/CCP1   13 ┤ ── BUZZER     ├ 16  RC5/D+ ── USB D+
           VUSB  14 ┤               ├ 15  RC4/D- ── USB D-
                    └───────────────┘
```

| Pino | Sinal        | Ligação no projeto                                    |
|-----:|--------------|-------------------------------------------------------|
|   1  | MCLR/VPP     | Pull-up 10 kΩ para +5 V (+ conector ICSP p/ gravar)   |
|   2  | RA0          | **Botão 1** (navegar no menu) — p/ GND, pull-up 10 kΩ |
|   3  | RA1          | **Botão 2** (alterar opção) — p/ GND, pull-up 10 kΩ   |
|   4  | RA2          | **LED de heartbeat** — em série c/ ~330 Ω para GND    |
|  11  | RC0          | **Sem ligação externa** — é T1OSO/T1CKI, usado pelo TMR1 internamente (base de tempo do bipe do alarme); não usar como I/O |
|  13  | RC2          | **Buzzer** via transistor NPN (nível 1 = tocando)     |
|   8  | VSS          | GND                                                   |
|   9  | OSC1         | Cristal 24 MHz + 15–22 pF para GND                     |
|  10  | OSC2         | Cristal 24 MHz + 15–22 pF para GND                     |
|  14  | VUSB         | Capacitor de 0,22 µF para GND; **1,5 kΩ p/ D‑ (pino 15)** |
|  15  | RC4/D-       | USB D- (verde)                                        |
|  16  | RC5/D+       | USB D+ (branco)                                       |
|  17  | RC6/TX       | Entrada serial do VFD (J1-14) **via MAX232** (T1IN, pino 11)  |
|  19  | VSS          | GND                                                   |
|  20  | VDD          | +5 V                                                  |
|  21  | RB0/INT      | **INT/SQW do DS3231** (dreno aberto, pull-up 4,7 kΩ; entrada INT0 — **uso futuro**) |
|  22  | RB1          | Livre                                                 |
|  23  | RB2/DATA     | DATA do SHT15 (pull-up 10 kΩ para +5 V)              |
|  24  | RB3/SCK      | SCK do SHT15                                           |
|  25  | RB4/SCL      | SCL do DS3231 (pull-up 4,7 kΩ para +5 V)              |
|  26  | RB5/SDA      | SDA do DS3231 (pull-up 4,7 kΩ para +5 V)              |
| 27/28| RB6/RB7      | ICSPCLK/ICSPDAT (gravação in-circuit)                 |

O pino RB1 fica livre (RB6/RB7 são reservados ao ICSP de gravação).

---

## 2. Alimentação e oscilador

- **+5 V / GND**: VDD no pino 20, VSS nos pinos 8 e 19 (ligar ambos!).
  Capacitor de desacoplamento de 100 nF junto a cada par VDD/VSS.
- **Cristal**: 24 MHz entre OSC1 (9) e OSC2 (10), com dois capacitores de
  carga (15–22 pF) de cada pino para GND, em modo HS (bit de configuração
  `FOSC = HS`). Os 24 MHz são exigidos diretamente pelo USB (sem PLL).
  Não use ressonador de baixa precisão: o USB exige ±0,25 % de tolerância.

---

## 3. USB (low-speed)

O PIC16C745 tem transceptor e regulador de 3,3 V embutidos.

```
   Conector USB          PIC16C745
   ─────────────         ─────────────
   VBUS (+5V) ───────────► VDD (20)   (se alimentado pelo barramento)
   D-         ───────────► RC4/D- (15)
   D+         ───────────► RC5/D+ (16)
   GND        ───────────► VSS (8,19)

   VUSB (14) ──┬── 0,22 µF ── GND
               └── 1,5 kΩ ──► D- (15)     ◄── identifica LOW-SPEED
```

- O resistor de **1,5 kΩ de VUSB (3,3 V) para D‑** é o que faz o host
  reconhecer um dispositivo **low-speed** (o firmware liga o VUSB via
  `UCTRL.DEV_ATT` em `usb_iniciar()`).
- Alimentação: o projeto declara-se **self-powered** (descritor,
  `bMaxPower = 20 mA`). Se preferir alimentar pelo próprio USB
  (bus-powered), ligue VBUS ao VDD e troque `bmAttributes` para 0x80 no
  `usb_desc.c` — mas cuidado com o limite de corrente do VFD (o display
  consome bem mais que 100 mA e deve ter fonte própria de qualquer modo).

---

## 4. Serial para o display VFD (o ponto que exige atenção)

O display **IEE 036X2** é usado em **modo serial** (19200 8N1). Ajuste os
jumpers de "personalidade" da placa do display para **19200 baud** e
partida **NORM**, mantendo o **conjunto de comandos nativo (Intel, que é o
padrão)** — **não** selecione o modo **LCD** (ele troca os comandos para os
do HD44780, e o firmware usa os códigos nativos IEE). Seção 3.2.3 do
datasheet do display.

### Por que um MAX232 (conversão de nível + inversão de polaridade)

O datasheet do display (seção 3.2.2.1) especifica a entrada serial em
níveis **EIA-232**, com a linha em **repouso no nível baixo (mark)**:

| Nível lógico              | Tensão EIA-232 | Estado de repouso |
|---------------------------|----------------|-------------------|
| space (logic 0 / start)   | +3 a +15 V     |                   |
| mark  (logic 1 / idle)    | −3 a −15 V     | **repouso = mark**|

A UART do PIC (RC6/TX) faz o oposto: repousa em **nível alto** (idle = 5 V)
e o PIC16C745 **não** inverte por hardware. O **MAX232 resolve as duas
coisas ao mesmo tempo**, pois é um driver **inversor** *e* conversor de
nível:

- converte TTL (0/5 V) → EIA-232 real (±~8 V); e
- inverte a polaridade que o display espera:
  - RC6 em repouso **alto** (mark)  → T1OUT **negativo** = mark ✔
  - RC6 em **start** baixo (space)  → T1OUT **positivo** = space ✔

Como só transmitimos para o display, usa-se **um único canal (T1)**:

```
   PIC RC6/TX (17) ──► T1IN (11)   MAX232   T1OUT (14) ──► J1-14 (SERIAL INPUT do VFD)
                       VCC (16) = +5 V      GND (15) = GND comum (PIC + display)

   Capacitores de charge-pump — 0,1 µF cerâmicos (SP232A / MAX232A):
     C1: pino 1 (C1+) ↔ pino 3 (C1−)       C2: pino 4 (C2+) ↔ pino 5 (C2−)
     V+ (pino 2) ── C ── GND               V− (pino 6) ── C ── GND
     + 100 nF de VCC (16) para GND (desacoplamento)
   Confira os valores exatos dos capacitores no datasheet do seu MAX232.
```

**Pinos não usados (canais R1, R2 e T2):**

| Pino | Nome | O que fazer |
|---:|---|---|
| 10 | T2IN | **ligar ao GND** — é a única entrada TTL sobrando |
| 7 | T2OUT | deixar **aberto** (saída) |
| 9 | R2OUT | deixar **aberto** (saída) |
| 12 | R1OUT | deixar **aberto** (saída) |
| 8 | R2IN | deixar **aberto** |
| 13 | R1IN | deixar **aberto** |

Regra geral: **saída nunca se amarra** (ligá-la a GND/VCC é curto);
**entrada não pode ficar indefinida**. Nesta família as entradas já têm
resistor interno — TTL com pull-up de ~400 kΩ e RS-232 com pull-down de
~5 kΩ — então nada flutua de fato. Mesmo assim vale aterrar o T2IN: o
pull-up de 400 kΩ é fraco e um fio solto em protoboard capta ruído.

- O firmware (`uart.c`) transmite UART padrão: toda a conversão de nível e
  a inversão de polaridade acontecem no MAX232, de forma transparente ao
  código.
- GND do PIC, do MAX232 e do display **sempre** em comum.

---

## 5. Barramentos dos periféricos

### DS3231 (I²C por software) — RB4/RB5 + INT/SQW em RB0

```
   +5 V ──4,7kΩ──┬───────────────────── SCL     (RB4, pino 25)
   +5 V ──4,7kΩ──┼──┬────────────────── SDA     (RB5, pino 26)
   +5 V ──4,7kΩ──┼──┼──┬─────────────── INT/SQW (RB0/INT, pino 21)
                 │  │  │
              ┌──┴──┴──┴──┐
              │  DS3231   │  VBAT ── bateria CR2032 (+)
              └───────────┘  GND  ── GND
```
Endereço I²C fixo 0x68. A bateria mantém a hora com a alimentação
principal desligada.

O **INT/SQW** é uma saída de **dreno aberto** (exige o pull-up externo,
que pode ir a uma alimentação de até **5,5 V**, independente de VCC) e
liga ao **RB0/INT** — a entrada de interrupção externa **INT0** do PIC.
Hoje o firmware **não** o utiliza (o alarme é lido por *polling* do flag
`A1F`, que é *latched*), mas o pino fica reservado para, no futuro,
disparar por interrupção de borda: com `INTCN=1` (padrão no power-on) a
saída é o **alarme ativo-baixo**; com `INTCN=0` vira **onda quadrada**
(frequência por RS2:RS1). Ver [`board.h`](../VFDCLK.X/board.h)
(`DS3231_INT_MASCARA`).

### SHT15 (Sensibus — 2 fios proprietário Sensirion) — RB2/RB3

```
   +5 V ──10kΩ──── DATA (RB2, pino 23)
                   SCK  (RB3, pino 24)  ── direto (só o PIC gera clock)
```
O pull-up de 10 kΩ em DATA é exigência do protocolo (a linha é
bidirecional; o sensor a puxa para 0 nos ACKs e ao final da medição).
Coloque 100 nF entre VDD e GND do sensor, bem próximo dele.

---

## 5.1 Botões e buzzer (alarme)

Os botões ficam no **PORTA** de propósito: o PORTB é reservado aos dois
barramentos bit-bang (I²C e Sensibus), e mantê-lo livre de outras
escritas evita qualquer interferência de *read-modify-write*.

```
   +5 V ──10kΩ──┬── RA0 (pino 2)         +5 V ──10kΩ──┬── RA1 (pino 3)
                │                                      │
             [BOTÃO 1]                              [BOTÃO 2]
                │                                      │
               GND                                    GND
```

| Botão | Ação |
|---|---|
| **1** (RA0) | **NAVEGAR**: abre o menu de configuração e percorre as opções (ALARME, BRILHO) |
| **2** (RA1) | **ALTERAR**: muda o valor da opção mostrada (liga/desliga o alarme; sobe o brilho até o máximo e volta ao mínimo) |

Com o alarme **tocando**, qualquer toque em qualquer botão silencia (e rearma
para o dia seguinte) — a distinção curto/longo continua disponível no driver
(`botoes.c`) para uso futuro, mas hoje qualquer toque conta como um "clique".

Buzzer: **TMB-05** — buzzer **magnético ativo** de 5 V (tem oscilador
interno, então emite som só com nível DC; o firmware apenas liga/desliga
o pino RC2, sem PWM). É acionado por um transistor porque o pino do PIC
não deve fornecer a corrente dele diretamente, e leva um **diodo de
roda-livre** por ser uma carga indutiva (magnética):

```
                          +5 V
                           │
                 ┌─────────┼─────────┐
                 │         │          │
              [diodo]  [TMB-05]       │   diodo 1N4148/1N4007
              catodo──►  buzzer       │   catodo (faixa) p/ +5 V
                 │         │          │
                 └─────────┴──► C     │
   RC2 (13) ──[1 kΩ]──B┤ NPN (BC547 / 2N3904)
                        └E── GND
```

- O **diodo em paralelo com o buzzer** (catodo (faixa) no +5 V, anodo no
  coletor) absorve o pico de tensão quando o transistor corta — sem ele,
  a indutância do TMB-05 pode danificar o transistor com o tempo.
- Corrente típica do TMB-05 ≈ 25–35 mA @ 5 V, folgada para um BC547
  (Ic máx 100 mA); a base com 1 kΩ dá saturação de sobra.
- O RC2 é o **CCP1**: um dia, se trocar por um piezo *passivo*, dá para
  gerar o tom por PWM (exigiria mudar o firmware, que hoje só chaveia o
  pino).

> **Nota sobre o pino INT/SQW do DS3231:** ele é ligado ao **RB0/INT**
> (entrada de interrupção externa INT0), com pull-up externo, e fica
> **reservado para uso futuro**. O firmware atual **ainda não o utiliza**:
> o flag de alarme `A1F` (status `0Fh`) é *latched* — fica em 1 até ser
> reconhecido — então o alarme é lido por *polling* 1×/segundo, sem risco
> de perder um disparo. Com o INT/SQW já roteado até o RB0/INT, dá para
> migrar esse polling para interrupção por borda quando quiser (INT0:
> `INTEDG=0`, `INTE=1`, tratar `INTF` na ISR). Ver [`board.h`](../VFDCLK.X/board.h).

### LED de heartbeat (liveness) — RA2

Item **fixo** do projeto: um LED que pisca a ~1 Hz sempre que o firmware
está executando o laço principal. Serve como sinal permanente de que o
PIC está vivo e o laço rodando — se ele congelar, algo travou (ex.: I²C
preso). Também facilita o diagnóstico na bancada: display apagado **com**
o LED piscando aponta problema no caminho do display, não no processador.

```
   RA2 (4) ──►|──[ ~330 Ω ]── GND     (acende em nível alto)
             LED
```

O firmware pisca o LED na cadência do laço (contador ÷10 sobre os 50 ms),
então o ritmo do pisca também reflete a saúde/tempo do laço.

> **Por que RA2 e não RC0:** o RC0 é também o pino T1OSO/T1CKI do Timer1,
> usado internamente como base de tempo do bipe do alarme (ver firmware).
> Em bancada, ligar o TMR1 fez o LED parar de acender quando estava em
> RC0, mesmo com o oscilador do timer desligado (clock interno) — por
> isso o LED foi para RA2, um pino puramente digital sem função de
> periférico associada.

---

## 6. Lista de materiais (resumo)

| Qtd | Componente                          | Observação                         |
|----:|-------------------------------------|------------------------------------|
| 1   | PIC16C745 (DIP-28)                   | versão **/JW** (janela) p/ regravar |
| 1   | Display VFD IEE 036X2 (20×2)         | jumper em 19200 baud               |
| 1   | DS3231 (módulo com bateria)          | RTC I²C                            |
| 1   | Sensirion SHT15                      | sensor T/RH                        |
| 1   | Cristal 24 MHz + 2× 15–22 pF         | clock do USB                       |
| 1   | SP232ACP (Sipex; equiv. MAX232A)     | conversor de nível serial TTL↔EIA-232 |
| 5   | Capacitor 0,1 µF cerâmico            | charge-pump do SP232ACP            |
| 2   | Resistor 4,7 kΩ                      | pull-ups I²C (SDA/SCL)             |
| 1   | Resistor 4,7 kΩ                      | pull-up do INT/SQW (RB0/INT)       |
| 1   | Resistor 10 kΩ                       | pull-up DATA do SHT15              |
| 1   | Resistor 10 kΩ                       | pull-up de MCLR                    |
| 2   | Botão táctil (push-button)           | troca de tela / alarme             |
| 2   | Resistor 10 kΩ                       | pull-ups dos botões                |
| 1   | Buzzer ativo 5 V (TMB-05)            | alarme sonoro                      |
| 1   | Transistor NPN (BC547/2N3904)        | aciona o buzzer                    |
| 1   | Resistor 1 kΩ                        | base do transistor do buzzer       |
| 1   | Diodo 1N4148/1N4007                  | roda-livre do buzzer               |
| 1   | LED (qualquer cor)                   | heartbeat / liveness (RA2)         |
| 1   | Resistor 330 Ω                       | limitador do LED de heartbeat      |
| 1   | Resistor 1,5 kΩ                      | identificação USB low-speed        |
| 1   | Capacitor 0,22 µF                    | VUSB                               |
| —   | Capacitores 100 nF                   | desacoplamento (1 por CI)          |
| 1   | Conector USB tipo B (ou cabo)        | —                                  |

> **Nota sobre o PIC16C745:** é uma peça **OTP** (EPROM). Para
> desenvolvimento, use a versão **/JW** com janela de quartzo, apagável
> por UV — assim você regrava durante os testes. Grave com o `.hex` gerado
> em `VFDCLK.X/dist/default/production/`.
