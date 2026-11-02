#pragma once

/*
 * free_shoot_mode.h
 *
 * Interface mínima da máquina Free Shoot.
 *
 * Todos os estados, linhas, cálculos e dados recuperáveis da página são
 * privados de free_shoot_mode.c. Nenhum outro módulo precisa conhecer ou
 * modificar o contexto. Essa decisão reduz o acoplamento e garante que somente
 * a task da máquina seja proprietária dos dados da sessão.
 */

#include "esp_err.h"

/**
 * @brief Inicializa fila, contexto, callbacks e task do Free Shoot.
 *
 * @return ESP_OK em sucesso ou o erro que impediu a inicialização.
 *
 * @pre hardware_inputs_init() deve ter sido executada.
 *
 * @note A função é idempotente após a primeira chamada bem-sucedida.
 *
 * Inicializa fila, contexto, callbacks e task.
 *
 * Pré-condição:
 *     hardware_inputs_init() já foi executada.
 *
 * A função é idempotente: chamadas posteriores ao primeiro sucesso retornam
 * ESP_OK sem criar recursos duplicados.
 */
esp_err_t free_shoot_mode_init(void);
