#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=======================================================================
 teste_conversao.py — Valida a aritmética de PONTO FIXO do firmware
=======================================================================
O firmware (VFDCLK.X/sht1x.c) converte as leituras brutas do SHT15
sem usar ponto flutuante (caro demais num PIC16 de 8 K words). Este
script reproduz EXATAMENTE a aritmética inteira do C e a compara com
as fórmulas float oficiais da Sensirion (datasheet SHT1x, coefs. V4):

    T[°C]      = -40,1 + 0,01·SO_T                      (14 bits @ 5 V)
    RH_lin[%]  = -2,0468 + 0,0367·SO_RH - 1,5955e-6·SO_RH²   (12 bits)
    RH_true[%] = (T - 25)·(0,01 + 8e-5·SO_RH) + RH_lin

Critério de aprovação (folga sobre a resolução exibida de 0,1):
    |erro de temperatura| <= 0,06 °C
    |erro de umidade|     <= 0,20 %RH

Rode com:  python teste_conversao.py
=======================================================================
"""

import sys

# Coeficientes oficiais Sensirion (V4, 12 bits RH / 14 bits T @ 5 V)
D1, D2 = -40.1, 0.01
C1, C2, C3 = -2.0468, 0.0367, -1.5955e-6
T1, T2 = 0.01, 0.00008


def firmware_t10(so_t: int) -> int:
    """Réplica exata de sht1x.c: t10 = (SO_T + 5)/10 - 401."""
    return (so_t + 5) // 10 - 401


def firmware_rh10(so_rh: int, t10: int) -> int:
    """Réplica exata de sht1x.c (inteiros de 32 bits, centi-%RH)."""
    rh100 = (15032 * so_rh) >> 12          # ~ 3,66992·SO  (100·C2)
    rh100 -= (so_rh * so_rh) // 6268       # ~ 1,5954e-4·SO² (-100·C3)
    rh100 -= 205                           # ~ -2,05 (100·C1)
    # compensação de temperatura (o C usa shift aritmético, que o
    # Python reproduz: >> em int negativo também arredonda p/ baixo)
    rh100 += ((t10 - 250) * (1638 + 13 * so_rh)) >> 14
    rh100 = max(10, min(10000, rh100))     # recorte 0,1..100 %
    return (rh100 + 5) // 10               # décimos de %RH


def referencia_float(so_t: int, so_rh: int):
    """Fórmulas float oficiais da Sensirion (sample code V2.4)."""
    t_c = D1 + D2 * so_t
    rh_lin = C1 + C2 * so_rh + C3 * so_rh * so_rh
    rh_true = (t_c - 25.0) * (T1 + T2 * so_rh) + rh_lin
    rh_true = max(0.1, min(100.0, rh_true))
    return t_c, rh_true


def main() -> int:
    erro_t_max = 0.0
    erro_rh_max = 0.0
    pior_t = pior_rh = None

    # Temperatura: varre toda a faixa de 14 bits
    for so_t in range(0, 16384):
        t10 = firmware_t10(so_t)
        t_ref = D1 + D2 * so_t
        erro = abs(t10 / 10.0 - t_ref)
        if erro > erro_t_max:
            erro_t_max, pior_t = erro, so_t

    # Umidade: toda a faixa de 12 bits, cruzada com temperaturas
    # representativas de -30 °C a +80 °C
    temps_teste = [-301, -200, -100, 0, 100, 200, 250, 300,
                   400, 500, 600, 700, 800]          # em décimos de °C
    for so_rh in range(0, 4096):
        for t10 in temps_teste:
            rh10 = firmware_rh10(so_rh, t10)
            t_c = t10 / 10.0
            rh_lin = C1 + C2 * so_rh + C3 * so_rh * so_rh
            rh_ref = (t_c - 25.0) * (T1 + T2 * so_rh) + rh_lin
            rh_ref = max(0.1, min(100.0, rh_ref))
            erro = abs(rh10 / 10.0 - rh_ref)
            if erro > erro_rh_max:
                erro_rh_max, pior_rh = erro, (so_rh, t10)

    print(f"Erro máximo de temperatura: {erro_t_max:.4f} °C "
          f"(em SO_T={pior_t})")
    print(f"Erro máximo de umidade    : {erro_rh_max:.4f} %RH "
          f"(em SO_RH={pior_rh[0]}, T={pior_rh[1] / 10:.1f} °C)")

    ok = (erro_t_max <= 0.06) and (erro_rh_max <= 0.20)
    print("RESULTADO: " + ("APROVADO" if ok else "REPROVADO"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
