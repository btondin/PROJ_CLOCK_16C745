# Hardware — Relógio VFD (PIC16C745)

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
         RA0/AN0  2 ┤               ├ 27  RB6/ICSPCLK
         RA1/AN1  3 ┤               ├ 26  RB5
         RA2/AN2  4 ┤               ├ 25  RB4
    RA3/AN3/VREF  5 ┤               ├ 24  RB3   ── SCK  (SHT15)
     RA4/T0CKI    6 ┤               ├ 23  RB2   ── DATA (SHT15)
        RA5/AN4   7 ┤   PIC16C745   ├ 22  RB1   ── SCL  (DS3231)
            VSS   8 ┤   (GND)       ├ 21  RB0   ── SDA  (DS3231)
      OSC1/CLKIN  9 ┤   ── xtal     ├ 20  VDD   (+5 V)
     OSC2/CLKOUT 10 ┤   ── xtal     ├ 19  VSS   (GND)
   RC0/T1OSO     11 ┤               ├ 18  RC7/RX/DT
   RC1/T1OSI     12 ┤               ├ 17  RC6/TX ── serial p/ VFD (via inversor)
      RC2/CCP1   13 ┤               ├ 16  RC5/D+ ── USB D+
           VUSB  14 ┤               ├ 15  RC4/D- ── USB D-
                    └───────────────┘
```

| Pino | Sinal        | Ligação no projeto                                    |
|-----:|--------------|-------------------------------------------------------|
|   1  | MCLR/VPP     | Pull-up 10 kΩ para +5 V (+ conector ICSP p/ gravar)   |
|   8  | VSS          | GND                                                   |
|   9  | OSC1         | Cristal 6 MHz + 15–22 pF para GND                     |
|  10  | OSC2         | Cristal 6 MHz + 15–22 pF para GND                     |
|  14  | VUSB         | Capacitor de 0,22 µF para GND; **1,5 kΩ p/ D‑ (pino 15)** |
|  15  | RC4/D-       | USB D- (verde)                                        |
|  16  | RC5/D+       | USB D+ (branco)                                       |
|  17  | RC6/TX       | Entrada serial do VFD (J1-14) **através do inversor**  |
|  19  | VSS          | GND                                                   |
|  20  | VDD          | +5 V                                                  |
|  21  | RB0/SDA      | SDA do DS3231 (pull-up 4,7 kΩ para +5 V)              |
|  22  | RB1/SCL      | SCL do DS3231 (pull-up 4,7 kΩ para +5 V)              |
|  23  | RB2/DATA     | DATA do SHT15 (pull-up 10 kΩ para +5 V)              |
|  24  | RB3/SCK      | SCK do SHT15                                           |
| 27/28| RB6/RB7      | ICSPCLK/ICSPDAT (gravação in-circuit)                 |

Os pinos RB4, RB5 e o restante ficam livres.

---

## 2. Alimentação e oscilador

- **+5 V / GND**: VDD no pino 20, VSS nos pinos 8 e 19 (ligar ambos!).
  Capacitor de desacoplamento de 100 nF junto a cada par VDD/VSS.
- **Cristal**: 6 MHz entre OSC1 (9) e OSC2 (10), com dois capacitores de
  carga (15–22 pF) de cada pino para GND. O PLL 4× interno multiplica
  para os 24 MHz exigidos pelo USB (bit de configuração `FOSC = H4`).
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

O display **IEE 036X2** é usado em **modo serial** (9600 8N1). Ajuste os
jumpers de "personalidade" da placa do display para **SERIAL** e
**9600 baud** (seção 3.2.3 do datasheet do display; sem jumper, o padrão
já é 9600).

### Polaridade — por que há um inversor

O datasheet do display (seção 3.2.2.1) especifica a entrada serial em
níveis **EIA-232**:

| Nível lógico              | Tensão EIA-232 | Estado de repouso |
|---------------------------|----------------|-------------------|
| space (logic 0 / start)   | +3 a +15 V     |                   |
| mark  (logic 1 / idle)    | −3 a −15 V     | **repouso = mark**|

Ou seja, o display espera a linha em **repouso no nível baixo** (mark).
A UART do PIC (RC6/TX) faz o contrário: fica em **repouso no nível alto**
(idle = 5 V) e o PIC16C745 **não** tem inversão por hardware. Por isso,
um **inversor de 1 transistor** entre RC6 e a entrada do display resolve
a polaridade *e* já entrega os níveis de 0/5 V que o receptor aceita
(limiar em ~+1,5 V; não é preciso ±12 V nem MAX232):

```
        +5 V
         │
        ┌┴┐ 4,7 kΩ  (pull-up do coletor)
        └┬┘
         ├──────────────► J1-14  (SERIAL INPUT do VFD)
         │
   RC6 ──┤ 4,7 kΩ ├──┐    ┌C
  (TX)   └─────────┘  └──B┤ NPN (ex.: BC547, 2N3904)
                          └E
                           │
                          GND
```

- RC6 em **repouso alto** → transistor **conduz** → coletor em **0 V** →
  display lê **mark** (repouso correto). ✔
- RC6 em **start (baixo)** → transistor **corta** → coletor em **5 V** →
  display lê **space** (bit de start). ✔

O firmware (`uart.c`) transmite UART padrão; a inversão é 100 % em
hardware e transparente para o código.

### E se o seu display aceitar TTL direto?

Alguns exemplares/modos aceitam UART TTL com **repouso alto** diretamente
(sem inversor). Procedimento de teste: ligue RC6 direto em J1-14 e rode o
firmware; se aparecer texto correto, ótimo — o inversor é dispensável. Se
aparecer lixo ou nada, use o inversor acima (opção alinhada ao datasheet).
GND do PIC e GND do display **sempre** em comum.

---

## 5. Barramentos dos periféricos

### DS3231 (I²C por software) — RB0/RB1

```
   +5 V ──4,7kΩ──┬─────────────── SDA (RB0, pino 21)
   +5 V ──4,7kΩ──┼──┬──────────── SCL (RB1, pino 22)
                 │  │
              ┌──┴──┴──┐
              │ DS3231 │  VBAT ── bateria CR2032 (+)
              └────────┘  GND  ── GND
```
Endereço I²C fixo 0x68. A bateria mantém a hora com a alimentação
principal desligada.

### SHT15 (Sensibus — 2 fios proprietário Sensirion) — RB2/RB3

```
   +5 V ──10kΩ──── DATA (RB2, pino 23)
                   SCK  (RB3, pino 24)  ── direto (só o PIC gera clock)
```
O pull-up de 10 kΩ em DATA é exigência do protocolo (a linha é
bidirecional; o sensor a puxa para 0 nos ACKs e ao final da medição).
Coloque 100 nF entre VDD e GND do sensor, bem próximo dele.

---

## 6. Lista de materiais (resumo)

| Qtd | Componente                          | Observação                         |
|----:|-------------------------------------|------------------------------------|
| 1   | PIC16C745 (DIP-28)                   | versão **/JW** (janela) p/ regravar |
| 1   | Display VFD IEE 036X2 (20×2)         | jumpers em SERIAL / 9600           |
| 1   | DS3231 (módulo com bateria)          | RTC I²C                            |
| 1   | Sensirion SHT15                      | sensor T/RH                        |
| 1   | Cristal 6 MHz + 2× 15–22 pF          | clock do USB                       |
| 1   | Transistor NPN (BC547/2N3904)        | inversor da serial                 |
| 2   | Resistor 4,7 kΩ                      | pull-ups I²C                       |
| 2   | Resistor 4,7 kΩ                      | inversor (base + coletor)          |
| 1   | Resistor 10 kΩ                       | pull-up DATA do SHT15              |
| 1   | Resistor 10 kΩ                       | pull-up de MCLR                    |
| 1   | Resistor 1,5 kΩ                      | identificação USB low-speed        |
| 1   | Capacitor 0,22 µF                    | VUSB                               |
| —   | Capacitores 100 nF                   | desacoplamento (1 por CI)          |
| 1   | Conector USB tipo B (ou cabo)        | —                                  |

> **Nota sobre o PIC16C745:** é uma peça **OTP** (EPROM). Para
> desenvolvimento, use a versão **/JW** com janela de quartzo, apagável
> por UV — assim você regrava durante os testes. Grave com o `.hex` gerado
> em `VFDCLK.X/dist/default/production/`.
