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

1. Conecte o relógio VFD numa porta USB (ele enumera como dispositivo
   HID, sem precisar de driver).
2. Execute:

```
python dtc_sync.py
```

O script envia a hora local do PC, o firmware grava no DS3231 e o
display mostra "HORA SINCRONIZADA". Em seguida o script lê de volta a
hora, a temperatura e a umidade do relógio como confirmação.

Códigos de saída: `0` sucesso · `1` dispositivo não encontrado ·
`2` falta a biblioteca `hid` · `3/4` falha na comunicação.

## teste_conversao.py

Teste de engenharia (não é preciso rodar para usar o relógio): valida a
aritmética de ponto fixo do firmware (`VFDCLK.X/sht1x.c`) contra as
fórmulas float oficiais da Sensirion, em toda a faixa do sensor:

```
python teste_conversao.py
```

Aprovação: erro ≤ 0,06 °C e ≤ 0,20 %RH em relação às fórmulas oficiais.
