# VFDCLK — Relógio VFD com PIC16C745

Relógio e termo-higrômetro de bancada baseado no microcontrolador
**PIC16C745** (USB 1.1 nativo). Mostra num display **VFD IEE Century 036X2
(20×2)** a **hora e a data**, alternando com **temperatura e umidade**, e
**acerta o relógio pela porta USB**: basta conectar o dispositivo ao PC e
rodar um pequeno utilitário em Python que envia a hora local — sem
instalar driver (o aparelho enumera como HID genérico).

> Firmware em C (MPLAB XC8), escrito na forma canônica de projetos para
> PIC: `board.h` central, um driver por periférico, documentação e
> referência de datasheet em cada módulo. Compila em **79,7 %** da ROM
> (6526/8192 words) e **71,1 %** da RAM (182/256 bytes) do PIC16C745.

---

## Recursos

- ⏰ **Relógio/calendário** com RTC **DS3231** (±2 ppm, com bateria de
  retenção). Detecta hora inválida (flag OSF) e avisa no display.
- 🌡️ **Temperatura e umidade** com sensor **Sensirion SHT15**, incluindo
  verificação de **CRC-8** e conversão em **ponto fixo** (sem `float`,
  validada contra as fórmulas oficiais da Sensirion).
- 🖥️ **Display VFD 20×2** em modo serial (19200 8N1), com telas que
  alternam automaticamente (6 s hora / 4 s clima / 3 s alarme) e
  controle de brilho.
- 🔌 **Acerto de hora por USB** (classe HID, **sem driver**): o firmware
  recebe a hora do PC e grava no DS3231. Todo o stack USB roda por
  interrupção, então o relógio nunca "trava" durante a comunicação.
- ⏱️ **Alarme diário**, configurável pelo PC (`--alarme 07:30`) ou pelos
  botões. Fica guardado nos registradores do **DS3231 alimentados pela
  bateria** — a única memória não-volátil do projeto, já que o PIC16C745
  é OTP e **não tem EEPROM**. Aviso sonoro (buzzer) + mensagem piscando
  no display, com auto-silenciamento após 2 minutos.
- 🔘 **Dois botões**: um avança as telas; o outro silencia o alarme
  (toque curto) ou liga/desliga (toque longo de ~2 s).
- 💓 **LED de heartbeat** (RC0): pisca a ~1 Hz enquanto o laço roda —
  sinal permanente de que o PIC está vivo, e um diagnóstico grátis (se
  congelar, algo travou).

---

## Estrutura do repositório

```
PROJ_CLOCK_16C745/
├── README.md                 ← este arquivo
├── LICENSE                   ← MIT (código)
├── VFDCLK.X/                 ← projeto MPLAB X (firmware)
│   ├── main.c                ← laço principal e telas
│   ├── board.h               ← mapa de pinos e macros de hardware
│   ├── uart.c/.h             ← USART (serial para o VFD)
│   ├── vfd.c/.h              ← driver do display IEE 036X2
│   ├── swi2c.c/.h            ← mestre I²C por software
│   ├── ds3231.c/.h           ← driver do RTC
│   ├── sht1x.c/.h            ← driver do sensor + conversão ponto fixo
│   ├── usb_hid.c/.h          ← stack USB HID (capítulo 9 + classe)
│   ├── usb_desc.c/.h         ← descritores USB
│   └── botoes.c/.h           ← debounce e toque curto/longo
├── DTCAPP/                   ← utilitário do PC (Python)
│   ├── dtc_sync.py           ← acerta hora e alarme pelo USB
│   ├── teste_conversao.py    ← valida a matemática do SHT15
│   └── requirements.txt
├── HARDWARE/
│   ├── pinagem.md            ← ligação elétrica completa (LEIA-ME)
│   └── VFD_CLOCK.pdsprj      ← simulação em Proteus
└── DOCS/                     ← datasheets (fora do controle de versão)
```

> A pasta `DOCS/` fica **fora do Git** (ver `.gitignore`): o datasheet do
> display IEE é material proprietário e não pode ser redistribuído. Os
> links oficiais estão [na seção de referências](#referências-datasheets).

---

## Hardware

Resumo dos periféricos (detalhes, valores de componentes e esquemático em
[`HARDWARE/pinagem.md`](HARDWARE/pinagem.md)):

| Bloco        | Pinos do PIC        | Observação                              |
|--------------|---------------------|-----------------------------------------|
| Display VFD  | RC6/TX (17)         | serial 19200 8N1, **via MAX232**        |
| RTC DS3231   | RB0/SDA, RB1/SCL    | I²C por software, pull-ups 4,7 kΩ        |
| Sensor SHT15 | RB2/DATA, RB3/SCK   | Sensibus, pull-up 10 kΩ em DATA          |
| USB          | RC4/D-, RC5/D+, VUSB | low-speed, 1,5 kΩ de VUSB para D-        |
| Clock        | OSC1/OSC2 (9/10)    | cristal 24 MHz (HS, sem PLL)             |
| Botões       | RA0, RA1 (2/3)      | troca de tela / alarme, pull-up 10 kΩ    |
| Buzzer       | RC2 (13)            | alarme, via transistor NPN               |
| LED heartbeat| RC0 (11)            | liveness ~1 Hz, série c/ 330 Ω           |

⚠️ **Ponto de atenção — serial do display:** o VFD espera a linha em
repouso no nível **baixo** (mark, padrão EIA-232), enquanto a UART do PIC
repousa em nível **alto**. Um **MAX232** entre RC6 (T1IN) e a entrada do
display (T1OUT) faz as duas coisas: converte TTL ↔ EIA-232 e inverte a
polaridade. O esquemático completo (canal, capacitores e jumpers do
display) está no documento de hardware.

---

## Compilando o firmware

**Ferramentas** (as mesmas com que o projeto foi validado):

- MPLAB X IDE **v6.30**
- Compilador MPLAB **XC8 v4.00**
- Device Family Pack **PIC12-16Cxxx_DFP 1.7.175**

### Pelo MPLAB X (recomendado)

1. `File → Open Project…` e selecione a pasta `VFDCLK.X`.
2. Clique em **Build** (o martelo). O `.hex` sai em
   `VFDCLK.X/dist/default/production/VFDCLK.X.production.hex`.

### Pela linha de comando

```bash
cd VFDCLK.X
make            # usa os Makefiles do projeto + XC8
```

> É esperado o build emitir avisos `(520) function ... is never called`
> para algumas funções de API dos drivers (ex.: `vfd_limpar`,
> `uart_enviar`): elas fazem parte da biblioteca reutilizável de cada
> módulo, não são usadas por *este* programa e o XC8 as remove do binário
> (garbage collection). O advisory `(1510)` sobre `ep1_armar` também é
> normal — o compilador duplica a função por ela ser chamada tanto pelo
> laço quanto pela interrupção.

### Gravando o PIC16C745

O PIC16C745 é **OTP** (memória EPROM). Para desenvolvimento, use a versão
**/JW** (com janela de quartzo, apagável por UV) e um gravador compatível
(ex.: PICSTART/PICkit com adaptador). Grave o `.hex` gerado acima. Os bits
de configuração já vão embutidos (`FOSC=HS, WDTE=OFF, PWRTE=ON, CP=OFF`).

---

## Acertando a hora pelo USB

Com o relógio conectado ao PC (ele aparece como HID, sem driver):

```bash
cd DTCAPP
pip install -r requirements.txt
python dtc_sync.py
```

O utilitário envia a hora local do PC; o firmware grava no DS3231, o
display mostra **"HORA SINCRONIZADA"** e o script lê de volta hora,
temperatura e umidade como confirmação. Detalhes em
[`DTCAPP/README.md`](DTCAPP/README.md).

---

## Como funciona (visão geral)

- **Laço principal cooperativo** (`main.c`): a cada ~50 ms lê o DS3231;
  na virada do segundo, redesenha a tela, alterna hora↔clima e, a cada
  30 s, dispara uma medição do SHT15.
- **USB por interrupção** (`usb_hid.c`): o SIE do PIC16C745 cuida da
  camada física; o firmware trata apenas transações completas (reset,
  enumeração do capítulo 9, classe HID). A troca de dados usa dois
  "correios" protegidos contra concorrência entre a ISR e o laço.
- **Sem `float`**: a conversão do SHT15 usa inteiros de 32 bits com
  coeficientes escalonados; o erro versus as fórmulas oficiais é
  < 0,05 °C / 0,15 %RH, comprovado por `DTCAPP/teste_conversao.py`.
- **Pilha de hardware de 8 níveis**: os módulos são deliberadamente
  "achatados" (poucos níveis de chamada) para conviver com a ISR do USB —
  ver a nota no cabeçalho de `usb_hid.c`.

---

## Referências (datasheets)

Documentos oficiais usados no projeto (baixe direto do fabricante):

- **PIC16C745/765** — Microchip, doc. *DS41124* →
  <https://www.microchip.com/en-us/product/PIC16C745>
- **DS3231** — Analog Devices (Maxim) →
  <https://www.analog.com/en/products/ds3231.html>
- **SHT1x / SHT15** — Sensirion (datasheet, *Sample Code* e *CRC
  Calculation*) →
  <https://sensirion.com/products/catalog/SHT15>
- **Display VFD IEE Century 036X2** — Industrial Electronic Engineers,
  *Product Specification S036X2* (documento proprietário; solicite ao
  fabricante) → <https://www.ieeinc.com/>

---

## Nota sobre o VID/PID USB

O firmware usa o par de **teste** do projeto [pid.codes](https://pid.codes)
(**VID 0x1209 / PID 0x0001**), adequado para desenvolvimento e uso
pessoal. Para distribuir um produto, solicite um PID próprio — nunca
reutilize o VID/PID de outro fabricante.

---

## Licença

Código sob licença **MIT** (ver [LICENSE](LICENSE)). A licença cobre o
firmware e o utilitário do PC; **não** se estende aos datasheets dos
fabricantes nem ao protocolo proprietário do display IEE.
