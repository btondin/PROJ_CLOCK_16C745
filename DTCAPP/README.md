# DTCAPP — utilitário de sincronização do relógio VFD

Ferramenta **manual** (sem serviço em segundo plano): rode quando quiser
acertar o relógio pela hora do PC.

## Instalação

Requer Python 3.8+ ([python.org](https://www.python.org/downloads/)) e a
biblioteca `hidapi`:

```
pip install -r requirements.txt
```

## Uso

Conecte o relógio VFD numa porta USB (ele enumera como dispositivo HID,
sem precisar de driver).

### Pelo menu (`configurar.bat`) — mais fácil

Dê duplo-clique em [`configurar.bat`](configurar.bat). Ele confere se o
Python e a `hidapi` estão instalados (oferece instalar sozinho se
faltar) e mostra um menu com as mesmas ações de baixo, sem precisar
decorar parâmetros de linha de comando. A opção **1** sincroniza pela
internet (mais preciso) e a **2** pela hora do próprio PC.

### Pela linha de comando

```
python dtc_sync.py                 # sincroniza com a hora deste PC (padrão)
python dtc_sync.py --ntp           # sincroniza com a hora oficial (internet)
python dtc_sync.py --status        # só mostra o estado, sem alterar
python dtc_sync.py --alarme 07:30  # programa o alarme diário e liga
python dtc_sync.py --alarme on     # liga o alarme (mantém o horário)
python dtc_sync.py --alarme off    # desliga o alarme
```

Ao sincronizar, o script envia a hora local, o firmware grava no DS3231
e o display mostra "HORA SINCRONIZADA". Em seguida ele lê de volta a
hora, temperatura, umidade e o estado do alarme como confirmação.

### Hora pela internet (`--ntp`)

Sem a opção, o script confia no relógio do PC — que costuma ter alguns
segundos de erro (e às vezes bem mais). Com `--ntp`, ele consulta um
servidor de tempo antes de enviar:

```
python dtc_sync.py --ntp                       # servidores padrão
python dtc_sync.py --ntp-servidor ntp.empresa  # servidor específico
```

Saída típica:

```
Consultando servidor de tempo a.ntp.br ...
  respondeu (stratum 2, ida-e-volta 28 ms)
  relógio do PC está 50.8 s adiantado — será corrigido no envio
Hora enviada ao relógio: Sunday 16/08/2026 14:58:50  (fonte: NTP a.ntp.br)
```

Detalhes que valem saber:

- **É SNTP (RFC 4330), não uma API HTTP.** O SNTP mede o tempo de
  ida-e-volta do pacote e desconta metade dele; num `GET` HTTP a
  latência da requisição vai embutida na resposta, sem como descontar.
  Resultado: erro de milissegundos, folgadíssimo para um RTC de 1 s de
  resolução.
- **Não instala nada.** Usa só `socket` e `struct` da biblioteca padrão
  — o `requirements.txt` continua com a `hidapi` e mais nada.
- **Servidores padrão:** `a.ntp.br`, `b.ntp.br` (NTP.br/NIC.br, hora
  legal brasileira) e `pool.ntp.org` de reserva, tentados nessa ordem.
- **Fuso horário continua vindo do PC.** O NTP entrega tempo absoluto
  (UTC); a conversão para a hora local que o DS3231 guarda usa o fuso do
  Windows. NTP conserta relógio atrasado, **não** fuso configurado
  errado.
- **Falhou a rede, ninguém acerta nada.** Se nenhum servidor responder,
  o script sai com código 6 sem tocar no relógio, em vez de cair
  silenciosamente para a hora do PC (que é justamente o que você quis
  evitar ao pedir `--ntp`). Algumas redes corporativas bloqueiam a UDP
  123 — nesse caso, aponte `--ntp-servidor` para o servidor de tempo
  interno, ou rode sem `--ntp`.

**O alarme fica guardado no próprio DS3231** (registradores alimentados
pela bateria), então sobrevive a quedas de energia — o PIC16C745 é OTP e
não tem EEPROM onde guardar configuração.

Também dá para operar o alarme pelos **botões** do aparelho: o botão 1
abre o menu de configuração (ALARME/BRILHO) e o botão 2 altera a opção
mostrada — liga/desliga o alarme, por exemplo. Com o alarme tocando,
qualquer botão silencia.

Códigos de saída: `0` sucesso · `1` dispositivo não encontrado ·
`2` falta a biblioteca `hid` · `3/4` falha na comunicação ·
`5` horário de alarme inválido · `6` não obteve a hora da internet.

> **Windows:** se `python` abrir a Microsoft Store, use o caminho
> completo do interpretador, por exemplo
> `& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" dtc_sync.py`,
> ou desative os *aliases de execução* em Configurações → Aplicativos.
> Rode `chcp 65001` antes para os acentos aparecerem corretamente.

## teste_conversao.py

Teste de engenharia (não é preciso rodar para usar o relógio): valida a
aritmética de ponto fixo do firmware (`VFDCLK.X/sht1x.c`) contra as
fórmulas float oficiais da Sensirion, em toda a faixa do sensor:

```
python teste_conversao.py
```

Aprovação: erro ≤ 0,06 °C e ≤ 0,20 %RH em relação às fórmulas oficiais.
