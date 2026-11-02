#include "storage.h"

#include "esp_err.h"
#include "nvs_flash.h"

/*
 * Inicializa a NVS.
 *
 * A NVS, Non-Volatile Storage, é uma área de memória flash usada
 * pelo ESP-IDF para guardar informações persistentes.
 *
 * O Wi-Fi usa a NVS internamente. Portanto, se ela não estiver pronta,
 * a inicialização do Wi-Fi pode falhar.
 */
/**
 * @brief Inicializa a partição NVS usada pela pilha Wi-Fi.
 *
 * @note Se a partição estiver cheia ou tiver versão incompatível, ela é
 * apagada e inicializada novamente. Falhas finais interrompem a inicialização
 * por ESP_ERROR_CHECK.
 */
void storage_init(void)
{
    // Tenta inicializar a NVS normalmente.
    esp_err_t udtResult = nvs_flash_init();

    /*
     * Existem dois erros comuns em desenvolvimento:
     *
     * 1. ESP_ERR_NVS_NO_FREE_PAGES
     *    A partição NVS ficou sem páginas livres.
     *
     * 2. ESP_ERR_NVS_NEW_VERSION_FOUND
     *    A NVS foi gravada por uma versão diferente/incompatível.
     *
     * Nesses casos, apagamos a NVS e inicializamos novamente.
     */
    if (udtResult == ESP_ERR_NVS_NO_FREE_PAGES ||
        udtResult == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        udtResult = nvs_flash_init();
    }

    // Interrompe a execução se a NVS ainda não puder ser inicializada.
    ESP_ERROR_CHECK(udtResult);
}
