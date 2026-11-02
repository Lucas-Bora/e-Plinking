#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "free_shoot_mode.h"
#include "hardware_inputs.h"
#include "storage.h"
#include "web_server.h"
#include "wifi_ap.h"

/*
 * app_main.c
 *
 * Ponto de entrada principal da aplicação ESP-IDF.
 *
 * Este arquivo ficou propositalmente pequeno porque a lógica foi separada
 * em módulos. A responsabilidade dele é apenas coordenar a inicialização
 * da aplicação na ordem correta.
 */

// Tag usada nos logs deste arquivo.
static const char *mptrTag = "APP_MAIN";

/*
 * Função principal chamada automaticamente pelo ESP-IDF.
 *
 * Ordem de inicialização:
 *
 * 1. storage_init()
 *    Inicializa a NVS, necessária para o Wi-Fi.
 *
 * 2. hardware_inputs_init()
 *    Configura GPIOs, ISRs e a fila FIFO, ainda com captura desabilitada.
 *
 * 3. free_shoot_mode_init()
 *    Cria a máquina, o contexto da página, a fila e registra callbacks.
 *
 * 4. wifi_ap_init()
 *    Cria a rede Wi-Fi do ESP32 em modo Access Point.
 *
 * 5. web_server_start()
 *    Inicia o servidor HTTP, registra a rota / e registra a rota WebSocket /ws.
 */
/**
 * @brief Inicializa os módulos do firmware na ordem exigida.
 *
 * @note A ordem é NVS, hardware, Free Shoot, Wi-Fi e servidor HTTP. O hardware
 * termina inicialmente fechado; somente o comando INICIAR habilita a captura.
 */
void app_main(void)
{
    // Inicializa a NVS antes do Wi-Fi.
    storage_init();

    /*
     * Inicializa primeiro a camada física. O módulo termina com as interrupções
     * desabilitadas; nenhum pulso é aceito antes do evento INICIAR.
     */
    esp_err_t udtInitializationResult = hardware_inputs_init();

    if (udtInitializationResult != ESP_OK) {
        /*
         * Não usamos ESP_ERROR_CHECK neste ponto.
         *
         * Uma configuração inválida de GPIO é um erro permanente de
         * configuração: reiniciar o processador não pode corrigi-la. Abortar
         * aqui criaria apenas um ciclo infinito de boot, panic e reboot.
         *
         * Ao retornar, o erro permanece claramente visível no console e o
         * processador fica estável para diagnóstico.
         */
        ESP_LOGE(
            mptrTag,
            "Hardware inputs not initialized: %s",
            esp_err_to_name(udtInitializationResult)
        );
        return;
    }

    /*
     * A máquina cria a fila de controle e a task proprietária do contexto.
     * Hardware e máquina capturam diretamente o mesmo relógio esp_timer.
     */
    udtInitializationResult = free_shoot_mode_init();

    if (udtInitializationResult != ESP_OK) {
        /*
         * A máquina depende da fila física e de sua própria task. Se a criação
         * de qualquer recurso falhar, iniciar Wi-Fi e aceitar comandos
         * produziria uma página aparentemente operacional sem uma máquina
         * funcional.
         */
        ESP_LOGE(
            mptrTag,
            "Free shoot mode not initialized: %s",
            esp_err_to_name(udtInitializationResult)
        );
        return;
    }

    // Inicializa o ESP32 como Access Point.
    wifi_ap_init();

    // Inicia o servidor HTTP e WebSocket.
    httpd_handle_t ptrHttpServer = web_server_start();

    // Se o servidor não iniciou, a aplicação não consegue atender a página web.
    if (ptrHttpServer == NULL) {
        ESP_LOGE(mptrTag, "HTTP server not started");
        return;
    }
}
