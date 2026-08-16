#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=======================================================================
 dtc_sync.py — Acerta o RTC e o alarme do relógio VFD (PIC16C745)
=======================================================================
Uso (manual, sempre que quiser):

    python dtc_sync.py                 # sincroniza com a hora do PC (padrão)
    python dtc_sync.py --ntp           # sincroniza com a hora oficial (internet)
    python dtc_sync.py --status        # só mostra o estado do relógio
    python dtc_sync.py --alarme 07:30  # programa o alarme e liga
    python dtc_sync.py --alarme on     # liga o alarme (mantém horário)
    python dtc_sync.py --alarme off    # desliga o alarme

HORA PELA INTERNET (--ntp)
    Consulta um servidor de tempo por SNTP (RFC 4330, UDP porta 123) —
    o mesmo protocolo que o Windows usa em "sincronizar com a hora da
    internet". Não é uma API HTTP: o SNTP mede o tempo de ida-e-volta do
    pacote e desconta metade dele, chegando a erro de poucos
    milissegundos; num GET HTTP a latência da requisição vai embutida na
    resposta, sem como descontar. E usa só a biblioteca padrão do Python
    (socket + struct), sem dependência nova.

    Servidores padrão: a.ntp.br / b.ntp.br (NTP.br — projeto do NIC.br
    que distribui a hora legal brasileira, sincronizada com o
    Observatório Nacional), com pool.ntp.org de reserva. Trocável com
    --ntp-servidor. O stratum efetivamente atendido é impresso a cada
    consulta.

    ATENÇÃO AO FUSO: o NTP entrega tempo ABSOLUTO (UTC). A conversão
    para hora local — que é o que o DS3231 guarda — continua saindo do
    fuso configurado no PC. NTP conserta relógio atrasado, não fuso
    horário errado.

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
import socket
import struct
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

# --- SNTP (RFC 4330) -------------------------------------------------
# Servidores tentados em ordem até um responder. Os dois primeiros são
# do NTP.br (NIC.br), que distribui a hora legal brasileira; o pool
# internacional fica de reserva para quando os de cá estiverem
# inacessíveis. Qualquer um deles é ordens de grandeza mais preciso do
# que o DS3231 precisa (resolução de 1 s).
NTP_SERVIDORES = ("a.ntp.br", "b.ntp.br", "pool.ntp.org")
NTP_PORTA = 123
NTP_TIMEOUT_S = 3.0
NTP_TAM_PACOTE = 48
# Segundos entre a época do NTP (1900-01-01) e a do Unix (1970-01-01).
NTP_DELTA_EPOCA = 2_208_988_800


def para_bcd(valor: int) -> int:
    """Converte 0..99 decimal para BCD (ex.: 37 -> 0x37)."""
    return ((valor // 10) << 4) | (valor % 10)


def de_bcd(valor: int) -> int:
    """Converte BCD para decimal (ex.: 0x37 -> 37)."""
    return ((valor >> 4) * 10) + (valor & 0x0F)


class ErroNTP(Exception):
    """Falha ao obter a hora de um servidor de tempo."""


def _ntp_para_unix(bruto: bytes) -> float:
    """Timestamp NTP de 64 bits (32.32 ponto fixo) -> segundos Unix."""
    segundos, fracao = struct.unpack("!II", bruto)
    return (segundos - NTP_DELTA_EPOCA) + (fracao / 2.0 ** 32)


def _unix_para_ntp(instante: float) -> bytes:
    """Segundos Unix -> timestamp NTP de 64 bits."""
    segundos = int(instante) + NTP_DELTA_EPOCA
    fracao = int((instante - int(instante)) * 2.0 ** 32)
    return struct.pack("!II", segundos & 0xFFFFFFFF, fracao & 0xFFFFFFFF)


def consultar_ntp(servidor: str, timeout: float = NTP_TIMEOUT_S):
    """Consulta um servidor SNTP. Devolve (offset, atraso, stratum).

    'offset' é quanto o relógio do PC está ERRADO, em segundos: somar
    esse valor a time.time() dá a hora correta (positivo = o PC está
    atrasado). 'atraso' é o tempo de ida-e-volta do pacote, uma medida
    da qualidade da consulta.

    Álgebra padrão do NTP com quatro marcas de tempo:
        t1 = saída do pedido (relógio do PC)
        t2 = chegada no servidor      } vêm dentro
        t3 = saída da resposta        } da resposta
        t4 = chegada da resposta (relógio do PC)
        offset = ((t2 - t1) + (t3 - t4)) / 2
        atraso = (t4 - t1) - (t3 - t2)
    A média das duas diferenças cancela o tempo de trânsito, desde que
    ida e volta demorem aproximadamente o mesmo — por isso o SNTP acerta
    em milissegundos onde um GET HTTP erraria pela latência inteira.
    """
    pedido = bytearray(NTP_TAM_PACOTE)
    pedido[0] = 0x23          # LI=0 (sem aviso), VN=4 (NTPv4), Mode=3 (cliente)

    t1 = time.time()
    pedido[40:48] = _unix_para_ntp(t1)   # transmit timestamp (volta ecoado)

    try:
        alvo = socket.getaddrinfo(servidor, NTP_PORTA,
                                  type=socket.SOCK_DGRAM)[0]
    except socket.gaierror as erro:
        raise ErroNTP(f"não resolveu o nome ({erro})") from erro

    familia, tipo, protocolo, _, endereco = alvo
    with socket.socket(familia, tipo, protocolo) as sock:
        sock.settimeout(timeout)
        try:
            sock.sendto(bytes(pedido), endereco)
            resposta, _ = sock.recvfrom(256)
        except socket.timeout as erro:
            raise ErroNTP("sem resposta (timeout)") from erro
        except OSError as erro:
            raise ErroNTP(f"erro de rede ({erro})") from erro
        t4 = time.time()

    if len(resposta) < NTP_TAM_PACOTE:
        raise ErroNTP("resposta curta demais")

    modo = resposta[0] & 0x07
    if modo != 4:                      # 4 = servidor
        raise ErroNTP(f"resposta não é de servidor (modo {modo})")

    stratum = resposta[1]
    if stratum == 0:
        # Kiss-o'-Death: o servidor está recusando (limite de taxa, etc.).
        codigo = bytes(resposta[12:16]).decode("ascii", "replace").strip("\0")
        raise ErroNTP(f"servidor recusou o pedido (KoD '{codigo}')")
    if stratum > 15:
        raise ErroNTP(f"servidor sem sincronismo próprio (stratum {stratum})")

    # O originate timestamp deve ser o eco exato do que enviamos: garante
    # que esta é a resposta do NOSSO pedido, e não um pacote atrasado.
    if bytes(resposta[24:32]) != bytes(pedido[40:48]):
        raise ErroNTP("resposta não corresponde ao pedido")

    t2 = _ntp_para_unix(bytes(resposta[32:40]))   # receive timestamp
    t3 = _ntp_para_unix(bytes(resposta[40:48]))   # transmit timestamp
    if t3 == 0.0:
        raise ErroNTP("servidor devolveu timestamp zerado")

    offset = ((t2 - t1) + (t3 - t4)) / 2.0
    atraso = (t4 - t1) - (t3 - t2)
    return offset, atraso, stratum


def obter_offset_ntp(servidores):
    """Tenta os servidores em ordem e devolve (offset, servidor) do
    primeiro que responder. Levanta ErroNTP se nenhum responder."""
    falhas = []
    for servidor in servidores:
        print(f"Consultando servidor de tempo {servidor} ...")
        try:
            offset, atraso, stratum = consultar_ntp(servidor)
        except ErroNTP as erro:
            print(f"  falhou: {erro}")
            falhas.append(f"{servidor}: {erro}")
            continue

        print(f"  respondeu (stratum {stratum}, ida-e-volta "
              f"{atraso * 1000:.0f} ms)")
        if abs(offset) < 0.5:
            print(f"  relógio do PC confere (desvio {offset * 1000:+.0f} ms)")
        else:
            sentido = "atrasado" if offset > 0 else "adiantado"
            print(f"  relógio do PC está {abs(offset):.1f} s {sentido} "
                  "— será corrigido no envio")
        return offset, servidor

    raise ErroNTP("nenhum servidor respondeu -> " + "; ".join(falhas))


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


def esperar_virada_do_segundo(offset: float) -> datetime:
    """Espera a próxima virada de segundo e devolve a hora local nesse
    instante, já com o offset do NTP aplicado.

    Enviar logo depois da virada deixa o DS3231 alinhado com a origem do
    tempo (a defasagem que sobra é só o trânsito USB + escrita I2C).
    Com --ntp, a virada perseguida é a do tempo CORRIGIDO, não a do
    relógio do PC — que pode estar em outro ponto do segundo.
    """
    alvo = int(time.time() + offset) + 1
    while True:
        restante = alvo - (time.time() + offset)
        if restante <= 0:
            break
        time.sleep(min(restante, 0.005))
    return datetime.fromtimestamp(time.time() + offset)


def acao_sincronizar(dispositivo, offset: float = 0.0,
                     fonte: str = "relógio do PC") -> int:
    """Envia a hora, alinhada com a virada do segundo.

    'offset' (em segundos) corrige o relógio do PC com o resultado da
    consulta NTP; 0.0 significa usar a hora do PC como está.
    """
    agora = esperar_virada_do_segundo(offset)

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
          f"{agora.strftime('%A %d/%m/%Y %H:%M:%S')}  (fonte: {fonte})")

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
    ap.add_argument("--ntp", action="store_true",
                    help="pega a hora num servidor de tempo da internet "
                         "(SNTP) em vez de usar o relógio do PC")
    ap.add_argument("--ntp-servidor", metavar="HOST", dest="ntp_servidor",
                    help="servidor SNTP a consultar (implica --ntp); "
                         "padrão: " + ", ".join(NTP_SERVIDORES))
    args = ap.parse_args()

    usar_ntp = args.ntp or bool(args.ntp_servidor)
    if usar_ntp and (args.status or args.alarme):
        print("AVISO: --ntp só se aplica à sincronização de hora; ignorado.")
        usar_ntp = False

    caminho = achar_dispositivo()
    if caminho is None:
        print("Relógio VFD não encontrado no USB.")
        print("Confira o cabo e se o dispositivo enumerou (VID 1209, "
              "PID 0001, produto 'VFDCLK 16C745').")
        return 1

    # A consulta de rede vem ANTES de abrir o dispositivo: se a internet
    # falhar, saímos sem ter mexido no relógio. E não caímos em silêncio
    # para a hora do PC — quem pediu --ntp quer a hora oficial, não uma
    # aproximação; para usar a do PC, basta rodar sem a opção.
    offset, fonte = 0.0, "relógio do PC"
    if usar_ntp:
        servidores = ((args.ntp_servidor,) if args.ntp_servidor
                      else NTP_SERVIDORES)
        try:
            offset, servidor = obter_offset_ntp(servidores)
        except ErroNTP as erro:
            print(f"ERRO: não consegui a hora da internet — {erro}")
            print("Confira a conexão. O SNTP usa UDP porta 123, que algumas "
                  "redes corporativas bloqueiam — nesse caso use "
                  "--ntp-servidor com o servidor de tempo da própria rede, "
                  "ou rode sem --ntp para usar a hora do PC.")
            return 6
        fonte = f"NTP {servidor}"

    dispositivo = hid.device()
    dispositivo.open_path(caminho)
    try:
        if args.status:
            return 0 if mostrar_estado(dispositivo, espera_s=0.1) >= 0 else 4
        if args.alarme:
            return acao_alarme(dispositivo, args.alarme)
        return acao_sincronizar(dispositivo, offset, fonte)
    finally:
        dispositivo.close()


if __name__ == "__main__":
    sys.exit(main())
