#pragma once

/*
 * target_protocol.h
 *
 * Pacote de 40 bytes enviado à página. Os campos de largura fixa são arrays de
 * uint8_t, não strings C: eles não possuem terminador NUL dentro do pacote.
 */

#include <stdbool.h>
#include <stdint.h>

#define TARGET_PACKET_STX  0x02
#define TARGET_PACKET_ETX  0x03
#define TARGET_PACKET_SIZE 40

typedef struct __attribute__((packed)) {
    uint8_t bytStx;
    uint8_t bytTotalShots[4];
    uint8_t bytTotalScore[4];
    uint8_t bytHitZone;
    uint8_t bytTableRow;
    uint8_t bytZone1Counter;
    uint8_t bytZone2Counter;
    uint8_t bytZone3Counter;
    uint8_t bytReactionTime[8];
    uint8_t bytRowShots[4];
    uint8_t bytAverageTime[8];
    uint8_t bytRowScore[5];
    uint8_t bytEtx;
} udtTargetPacket;

_Static_assert(
    sizeof(udtTargetPacket) == TARGET_PACKET_SIZE,
    "udtTargetPacket must contain exactly 40 bytes"
);

/**
 * @brief Valida os marcadores STX e ETX.
 *
 * @param ptrPacket Pacote que será inspecionado.
 *
 * @return true quando os delimitadores são válidos.
 */
bool target_packet_is_valid(const udtTargetPacket *ptrPacket);

/**
 * @brief Constrói o pacote completo de 40 bytes.
 *
 * @param ptrPacket Destino do pacote.
 * @param l32TotalShots Total geral de disparos.
 * @param l32TotalScore Pontuação geral.
 * @param bytHitZone Zona de 0 a 3.
 * @param bytTableRow Linha de destino.
 * @param bytZone1Hits Acertos da zona 1.
 * @param bytZone2Hits Acertos da zona 2.
 * @param bytZone3Hits Acertos da zona 3.
 * @param l64ReactionTimeUs Última reação.
 * @param l32RowShots Disparos da linha.
 * @param l64AverageTimeUs Média da linha.
 * @param l32RowScore Pontuação da linha.
 *
 * @return true quando o pacote foi montado.
 */
bool target_packet_build(
    udtTargetPacket *ptrPacket,
    uint32_t l32TotalShots,
    uint32_t l32TotalScore,
    uint8_t bytHitZone,
    uint8_t bytTableRow,
    uint8_t bytZone1Hits,
    uint8_t bytZone2Hits,
    uint8_t bytZone3Hits,
    uint64_t l64ReactionTimeUs,
    uint32_t l32RowShots,
    uint64_t l64AverageTimeUs,
    uint32_t l32RowScore
);
