/**
 * =====================================================================
 *  vfd.h — Driver do display VFD IEE Century Series 036X2 (20x2)
 * =====================================================================
 *  Interface: SERIAL 9600 baud 8N1 (jumpers de personalidade do módulo
 *  em SERIAL / 9600 — ver HARDWARE/pinagem.md).
 *
 *  Em modo serial o display é somente-escrita (o spec proíbe leitura),
 *  e não há linha BUSY: o handshake é garantido por aritmética — a
 *  9600 baud um caractere leva ~1,04 ms na linha, e o comando mais
 *  lento do 036X2 20x2 executa em <0,9 ms (tabela 3-5 do spec S036X2).
 *  Ou seja, o display sempre termina um comando antes do próximo chegar.
 * =====================================================================
 */
#ifndef VFD_H
#define VFD_H

#include <stdint.h>

#define VFD_COLUNAS  20u
#define VFD_LINHAS   2u

/* Níveis de brilho do 036X2: 0 = máximo ... 7 = mínimo (spec 4.5).   */
#define VFD_BRILHO_MAXIMO   0u
#define VFD_BRILHO_MINIMO   7u

/* Reinicializa o display (reset por software) e o deixa limpo, com
 * cursor invisível e rolagem vertical padrão. Aguarda os 500 ms de
 * power-up exigidos pelo spec (seção 3.3) antes de falar com ele.    */
void vfd_iniciar(void);

/* Apaga a tela e leva o cursor para a posição inicial (código 15h).  */
void vfd_limpar(void);

/* Posiciona o cursor. linha: 0..1, coluna: 0..19 (comando 1Bh + ID,
 * posições numeradas da esquerda p/ direita, de cima p/ baixo).      */
void vfd_cursor(uint8_t linha, uint8_t coluna);

/* Escreve um caractere ASCII imprimível na posição atual do cursor.  */
void vfd_escrever_char(char c);

/* Escreve uma string C (terminada em '\0') a partir do cursor atual. */
void vfd_escrever_texto(const char *texto);

/* Substitui uma linha inteira (20 colunas): escreve o texto e completa
 * com espaços. Evita usar "clear" a cada atualização (sem piscadas). */
void vfd_linha(uint8_t linha, const char *texto);

/* Ajusta o brilho de toda a tela: 0 (máximo) a 7 (mínimo).
 * Sequência serial: 19h (A0 alto p/ próximo byte) 30h FFh nível.     */
void vfd_brilho(uint8_t nivel);

#endif /* VFD_H */
