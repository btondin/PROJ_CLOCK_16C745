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
sem precisar de driver) e rode:

```
python dtc_sync.py                 # sincroniza a hora do PC (padrão)
python dtc_sync.py --status        # só mostra o estado, sem alterar
python dtc_sync.py --alarme 07:30  # programa o alarme diário e liga
python dtc_sync.py --alarme on     # liga o alarme (mantém o horário)
python dtc_sync.py --alarme off    # desliga o alarme
```

Ao sincronizar, o script envia a hora local, o firmware grava no DS3231
e o display mostra "HORA SINCRONIZADA". Em seguida ele lê de volta a
hora, temperatura, umidade e o estado do alarme como confirmação.

**O alarme fica guardado no próprio DS3231** (registradores alimentados
pela bateria), então sobrevive a quedas de energia — o PIC16C745 é OTP e
não tem EEPROM onde guardar configuração.

Também dá para operar o alarme pelos **botões** do aparelho: o botão 2
silencia com um toque curto e liga/desliga com um toque longo (~2 s).

Códigos de saída: `0` sucesso · `1` dispositivo não encontrado ·
`2` falta a biblioteca `hid` · `3/4` falha na comunicação ·
`5` horário de alarme inválido.

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
