#pragma once

/*
 * hardware_inputs.h
 *
 * Camada física das três entradas digitais do alvo.
 *
 * O módulo transforma cada borda de descida aceita em um item independente da
 * fila. A fila preserva a ordem e permite registrar ocorrências repetidas da
 * mesma entrada.
 *
 * O instante do disparo é capturado diretamente com esp_timer_get_time().
 * Portanto, não existe módulo intermediário de cronômetro, callback de tempo
 * ou uso do Tick do FreeRTOS como timestamp.
 *
 * Pontuação, rodada, cálculo de reação, página e WebSocket pertencem ao módulo
 * free_shoot_mode e não são tratados aqui.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/* Identificadores lógicos usados no lugar dos números físicos dos GPIOs. */
typedef enum {
    HARDWARE_INPUT_1 = 0,
    HARDWARE_INPUT_2,
    HARDWARE_INPUT_3,
    HARDWARE_INPUT_COUNT
} udtHardwareInputId;

/*
 * Registro imutável copiado pela ISR para a fila.
 *
 * udtInputId: entrada que produziu a borda.
 * l32Sequence: ordem de captura, iniciada em 1 a cada nova sessão.
 * l64TimestampUs: leitura absoluta de esp_timer_get_time(); não é Tick do
 * FreeRTOS.
 */
typedef struct {
    udtHardwareInputId udtInputId;
    uint32_t l32Sequence;
    uint64_t l64TimestampUs;
} udtHardwareInputEvent;

/**
 * @brief Cria a fila, configura os GPIOs e registra uma ISR genérica.
 *
 * @return ESP_OK em sucesso ou erro de memória/driver.
 */
esp_err_t hardware_inputs_init(void);

/**
 * @brief Habilita ou desabilita a captura física.
 *
 * @param blnEnabled true para abrir os GPIOs; false para fechá-los.
 *
 * @return ESP_OK ou o erro devolvido pelo driver.
 *
 * Abre ou fecha as interrupções das três entradas.
 * A fila existente não é apagada automaticamente.
 */
esp_err_t hardware_inputs_set_capture_enabled(bool blnEnabled);

/**
 * @brief Retira o evento mais antigo da FIFO.
 *
 * @param ptrEvent Destino do evento.
 * @param udtTicksToWait Timeout de espera da task.
 *
 * @return true quando um item foi copiado.
 *
 * Copia o evento mais antigo para ptrEvent.
 * udtTicksToWait controla apenas a espera da task, nunca o timestamp.
 */
bool hardware_inputs_receive_event(
    udtHardwareInputEvent *ptrEvent,
    TickType_t udtTicksToWait
);

/**
 * @brief Descarta a fila e reinicia sequência e debounce.
 */
void hardware_inputs_clear_queue(void);

/**
 * @brief Consulta perdas de eventos físicos.
 *
 * @return Quantidade de eventos descartados porque a fila estava cheia.
 */
uint32_t hardware_inputs_get_dropped_count(void);

/**
 * @brief Zera o contador de eventos físicos descartados.
 */
void hardware_inputs_reset_dropped_count(void);
