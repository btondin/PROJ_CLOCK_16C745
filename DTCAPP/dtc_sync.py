#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=======================================================================
 dtc_sync.py — Acerta o RTC do relógio VFD (PIC16C745) pela porta USB
=======================================================================
Uso (manual, sempre que quiser sincronizar):

    python dtc_sync.py

O dispositivo enumera como HID genérico (VID 0x1209 / PID 0x0001,
produto "VFDCLK 16C745"), portanto NÃO precisa de driver. O script:

 1. localiza o dispositivo;
 2. envia um "report de saída" de 8 bytes com a hora local do PC
    (o firmware grava no DS3231 e limpa a flag de hora inválida);
 3. lê de volta o "report de entrada" e mostra a hora, a temperatura
    e a umidade que o relógio está medindo, como confirmação.

Formato do report de SAÍDA (PC -> dispositivo), tudo BCD exceto [4]:
    [0] 0x01 = comando "acertar RTC"
    [1] segundos  [2] minutos  [3] horas (24 h)
    [4] dia da semana 1..7 (1 = segunda ... 7 = domingo, binário)
    [5] dia       [6] mês     [7] ano (duas casas, século 20xx)

Formato do report de ENTRADA (dispositivo -> PC):
    [0] flags: bit0 = hora válida no RTC, bit1 = leitura do SHT15 ok
    [1] segundos  [2] minutos  [3] horas (BCD)
    [4..5] temperatura em décimos de °C (int16 little-endian)
    [6..7] umidade em décimos de %RH (uint16 little-endian)

Dependência: hidapi  ->  pip install -r requirements.txt
=======================================================================
"""

import sys
import time
from datetime import datetime

try:
    import hid  # pacote "hidapi"
except ImportError:
    print("ERRO: biblioteca 'hid' ausente. Instale com:")
    print("    pip install -r requirements.txt")
    sys.exit(2)

VID = 0x1209          # pid.codes (par de teste — ver usb_desc.h)
PID = 0x0001
PRODUTO = "VFDCLK"    # início do nome do produto anunciado via USB

CMD_ACERTAR_RTC = 0x01
FLAG_HORA_VALIDA = 0x01
FLAG_SENSOR_OK = 0x02


def para_bcd(valor: int) -> int:
    """Converte 0..99 decimal para BCD (ex.: 37 -> 0x37)."""
    return ((valor // 10) << 4) | (valor % 10)


def de_bcd(valor: int) -> int:
    """Converte BCD para decimal (ex.: 0x37 -> 37)."""
    return ((valor >> 4) * 10) + (valor & 0x0F)


def achar_dispositivo():
    """Retorna o caminho HID do relógio, ou None se não conectado."""
    for info in hid.enumerate(VID, PID):
        produto = info.get("product_string") or ""
        if produto.startswith(PRODUTO):
            return info["path"]
    return None


def montar_report_de_acerto(agora: datetime) -> bytes:
    """Monta o report de saída (9 bytes: report ID 0 + 8 de dados)."""
    return bytes([
        0x00,                          # report ID (0 = sem numeração)
        CMD_ACERTAR_RTC,
        para_bcd(agora.second),
        para_bcd(agora.minute),
        para_bcd(agora.hour),
        agora.isoweekday(),            # 1 = segunda ... 7 = domingo
        para_bcd(agora.day),
        para_bcd(agora.month),
        para_bcd(agora.year % 100),
    ])


def main() -> int:
    caminho = achar_dispositivo()
    if caminho is None:
        print("Relógio VFD não encontrado no USB.")
        print("Confira o cabo e se o dispositivo enumerou (VID 1209, "
              "PID 0001, produto 'VFDCLK 16C745').")
        return 1

    dispositivo = hid.device()
    dispositivo.open_path(caminho)
    try:
        # ------------------------------------------------------------
        # 1) Envia a hora local. Espera a virada do segundo para o
        #    RTC ficar alinhado com o PC com erro < 50 ms.
        # ------------------------------------------------------------
        agora = datetime.now()
        alvo = agora.replace(microsecond=0)
        while datetime.now() < alvo.replace(microsecond=999999):
            if datetime.now().second != alvo.second:
                break
            time.sleep(0.005)
        agora = datetime.now()

        report = montar_report_de_acerto(agora)
        escritos = dispositivo.write(report)
        if escritos <= 0:
            print("ERRO: falha ao enviar o report de acerto.")
            return 3

        print(f"Hora enviada ao relógio: "
              f"{agora.strftime('%A %d/%m/%Y %H:%M:%S')}")

        # ------------------------------------------------------------
        # 2) Lê o estado de volta (o firmware atualiza o report de
        #    entrada a cada segundo; 2 s de tolerância).
        # ------------------------------------------------------------
        time.sleep(1.2)  # dá tempo do firmware gravar e re-publicar
        dados = dispositivo.read(8, timeout_ms=2000)
        if not dados:
            print("AVISO: hora enviada, mas sem resposta de leitura. "
                  "(O acerto provavelmente funcionou.)")
            return 0

        flags = dados[0]
        hh, mm, ss = de_bcd(dados[3]), de_bcd(dados[2]), de_bcd(dados[1])
        temp = int.from_bytes(bytes(dados[4:6]), "little", signed=True)
        umid = int.from_bytes(bytes(dados[6:8]), "little", signed=False)

        print("Resposta do relógio:")
        print(f"  Hora do RTC : {hh:02d}:{mm:02d}:{ss:02d} "
              f"({'válida' if flags & FLAG_HORA_VALIDA else 'INVÁLIDA'})")
        if flags & FLAG_SENSOR_OK:
            print(f"  Temperatura : {temp / 10:.1f} °C")
            print(f"  Umidade     : {umid / 10:.1f} %RH")
        else:
            print("  Sensor SHT15: sem leitura válida ainda")

        if flags & FLAG_HORA_VALIDA:
            print("Sincronização concluída com sucesso.")
            return 0
        print("ERRO: o relógio não confirmou a hora como válida.")
        return 4
    finally:
        dispositivo.close()


if __name__ == "__main__":
    sys.exit(main())
