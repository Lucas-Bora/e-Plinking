#pragma once

/*
 * storage.h
 *
 * Interface pública do módulo de armazenamento/NVS.
 *
 * No ESP-IDF, o Wi-Fi depende da NVS para armazenar dados internos
 * de configuração e calibração. Por isso, a NVS precisa ser inicializada
 * antes de iniciar o driver Wi-Fi.
 */

/**
 * @brief Inicializa a NVS necessária à pilha Wi-Fi.
 *
 * @note Deve ser chamada antes de wifi_ap_init().
 *
 * Inicializa a memória NVS usada pelo ESP-IDF.
 *
 * Esta função deve ser chamada uma vez no início da aplicação,
 * antes de wifi_ap_init().
 */
void storage_init(void);
