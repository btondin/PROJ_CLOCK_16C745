/**
 * =====================================================================
 *  vfd.h — Driver do display VFD IEE Century Series 036X2 (20x2)
 * =====================================================================
 *  Interface: SERIAL 19200 baud 8N1 (jumper de personalidade do módulo
 *  em 19200 — ver HARDWARE/pinagem.md).
 *
 *  Em modo serial o display é somente-escrita (o spec proíbe leitura),
 *  e não há linha BUSY: o handshake é garantido por aritmética — a
 *  19200 baud um caractere ocupa 521 us na linha, e os comandos que
 *  este driver usa em regime executam bem abaixo disso (tabela 3-5 do
 *  spec S036X2): "Send Character" e "Cursor Locate" levam ~170 us tanto
 *  no 2x20 quanto no 2x40. Ou seja, o display sempre termina antes do
 *  próximo byte chegar.
 *
 *  EXCEÇÕES que NÃO cabem em 521 us e por isso levam espera explícita
 *  no código: "Clear" (410 us no 2x20, 715 us no 2x40) e "Reset"
 *  (~800 us) — ambos só acontecem em vfd_iniciar(), que já aguarda.
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

/* Substitui uma linha inteira: escreve o texto e completa com espaços.
 * Use para (re)desenhar uma tela inteira.                            */
void vfd_linha(uint8_t linha, const char *texto);

/* Escreve um trecho curto em (linha, coluna) SEM mexer no resto da
 * linha. Use para atualizar só o campo que muda (ex.: os dígitos do
 * relógio), evitando a piscada de repintar a tela toda.              */
void vfd_escrever_em(uint8_t linha, uint8_t coluna, const char *texto);

/* Atualiza um campo de DOIS DÍGITOS BCD (ex.: "07") escrevendo apenas
 * o(s) dígito(s) que mudaram entre 'antigo' e 'novo'. Se nada mudou,
 * não envia byte nenhum. É o que mantém o relógio sem piscar: na
 * maioria dos segundos só a unidade muda -> 1 caractere reescrito.   */
void vfd_campo_bcd(uint8_t linha, uint8_t coluna, uint8_t novo, uint8_t antigo);

/* Ajusta o brilho de toda a tela: 0 (máximo) a 7 (mínimo).
 * Sequência serial: 19h (A0 alto p/ próximo byte) 30h FFh nível.     */
void vfd_brilho(uint8_t nivel);

/* Taxas de piscada aceitas pelo 036X2 (spec 4.5, código 31h)         */
#define VFD_BLINK_DESLIGA   0x00u
#define VFD_BLINK_1HZ       0x01u
#define VFD_BLINK_2HZ       0x02u
#define VFD_BLINK_4HZ       0x04u

/* Liga o atributo "piscante" para os caracteres escritos A PARTIR
 * daqui (usado na tela de alarme). Chamar vfd_piscar_fim() depois.   */
void vfd_piscar_inicio(uint8_t taxa);

/* Encerra o trecho piscante iniciado por vfd_piscar_inicio().        */
void vfd_piscar_fim(void);

#endif /* VFD_H */
