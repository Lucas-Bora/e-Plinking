// Interface pública do próprio módulo.
#include "wifi_ap.h"

#include <string.h> // strlen(): tamanho de SSID e verificação de senha vazia.

#include "esp_err.h"   // ESP_ERROR_CHECK e esp_err_t.
#include "esp_event.h" // Loop e registro de eventos Wi-Fi.
#include "esp_log.h"   // Logs de conexão/desconexão e configuração.
#include "esp_mac.h"   // MACSTR/MAC2STR para imprimir o cliente.
#include "esp_netif.h" // Interface de rede padrão do Access Point.
#include "esp_wifi.h"  // Driver, configuração e eventos do rádio Wi-Fi.

#include "app_config.h"        // SSID, senha, canal e limite de clientes.
#include "websocket_service.h" // Invalidação do socket após queda da estação.

// Tag usada nas mensagens de log deste módulo.
static const char *mptrTag = "WIFI_AP";

/*
 * Handler de eventos Wi-Fi.
 *
 * O ESP-IDF chama esta função automaticamente quando eventos Wi-Fi ocorrem.
 * Neste firmware, tratamos principalmente:
 * - cliente conectado ao Access Point;
 * - cliente desconectado do Access Point.
 */
/**
 * @brief Trata conexão e desconexão de estações no Access Point.
 *
 * @param ptrArgument Argumento opcional do registro; não utilizado.
 * @param udtEventBase Base do evento; não utilizada porque o handler é
 * registrado exclusivamente para eventos Wi-Fi.
 * @param l32EventId Identificador do evento recebido.
 * @param ptrEventData Estrutura específica do evento.
 *
 * @note Ao desconectar uma estação, invalida o WebSocket associado.
 */
static void wifi_event_handler(
    void *ptrArgument,
    esp_event_base_t udtEventBase,
    int32_t l32EventId,
    void *ptrEventData
)
{
    // Parâmetros não usados neste handler.
    (void)ptrArgument;
    (void)udtEventBase;

    // Evento: uma estação, como celular ou computador, conectou ao AP do ESP32.
    if (l32EventId == WIFI_EVENT_AP_STACONNECTED) {
        // event_data possui este layout especificamente para STACONNECTED.
        wifi_event_ap_staconnected_t *ptrEvent =
            (wifi_event_ap_staconnected_t *)ptrEventData;

        ESP_LOGI(
            mptrTag,
            "Wi-Fi client connected. MAC: " MACSTR ", AID=%d",
            MAC2STR(ptrEvent->mac),
            ptrEvent->aid
        );

        return;
    }

    // Evento: uma estação desconectou do AP do ESP32.
    if (l32EventId == WIFI_EVENT_AP_STADISCONNECTED) {
        // Converte somente dentro do ramo correspondente ao tipo do evento.
        wifi_event_ap_stadisconnected_t *ptrEvent =
            (wifi_event_ap_stadisconnected_t *)ptrEventData;

        ESP_LOGI(
            mptrTag,
            "Wi-Fi client disconnected. MAC: " MACSTR ", AID=%d",
            MAC2STR(ptrEvent->mac),
            ptrEvent->aid
        );

        /*
         * Se o cliente Wi-Fi caiu, o WebSocket associado também deixa
         * de ser confiável. Por isso limpamos o cliente WebSocket salvo.
         */
        websocket_service_clear_client();

        return;
    }

    // Outros eventos Wi-Fi são deliberadamente ignorados por este módulo.
}

/*
 * Inicializa o ESP32 como Access Point.
 *
 * Fluxo:
 * 1. Inicializa a camada de rede do ESP-IDF.
 * 2. Cria o loop padrão de eventos.
 * 3. Cria a interface Wi-Fi AP.
 * 4. Inicializa o driver Wi-Fi.
 * 5. Registra o handler de eventos Wi-Fi.
 * 6. Configura SSID, senha, canal e limite de clientes.
 * 7. Coloca o Wi-Fi em modo AP.
 * 8. Inicia o Wi-Fi.
 */
/**
 * @brief Inicializa rede, driver e Access Point Wi-Fi.
 *
 * @note A função aplica SSID, senha, canal e limite definidos em app_config.h.
 * Qualquer falha obrigatória é tratada por ESP_ERROR_CHECK.
 */
void wifi_ap_init(void)
{
    // Inicializa a pilha de rede do ESP-IDF.
    ESP_ERROR_CHECK(esp_netif_init());

    // Cria o loop padrão de eventos usado por Wi-Fi, IP e outros componentes.
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Cria a interface de rede padrão para Wi-Fi no modo Access Point.
    /*
     * Cria netif, handlers padrão e configuração de IP do AP. O retorno não
     * precisa ser guardado porque esta versão não altera IP/DHCP posteriormente.
     */
    esp_netif_create_default_wifi_ap();

    // Carrega a configuração padrão de inicialização do driver Wi-Fi.
    wifi_init_config_t udtWifiInitialization = WIFI_INIT_CONFIG_DEFAULT();

    // Inicializa o driver Wi-Fi com a configuração padrão.
    ESP_ERROR_CHECK(esp_wifi_init(&udtWifiInitialization));

    // Registra o handler para receber qualquer evento Wi-Fi.
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL,
            NULL
        )
    );

    // Monta a configuração do Access Point.
    wifi_config_t udtAccessPointConfiguration = {
        .ap = {
            // Nome da rede criada pelo ESP32.
            .ssid = WIFI_AP_SSID,

            // Tamanho do SSID.
            .ssid_len = strlen(WIFI_AP_SSID),

            // Canal Wi-Fi usado pelo AP.
            .channel = WIFI_AP_CHANNEL,

            // Senha da rede.
            .password = WIFI_AP_PASSWORD,

            // Número máximo de estações conectadas ao AP.
            .max_connection = WIFI_AP_MAX_CLIENTS,

            // Modo de autenticação padrão quando há senha.
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };

    // Se a senha estiver vazia, transforma a rede em aberta.
    if (strlen(WIFI_AP_PASSWORD) == 0) {
        // WPA2 sem senha é inválido; rede aberta é a alternativa explícita.
        udtAccessPointConfiguration.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Define o modo Wi-Fi como Access Point.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Aplica a configuração na interface AP.
    ESP_ERROR_CHECK(
        esp_wifi_set_config(WIFI_IF_AP, &udtAccessPointConfiguration)
    );

    // Inicia o Wi-Fi.
    ESP_ERROR_CHECK(esp_wifi_start());

    // Logs abaixo são executados após todas as etapas obrigatórias terem sucesso.
    ESP_LOGI(mptrTag, "Access Point started");
    ESP_LOGI(mptrTag, "SSID: %s", WIFI_AP_SSID);
    ESP_LOGI(mptrTag, "Password: %s", WIFI_AP_PASSWORD);
    ESP_LOGI(mptrTag, "Address: http://192.168.4.1/");
}
