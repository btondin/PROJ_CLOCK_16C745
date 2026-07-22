# Melhorias — VFDCLK

Lista de melhorias observadas em bancada. Os 6 itens foram **implementados**
(build de 22/07/2026, ROM 90,6% / RAM 89,8%) e aguardam validação no hardware.
Cada item mantém o sintoma e o diagnóstico como registro, seguidos de uma nota
**Feito:** com o que foi alterado.

Status:
- ✅ **#1** Dois-pontos na virada de minuto — redesenho completo quando muda min/hora.
- ✅ **#2** Bipe irregular — base de tempo real por TMR1 + SHT adiado no alarme.
  O TMR1 usa o pino RC0, então o **LED de heartbeat foi movido de RC0 para RA2
  (pino 4)** — com o TMR1 ligado, o RC0 deixava de acionar o LED.
- ✅ **#3** Mensagem do alarme — novo layout + pisca por firmware sincronizado ao bipe.
- ✅ **#4** Menu de brilho travava — respiros no comando multibyte de brilho.
- ✅ **#5** Alarme durante edição — redraw no autossilêncio + fecha o menu ao tocar.
- ✅ **#6** Persistência — brilho salvo nos registradores do Alarme 2 do DS3231.

> Validar no PIC: virada de minuto (#1), bipe+pisca do alarme (#2/#3), ciclar o
> brilho sem travar (#4), alarme por cima do menu (#5), brilho após desligar/
> religar (#6). Se o brilho ainda travar, conferir o LED de heartbeat (RC0):
> piscando = display; congelado = PIC.

---

## 1. Corrupção dos dois-pontos na virada de minuto

**Sintoma:** ao virar de 16:12:59 para 16:13:00, apareceu "16:13000"
(os dois-pontos entre minuto e segundo viraram '0').

**Hipótese:** `tela_hora()` ([main.c](VFDCLK.X/main.c)) faz, na mesma
virada de segundo, DUAS chamadas a `vfd_campo_bcd()`
([vfd.c](VFDCLK.X/vfd.c)) em sequência — uma para minutos (coluna 9) e
outra para segundos (coluna 12) — cada uma com seu próprio comando de
posicionamento (`1Bh`). Em qualquer segundo comum só o campo de
segundos muda (uma chamada); só na virada de minuto é que duas
chamadas de posicionamento saem back-to-back. É bem provável que seja
a mesma classe de problema já visto antes (display "ocupado" após um
`1Bh` e perdendo o byte seguinte) — só que agora entre uma chamada de
`vfd_campo_bcd` e a próxima, sem nenhum atraso extra além do que já
existe dentro de `VFD_IR_PARA`.

**Pista para testar:** forçar esse cenário (dois campos mudando no
mesmo tick) e ver se um pequeno atraso *entre* as duas chamadas de
`vfd_campo_bcd` em `tela_hora()` resolve.

---

## 2. Intervalo do bipe do alarme fica irregular

**Sintoma:** o padrão do buzzer (200 ms ligado / 300 ms desligado)
some vezes fica com intervalos errados — mais visível quando os
segundos "viram" com mais de um dígito mudando. O RTC em si está
correto (o horário não desvia), é só o ritmo sonoro que falha.

**Hipótese (forte):** o contador `buzzer_fase` avança **uma vez por
volta do laço principal** ([main.c](VFDCLK.X/main.c), bloco "0d"),
supondo que cada volta dura sempre ~50 ms (o `__delay_ms(50)` do fim
do laço). Mas a duração da volta NÃO é constante:
- a cada 30 s, `sht1x_medir()` bloqueia ~0,4 s numa única volta;
- os redesenhos do display (principalmente quando mais de um campo
  BCD muda, como no item 1 acima) also alongam aquela volta específica
  com os atrasos de `VFD_IR_PARA`.

Nessas voltas mais longas, `buzzer_fase` avança só 1 passo mesmo com
bem mais tempo real passado — daí o "hiccup" perceptível no ritmo do
bipe, e a correlação com os momentos em que os segundos mostram mais
de um dígito mudando.

**Direção de correção:** desacoplar o ritmo do buzzer da contagem de
voltas do laço — ex.: um contador de tempo real (não incrementado
"por iteração", e sim proporcional ao tempo realmente decorrido), ou
mover o toggle do buzzer para um timer de hardware (TMR0/TMR1),
independente de quanto a volta do laço demorou.

---

## 3. Mensagem do alarme tocando — layout confuso

**Sintoma:** a tela atual (`tela_alarme_tocando()` em
[main.c](VFDCLK.X/main.c)) mostra `***ALARME***` numa linha e
`HH:MM` + `(BTN)` espalhados na outra — informação ruim, ficou
estranho.

**Pedido:** layout mais limpo —
```
ALARME! HH:MM
APERTE QUALQUER BOTAO
```
com o **pisca sincronizado com o bipe** do buzzer.

**Observação importante:** o pisca hoje usa o atributo de hardware do
próprio display (`vfd_quadro_piscante()` em [vfd.c](VFDCLK.X/vfd.c),
comandos `31h`/`32h`) — é um clock **interno ao display**, totalmente
independente do `buzzer_fase` do firmware. Os dois nunca vão ficar de
fato sincronizados desse jeito (frequências geradas por relógios
diferentes). Para sincronizar de verdade, o pisca da linha 1 precisa
virar controlado por firmware (escrever/apagar o texto em sincronia
com o mesmo contador que liga/desliga o buzzer) em vez de usar o
atributo do display. Isso também depende de resolver o item 2 acima
primeiro — sem um `buzzer_fase` com ritmo confiável, sincronizar
qualquer coisa a ele preserva o mesmo defeito.

---

## 4. Menu de brilho trava o aparelho inteiro (precisa desligar/religar)

**Sintoma:** no menu de configuração, opção BRILHO: o primeiro toque
no botão 2 não parece mudar nada visualmente; ao continuar apertando,
em algum nível (relatado no nível 3) o aparelho trava por completo —
só volta ciclando a energia.

Isso é o item **mais sério** da lista (trava total, sem
recuperação — o watchdog está desligado, `WDTE = OFF`). Duas hipóteses,
não excludentes:

**Hipótese A — margem da pilha de hardware (8 níveis) apertada.**
Contei manualmente a profundidade de chamadas no pior caso (ISR de USB
interrompendo bem no meio de `main → tela_config → vfd_quadro`): dá
por volta de 7 dos 8 níveis disponíveis — ficou **muito** perto do
limite, mesmo com `ep0_saida_processar`/`ep0_entrada_processar`
propositalmente "achatados" (ver cabeçalho de
[usb_hid.c](VFDCLK.X/usb_hid.c)). Contagem manual é sujeita a erro
(o compilador pode inlinear ou duplicar funções — já vimos o aviso
`(1510)` sobre `ep1_armar` duplicada). Antes de mexer no código, vale
checar o relatório de call-graph/uso de pilha que o próprio XC8 gera
(`-msummary`, ou o Call Graph do MPLAB X) para um número real em vez
de contagem manual.

**Hipótese B — comando de brilho (`19h 30h FFh nível`) mais lento do
que o assumido.** O driver assume que só `Clear`/`Reset` precisam de
espera explícita (ver cabeçalho de [vfd.h](VFDCLK.X/vfd.h)); o comando
de brilho nunca foi validado contra a tabela de tempos do datasheet
(seção 3.3) — se for uma operação "lenta" como Clear/Reset, mandar o
redesenho da tela (`vfd_quadro`) logo em seguida, sem nenhum atraso
entre os dois, pode confundir o parser do display.

**Primeiro passo de diagnóstico (barato, não precisa mexer em código):**
quando travar de novo, observar se o **LED de heartbeat (RC0) continua
piscando**. Se continuar piscando => o firmware está vivo, o problema é
só no display (aponta para a Hipótese B). Se o LED também congelar =>
o PIC travou de verdade (aponta para a Hipótese A — pilha estourada ou
outro travamento de firmware).

**Feito (Hipótese B confirmada pelo datasheet):** o comando de brilho é
MULTIBYTE (`19h 30h FFh nível`) e a seção 4.1 do datasheet avisa que
*erro em comando multibyte faz o firmware do display "pular" para fora do
modo de controle*. Como `vfd_brilho()` mandava os 4 bytes sem respiro, o
display (ocupado após o código de comando, igual ao caso do `1Bh` de
posição) derrubava um parâmetro; cada toque acumulava a corrupção até
travar o parser. **Correção:** `__delay_ms(VFD_ATRASO_POS_MS)` após o
código de comando e após os parâmetros, em [vfd.c](VFDCLK.X/vfd.c). O PIC
não travava (Hipótese A descartada) — era só o parser do display. Também
documentado que, sem pot. externo no pino DIMMING, o software cobre os 8
níveis (por isso "não mudava" era o 1º comando corrompido, não um limite).

---

## 5. Alarme disparando durante a edição das telas de configuração

**Pedido:** prever o que acontece se o alarme tocar enquanto o menu de
configuração (alarme/brilho) está aberto, e evitar problemas.

**Já está seguro:** o bloco "0a" do laço principal
([main.c](VFDCLK.X/main.c)) intercepta QUALQUER toque de botão
enquanto `alarme_tocando` é verdadeiro e o consome inteiro para
silenciar — os blocos de navegar/alterar do menu ("0b"/"0c") só veem
`btn1`/`btn2` DEPOIS dessa interceptação, já zerados. Ou seja: **não
dá** para acidentalmente mudar o brilho ou o alarme por engano
enquanto o alarme está tocando; o botão sempre silencia primeiro.
A prioridade de desenho de tela (`if (alarme_tocando) {...}`) também já
garante que a tela do alarme toma a frente do menu.

**Lacuna real encontrada:** quando o alarme se autossilencia pelo
tempo de segurança (`ALARME_MAX_SEGUNDOS`, bloco "2b") — ou seja,
ninguém apertou botão — o código zera `alarme_tocando` e força
`tela_desenhada = 0xFFu`, mas **não** liga `redesenhar = true`. Se
`modo_config` estava ativo antes do alarme interromper, o ramo que
prestaria atenção em `tela_desenhada` (o carrossel) nem roda —
quem precisaria redesenhar é o ramo do menu, que só reage a
`redesenhar`. Resultado: a tela de "ALARME!" fica congelada na tela
por até mais `CONFIG_TIMEOUT_SEGUNDOS` segundos depois do alarme já
ter se calado sozinho, até o menu expirar por timeout. Correção
simples: também setar `redesenhar = true` nesse bloco de
autossilenciamento.

---

## 6. Usar o Alarme 2 do DS3231 como armazenamento de configuração

**Pedido:** guardar se o alarme está ativado e o nível de brilho no
DS3231, para sobreviver ao power-on (hoje o brilho sempre volta ao
máximo e a habilitação do alarme é a única coisa já persistida).

**Confirmado no código:** o projeto só usa o **Alarme 1** do DS3231
(registradores `07h`..`0Ah`, controle `0Eh` — ver
[ds3231.c](VFDCLK.X/ds3231.c)). Os registradores do **Alarme 2**
(`0Bh` minutos, `0Ch` horas, `0Dh` dia/data) não aparecem em nenhum
lugar do driver — estão livres e são alimentados pela mesma bateria,
exatamente como os do Alarme 1. Como o Alarme 2 nunca é armado/
habilitado (bit A2IE do controle permanece 0), esses 3 bytes nunca
disparam nada sozinhos: dá para tratá-los como uma pequena "EEPROM"
de 3 bytes, gratuita.

**Plano sugerido:**
- `alarme_ligado` já é persistido indiretamente pelo bit `A1IE` (não
  precisa duplicar);
- usar 1 desses 3 bytes (ex.: `0x0Bh`) para o **nível de brilho**
  (0..7, cabe fácil num byte; os outros 2 bytes ficam de reserva para
  futuras opções do menu, como o `CFG_NUM_OPCOES` já prevê
  crescimento);
- gravar apenas quando o valor mudar (troca de opção no menu), não a
  cada volta do laço — é EPROM/registrador de RTC, não precisa
  desgastar com escritas repetidas;
- ler no boot, junto com `ds3231_alarme_ler()`, antes de aplicar
  `vfd_brilho()` pela primeira vez.

Precisa de uma função nova tipo `ds3231_config_gravar()`/
`ds3231_config_ler()` em `ds3231.c/.h`, seguindo o mesmo padrão de
leitura/escrita crua de registrador já usado para o Alarme 1.
