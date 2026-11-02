#include "target_protocol.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Satura um valor para impedir que ele ultrapasse a largura do protocolo. */
/**
 * @brief Limita um inteiro ao maior valor aceito pelo campo de destino.
 *
 * @param l32Value Valor solicitado pela aplicação.
 * @param l32Maximum Limite representável pelo protocolo.
 *
 * @return O valor original quando válido ou l32Maximum quando excedido.
 */
static uint32_t target_protocol_clamp_u32(
    uint32_t l32Value,
    uint32_t l32Maximum
)
{
    return l32Value > l32Maximum ? l32Maximum : l32Value;
}

/*
 * Escreve MM:SSmmm em oito bytes, sem NUL.
 * O buffer temporário possui o nono byte exigido por snprintf().
 */
/**
 * @brief Converte microssegundos para o campo fixo MM:SSmmm.
 *
 * @param ptrDestination Campo de oito bytes sem terminador NUL.
 * @param l64TimeUs Tempo solicitado em microssegundos.
 *
 * @note Valores acima de 99:59.999 são saturados; microssegundos inferiores a
 * um milissegundo são truncados.
 */
static void target_protocol_format_time(
    uint8_t ptrDestination[8],
    uint64_t l64TimeUs
)
{
    const uint64_t l64MaximumMs =
        (99ULL * 60ULL * 1000ULL) + 59999ULL;
    uint64_t l64TotalMs = l64TimeUs / 1000ULL;

    if (l64TotalMs > l64MaximumMs) {
        l64TotalMs = l64MaximumMs;
    }

    const uint32_t l32Minutes = (uint32_t)(l64TotalMs / 60000ULL);
    const uint32_t l32Seconds =
        (uint32_t)((l64TotalMs / 1000ULL) % 60ULL);
    const uint32_t l32Milliseconds = (uint32_t)(l64TotalMs % 1000ULL);
    uint8_t bytBuffer[9];

    (void)snprintf(
        (char *)bytBuffer,
        sizeof(bytBuffer),
        "%02lu:%02lu%03lu",
        (unsigned long)l32Minutes,
        (unsigned long)l32Seconds,
        (unsigned long)l32Milliseconds
    );

    memcpy(ptrDestination, bytBuffer, 8);
}

/* Converte um inteiro para um campo decimal fixo preenchido com zeros. */
/**
 * @brief Formata um inteiro decimal com largura fixa e zeros à esquerda.
 *
 * @param ptrDestination Campo de destino sem terminador NUL.
 * @param udtWidth Quantidade exata de bytes do campo.
 * @param l32Value Valor solicitado pela aplicação.
 * @param l32Maximum Maior valor aceito para a largura.
 */
static void target_protocol_format_uint(
    uint8_t *ptrDestination,
    size_t udtWidth,
    uint32_t l32Value,
    uint32_t l32Maximum
)
{
    uint8_t bytBuffer[16];
    const uint32_t l32SafeValue =
        target_protocol_clamp_u32(l32Value, l32Maximum);

    (void)snprintf(
        (char *)bytBuffer,
        sizeof(bytBuffer),
        "%0*lu",
        (int)udtWidth,
        (unsigned long)l32SafeValue
    );

    memcpy(ptrDestination, bytBuffer, udtWidth);
}

/**
 * @brief Verifica os delimitadores básicos de um pacote.
 *
 * @param ptrPacket Pacote que será inspecionado.
 *
 * @return true quando o ponteiro e os bytes STX/ETX são válidos.
 */
bool target_packet_is_valid(const udtTargetPacket *ptrPacket)
{
    return ptrPacket != NULL &&
           ptrPacket->bytStx == TARGET_PACKET_STX &&
           ptrPacket->bytEtx == TARGET_PACKET_ETX;
}

/**
 * @brief Monta integralmente o pacote de 40 bytes usado pela página.
 *
 * @param ptrPacket Destino que será zerado e preenchido.
 * @param l32TotalShots Total geral de disparos.
 * @param l32TotalScore Pontuação geral.
 * @param bytHitZone Zona a destacar, de 0 a 3.
 * @param bytTableRow Linha da tabela, de 1 a 9 pelo formato.
 * @param bytZone1Hits Contador da zona 1.
 * @param bytZone2Hits Contador da zona 2.
 * @param bytZone3Hits Contador da zona 3.
 * @param l64ReactionTimeUs Último tempo de reação da linha.
 * @param l32RowShots Disparos acumulados na linha.
 * @param l64AverageTimeUs Tempo médio da linha.
 * @param l32RowScore Pontuação acumulada na linha.
 *
 * @return true quando o pacote foi construído e validado.
 *
 * @note Campos excedentes são saturados, nunca truncados ambiguamente.
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
)
{
    if (ptrPacket == NULL ||
        bytHitZone > 3 ||
        bytTableRow < 1 ||
        bytTableRow > 9) {
        return false;
    }

    memset(ptrPacket, 0, sizeof(*ptrPacket));
    ptrPacket->bytStx = TARGET_PACKET_STX;

    target_protocol_format_uint(
        ptrPacket->bytTotalShots, 4, l32TotalShots, 9999
    );
    target_protocol_format_uint(
        ptrPacket->bytTotalScore, 4, l32TotalScore, 9999
    );

    ptrPacket->bytHitZone = (uint8_t)('0' + bytHitZone);
    ptrPacket->bytTableRow = (uint8_t)('0' + bytTableRow);
    ptrPacket->bytZone1Counter = (uint8_t)(
        '0' + target_protocol_clamp_u32(bytZone1Hits, 9)
    );
    ptrPacket->bytZone2Counter = (uint8_t)(
        '0' + target_protocol_clamp_u32(bytZone2Hits, 9)
    );
    ptrPacket->bytZone3Counter = (uint8_t)(
        '0' + target_protocol_clamp_u32(bytZone3Hits, 9)
    );

    target_protocol_format_time(
        ptrPacket->bytReactionTime,
        l64ReactionTimeUs
    );
    target_protocol_format_uint(
        ptrPacket->bytRowShots, 4, l32RowShots, 9999
    );
    target_protocol_format_time(
        ptrPacket->bytAverageTime,
        l64AverageTimeUs
    );
    target_protocol_format_uint(
        ptrPacket->bytRowScore, 5, l32RowScore, 99999
    );

    ptrPacket->bytEtx = TARGET_PACKET_ETX;
    return target_packet_is_valid(ptrPacket);
}
