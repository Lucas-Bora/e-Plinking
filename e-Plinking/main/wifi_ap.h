#pragma once

/*
 * wifi_ap.h
 *
 * Interface pública do módulo Wi-Fi em modo Access Point.
 *
 * Este módulo cria a rede Wi-Fi do ESP32. Depois que a rede estiver ativa,
 * o usuário poderá conectar o celular/computador nela e acessar
 * http://192.168.4.1/ no navegador.
 */

/**
 * @brief Inicializa o ESP32 como Access Point.
 *
 * @note Usa as configurações definidas em app_config.h.
 *
 * Inicializa o Wi-Fi do ESP32 em modo Access Point.
 */
void wifi_ap_init(void);
