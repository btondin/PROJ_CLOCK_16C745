#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=======================================================================
 dtc_sync.py — Acerta o RTC e o alarme do relógio VFD (PIC16C745)
=======================================================================
Uso (manual, sempre que quiser):

    python dtc_sync.py                 # sincroniza a hora do PC (padrão)
    python dtc_sync.py --status        # só mostra o estado do relógio
    python dtc_sync.py --alarme 07:30  # programa o alarme e liga
    python dtc_sync.py --alarme on     # liga o alarme (mantém horário)
    python dtc_sync.py --alarme off    # desliga o alarme

O dispositivo enumera como HID genérico (VID 0x1209 / PID 0x0001,
produto "VFDCLK 16C745"), portanto NÃO precisa de driver.

Formato do report de SAÍDA (PC -> dispositivo), 8 bytes:
    [0] = 0x01  acertar RTC
          [1] segundos  [2] minutos  [3] horas (BCD, 24 h)
          [4] dia da semana 1..7 (1 = segunda, binário)
          [5] dia  [6] mês  [7] ano (BCD, século 20xx)
    [0] = 0x02  configurar alarme diário
          [1] horas BCD  [2] minutos BCD  [3] 1 = habilita / 0 = não
    [0] = 0x03  habilitar/desabilitar alarme
          [1] 1 = liga / 0 = desliga

Formato do report de ENTRADA (dispositivo -> PC), 8 bytes:
    [0] flags: bit0 hora válida, bit1 sensor ok,
               bit2 alarme habilitado, bit3 alarme tocando
    [1] segundos  [2] minutos  [3] horas (BCD)
    [4..5] temperatura em décimos de °C (int16 little-endian)
    [6..7] umidade em décimos de %RH (uint16 little-endian)

O alarme fica guardado nos registradores do DS3231, alimentados pela
bateria — sobrevive a quedas de energia sem precisar de EEPROM (o
PIC16C745 não tem nenhuma).

Dependência: hidapi  ->  pip install -r requirements.txt
=======================================================================
"""

import argparse
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
CMD_CONFIG_ALARME = 0x02
CMD_LIGA_ALARME = 0x03

FLAG_HORA_VALIDA = 0x01
FLAG_SENSOR_OK = 0x02
FLAG_ALARME_ON = 0x04
FLAG_ALARME_TOCA = 0x08


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


def enviar(dispositivo, dados: list) -> bool:
    """Envia um report de saída (report ID 0 + 8 bytes de dados)."""
    pacote = bytes([0x00] + dados + [0] * (8 - len(dados)))
    return dispositivo.write(pacote) > 0


def mostrar_estado(dispositivo, espera_s: float = 1.2) -> int:
    """Lê o report de entrada e imprime o estado. Devolve as flags."""
    time.sleep(espera_s)  # o firmware republica o report a cada segundo
    dados = dispositivo.read(8, timeout_ms=2000)
    if not dados:
        print("AVISO: sem resposta de leitura do dispositivo.")
        return -1

    flags = dados[0]
    hh, mm, ss = de_bcd(dados[3]), de_bcd(dados[2]), de_bcd(dados[1])
    temp = int.from_bytes(bytes(dados[4:6]), "little", signed=True)
    umid = int.from_bytes(bytes(dados[6:8]), "little", signed=False)

    print("Estado do relógio:")
    print(f"  Hora do RTC : {hh:02d}:{mm:02d}:{ss:02d} "
          f"({'válida' if flags & FLAG_HORA_VALIDA else 'INVÁLIDA'})")
    if flags & FLAG_SENSOR_OK:
        print(f"  Temperatura : {temp / 10:.1f} °C")
        print(f"  Umidade     : {umid / 10:.1f} %RH")
    else:
        print("  Sensor SHT15: sem leitura válida ainda")
    print(f"  Alarme      : {'LIGADO' if flags & FLAG_ALARME_ON else 'desligado'}"
          f"{'  *** TOCANDO ***' if flags & FLAG_ALARME_TOCA else ''}")
    return flags


def acao_sincronizar(dispositivo) -> int:
    """Envia a hora local do PC, alinhada com a virada do segundo."""
    agora = datetime.now()
    alvo_seg = agora.second
    # Espera a virada do segundo para o RTC ficar alinhado (< 50 ms).
    while datetime.now().second == alvo_seg:
        time.sleep(0.005)
    agora = datetime.now()

    ok = enviar(dispositivo, [
        CMD_ACERTAR_RTC,
        para_bcd(agora.second),
        para_bcd(agora.minute),
        para_bcd(agora.hour),
        agora.isoweekday(),            # 1 = segunda ... 7 = domingo
        para_bcd(agora.day),
        para_bcd(agora.month),
        para_bcd(agora.year % 100),
    ])
    if not ok:
        print("ERRO: falha ao enviar o report de acerto.")
        return 3

    print(f"Hora enviada ao relógio: "
          f"{agora.strftime('%A %d/%m/%Y %H:%M:%S')}")

    flags = mostrar_estado(dispositivo)
    if flags < 0:
        print("(O acerto provavelmente funcionou.)")
        return 0
    if flags & FLAG_HORA_VALIDA:
        print("Sincronização concluída com sucesso.")
        return 0
    print("ERRO: o relógio não confirmou a hora como válida.")
    return 4


def acao_alarme(dispositivo, valor: str) -> int:
    """Configura o alarme: 'HH:MM', 'on' ou 'off'."""
    texto = valor.strip().lower()

    if texto in ("on", "ligar", "liga"):
        if not enviar(dispositivo, [CMD_LIGA_ALARME, 1]):
            print("ERRO: falha ao enviar o comando.")
            return 3
        print("Alarme LIGADO (horário mantido).")
    elif texto in ("off", "desligar", "desliga"):
        if not enviar(dispositivo, [CMD_LIGA_ALARME, 0]):
            print("ERRO: falha ao enviar o comando.")
            return 3
        print("Alarme DESLIGADO.")
    else:
        try:
            partes = texto.split(":")
            hora, minuto = int(partes[0]), int(partes[1])
            if not (0 <= hora <= 23 and 0 <= minuto <= 59):
                raise ValueError
        except (ValueError, IndexError):
            print(f"ERRO: horário inválido '{valor}'. "
                  "Use HH:MM (ex.: 07:30), 'on' ou 'off'.")
            return 5

        if not enviar(dispositivo,
                      [CMD_CONFIG_ALARME, para_bcd(hora), para_bcd(minuto), 1]):
            print("ERRO: falha ao enviar o comando.")
            return 3
        print(f"Alarme programado para {hora:02d}:{minuto:02d} e LIGADO.")

    mostrar_estado(dispositivo)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Acerta a hora e o alarme do relógio VFD (PIC16C745).")
    ap.add_argument("--alarme", metavar="HH:MM|on|off",
                    help="programa o alarme diário, ou liga/desliga")
    ap.add_argument("--status", action="store_true",
                    help="apenas mostra o estado atual, sem alterar nada")
    args = ap.parse_args()

    caminho = achar_dispositivo()
    if caminho is None:
        print("Relógio VFD não encontrado no USB.")
        print("Confira o cabo e se o dispositivo enumerou (VID 1209, "
              "PID 0001, produto 'VFDCLK 16C745').")
        return 1

    dispositivo = hid.device()
    dispositivo.open_path(caminho)
    try:
        if args.status:
            return 0 if mostrar_estado(dispositivo, espera_s=0.1) >= 0 else 4
        if args.alarme:
            return acao_alarme(dispositivo, args.alarme)
        return acao_sincronizar(dispositivo)
    finally:
        dispositivo.close()


if __name__ == "__main__":
    sys.exit(main())
