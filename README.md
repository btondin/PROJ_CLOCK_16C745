# VFDCLK — Relógio VFD com PIC16C745

Relógio e termo-higrômetro de bancada baseado no microcontrolador
**PIC16C745** (USB 1.1 nativo). Mostra num display **VFD IEE Century 036X2
(20×2)** a **hora e a data**, alternando com **temperatura e umidade**, e
**acerta o relógio pela porta USB**: basta conectar o dispositivo ao PC e
rodar um pequeno utilitário em Python que envia a hora local — sem
instalar driver (o aparelho enumera como HID genérico).

> Firmware em C (MPLAB XC8), escrito na forma canônica de projetos para
> PIC: `board.h` central, um driver por periférico, documentação e
> referência de datasheet em cada módulo. Compila em **90,6 %** da ROM
> (7423/8192 words) e **96,9 %** da RAM (248/256 bytes) do PIC16C745.

---

## Recursos

- ⏰ **Relógio/calendário** com RTC **DS3231** (±2 ppm, com bateria de
  retenção). Detecta hora inválida (flag OSF) e avisa no display.
- 🌡️ **Temperatura e umidade** com sensor **Sensirion SHT15**, incluindo
  verificação de **CRC-8** e conversão em **ponto fixo** (sem `float`,
  validada contra as fórmulas oficiais da Sensirion).
- 🖥️ **Display VFD 20×2** em modo serial (19200 8N1). A **linha de cima
  é fixa com o horário** e o indicador de alarme; só a **linha de baixo
  alterna** (6 s data / 4 s clima):

  ```
  |      14:35:27     S|      S = sino, só com o alarme ligado
  |   TER 16/07/2026   |   <-> | 23.4 °C     45.2 %RH|
  ```

  As duas linhas são escritas num único fluxo de 40 caracteres
  (auto-wrap), o que mantém a 2ª linha estável sem depender do comando
  de trava de rolagem. Como o relógio fica sempre na mesma posição, a
  atualização por segundo reescreve só o dígito que mudou — sem piscar, e
  isso vale nas duas telas do carrossel.
- 🔔 **Os dois símbolos vêm por caminhos diferentes**:
  - o **grau** é caractere **nativo** da fonte deste módulo (`B9h`), então
    não gasta comando nem slot;
  - o **sino** não existe na fonte, então é desenhado numa matriz 5×7 e
    gravado no charset (`F6h`, comando `18h`) uma vez no boot.
- 🔌 **Acerto de hora por USB** (classe HID, **sem driver**): o firmware
  recebe a hora do PC e grava no DS3231. Todo o stack USB roda por
  interrupção, então o relógio nunca "trava" durante a comunicação.
- ⏱️ **Alarme de despertador, só em DIA ÚTIL** (seg–sex), fixo: é o uso
  real do aparelho e o espaço de programa é curto, então não há opção de
  menu para isso. O horário é configurável pelo PC (`--alarme 07:30`) e
  liga/desliga pelos botões; tudo mora nos registradores do **DS3231
  alimentados pela bateria** — a única memória não-volátil do projeto, já
  que o PIC16C745 é OTP e **não tem EEPROM**. Aviso sonoro (buzzer) +
  mensagem piscando no display; ao tocar, **qualquer botão silencia** (e
  rearma para o dia seguinte).

  > O filtro de dia útil é feito pelo firmware: o DS3231 só sabe casar
  > *um* dia da semana específico, não "seg a sex". Ele dispara todo dia
  > e o firmware, no fim de semana, reconhece o flag em silêncio.

  Ligado ou desligado aparece direto na tela do relógio, na última coluna
  da linha de cima: **sino** ou **nada**.
- 🔘 **Menu de configuração por dois botões**: o **botão 1** abre o menu e
  percorre as opções (**alarme**, **brilho da tela**); o **botão 2** altera
  a opção mostrada — o alarme liga/desliga, e o brilho sobe até o máximo e
  volta ao mínimo. Sem toque por 3 s, o menu fecha e o carrossel volta.
- 🐕 **Watchdog ligado** (~2,3 s): qualquer travamento reinicia o PIC e o
  relógio se recupera sozinho, em vez de ficar mudo. As esperas longas do
  boot são fatiadas com `CLRWDT` (macro `ESPERAR_MS`, em `board.h`), porque
  o pior caso do temporizador do 16C745 é ~0,9 s, não os 2,3 s nominais.
- 💓 **LED de heartbeat** (RA2): pisca a ~1 Hz enquanto o laço roda —
  sinal permanente de que o PIC está vivo, e um diagnóstico grátis (se
  congelar, algo travou). Fica em RA2 (não em RC0) porque o RC0 é o pino
  do Timer1, usado como base de tempo do bipe do alarme.

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
| RTC DS3231   | RB4/SCL, RB5/SDA, RB0/INT | I²C por software (pull-ups 4,7 kΩ); INT/SQW no INT0 (uso futuro) |
| Sensor SHT15 | RB2/DATA, RB3/SCK   | Sensibus, pull-up 10 kΩ em DATA          |
| USB          | RC4/D-, RC5/D+, VUSB | low-speed, 1,5 kΩ de VUSB para D-        |
| Clock        | OSC1/OSC2 (9/10)    | cristal 24 MHz (HS, sem PLL)             |
| Botões       | RA0, RA1 (2/3)      | menu: navegar / alterar, pull-up 10 kΩ   |
| Buzzer       | RC2 (13)            | alarme, via transistor NPN               |
| LED heartbeat| RA2 (4)             | liveness ~1 Hz, série c/ 330 Ω           |
| Base de tempo| TMR1 (usa RC0)      | ritmo do bipe; RC0 fica sem uso externo  |

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
de configuração já vão embutidos (`FOSC=HS, WDTE=ON, PWRTE=ON, CP=OFF`).

---

## Acertando a hora pelo USB

Com o relógio conectado ao PC (ele aparece como HID, sem driver):

```bash
cd DTCAPP
pip install -r requirements.txt
python dtc_sync.py         # usa a hora deste PC
python dtc_sync.py --ntp   # usa a hora oficial, buscada na internet
```

O utilitário envia a hora local; o firmware grava no DS3231, o display
mostra **"HORA SINCRONIZADA"** e o script lê de volta hora, temperatura
e umidade como confirmação.

Com **`--ntp`** ele consulta antes um servidor de tempo por **SNTP**
(RFC 4330, UDP 123 — padrão `a.ntp.br`/`b.ntp.br` do NIC.br, com
`pool.ntp.org` de reserva; trocável por `--ntp-servidor`). O SNTP mede o
tempo de ida-e-volta do pacote e desconta metade dele, acertando em
milissegundos — muito além do que um RTC de 1 s de resolução precisa, e
melhor que uma API HTTP, cuja latência de requisição vai embutida na
resposta. Não custa dependência nenhuma: só `socket` e `struct` da
biblioteca padrão. O envio é alinhado com a virada do segundo *já
corrigido*, então o DS3231 começa o segundo junto com a fonte oficial.

> O NTP entrega tempo absoluto (UTC); o **fuso** continua vindo da
> configuração do PC. Ou seja: `--ntp` conserta relógio atrasado, não
> fuso horário errado.

No Windows, [`DTCAPP/configurar.bat`](DTCAPP/configurar.bat) dá um menu
(duplo-clique) para sincronizar a hora e configurar o alarme sem digitar
comando nenhum. Detalhes em [`DTCAPP/README.md`](DTCAPP/README.md).

---

## Como funciona (visão geral)

- **Laço principal cooperativo** (`main.c`): a cada ~50 ms lê o DS3231;
  na virada do segundo, redesenha a tela, alterna a linha de baixo entre
  data e clima e, a cada 30 s, dispara uma medição do SHT15. Alimenta o
  watchdog uma vez por volta.
- **USB por interrupção** (`usb_hid.c`): o SIE do PIC16C745 cuida da
  camada física; o firmware trata apenas transações completas (reset,
  enumeração do capítulo 9, classe HID). A troca de dados usa dois
  "correios" protegidos contra concorrência entre a ISR e o laço.
- **Sem `float`**: a conversão do SHT15 usa inteiros de 32 bits com
  coeficientes escalonados; o erro versus as fórmulas oficiais é
  < 0,05 °C / 0,15 %RH, comprovado por `DTCAPP/teste_conversao.py`.
- **Pilha de hardware de 8 níveis**: é o recurso mais escasso do projeto e
  a origem do bug mais difícil que ele teve. Ela é **circular e não detecta
  estouro**: passando de 8, o endereço de retorno mais antigo é sobrescrito
  em silêncio e o PIC volta para lixo. Com `_main` exigindo 8 e a ISR do USB
  exigindo 3, bastava a interrupção cair no ponto mais fundo para o firmware
  sumir — sem display e sem heartbeat. Como depende do instante da
  interrupção, o defeito era uma **corrida**: builds quase idênticos ora
  rodavam, ora não.

  A cadeia foi encurtada para **7 de 8** eliminando chamadas escondidas de
  biblioteca no caminho mais fundo: divisões e multiplicações em `por_decimos`
  e `vfd_campo_bcd` (que viravam `___lwdiv`/`___lwmod`/`___bmul`) e funções
  intermediárias de uma linha só. Confira o orçamento sempre que mexer no
  código, com:

  ```bash
  grep -B2 "Hardware stack levels required" VFDCLK.X/dist/default/production/VFDCLK.X.production.lst
  ```

  Se `_main` voltar a 8, o firmware pode parar de dar boot sem nenhuma outra
  pista. Os módulos são deliberadamente "achatados" por esse motivo — ver
  também a nota no cabeçalho de `usb_hid.c`.

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
