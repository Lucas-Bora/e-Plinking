#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_mac.h"

// ==========================================
// Configurações do Access Point (AP)
// ==========================================
#define ESP_WIFI_SSID      "ALVO_ELETRÔNICO - Lucas B."
#define ESP_WIFI_PASS      "12345678"
#define MAX_STA_CONN       4

#define PINO_INTERRUPCAO    GPIO_NUM_4
#define DEBOUNCE_TEMPO_US   200000

static const char *TAG = "ESP32C3_AP";

volatile uint32_t contador = 0;
volatile int64_t ultimo_tempo_isr = 0;

// ==========================================
// Função de Interrupção (ISR)
// ==========================================
static void IRAM_ATTR gpio_isr_handler(void* arg) {
    int64_t tempo_atual = esp_timer_get_time();
    if ((tempo_atual - ultimo_tempo_isr) > DEBOUNCE_TEMPO_US) {
        contador++;
        ultimo_tempo_isr = tempo_atual;
    }
}

// ==========================================
// Código HTML/JS da Página Web Principal (Modificada)
// ==========================================
const char html_page[] = 
"<!DOCTYPE html>\n"
"<html lang=\"pt-BR\">\n"
"<head>\n"
"    <meta charset=\"UTF-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"    <title>Contador AP ESP-IDF</title>\n"
"    <style>\n"
"        body { font-family: sans-serif; text-align: center; margin-top: 50px; background-color: #f4f4f9; }\n"
"        .card { background: white; padding: 10px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); display: inline-block; }\n"
"        .contador { font-size: 80px; font-weight: bold; color: #28a745; margin: 20px 0; }\n"
"        .btn { padding: 10px 20px; font-size: 16px; margin: 10px; cursor: pointer; border: none; border-radius: 5px; background-color: #007bff; color: white; transition: 0.3s; }\n"
"        .btn:hover { opacity: 0.8; }\n"
"        .btn-danger { background-color: #dc3545; }\n"
"        h1 { font-size: 18px; font-weight: bold; }\n"
"    </style>\n"
"    <script>\n"
"        function atualizarContador() {\n"
"            fetch('/valor')\n"
"                .then(response => response.text())\n"
"                .then(data => { document.getElementById('numero').innerText = data; })\n"
"                .catch(error => console.error('Erro:', error));\n"
"        }\n"
"        function zerarContador() {\n"
"            fetch('/reset', { method: 'POST' })\n"
"                .then(response => { if(response.ok) atualizarContador(); });\n"
"        }\n"
"        setInterval(atualizarContador, 50);\n"
"    </script>\n"
"</head>\n"
"<body>\n"
"    <div class=\"card\">\n"
"        <h1>︻┳═一 ALVO ELETRÔNICO ︻┳═一 </h1>\n"
"        <p>DISPAROS EFETUADO Z1:</p>\n"
"        <div class=\"contador\" id=\"numero\">0</div>\n"
"        <button class=\"btn btn-danger\" onclick=\"zerarContador()\">Zerar Contador</button>\n"
"        <br>\n"
"        <a href=\"/pagina2\"><button class=\"btn\">Ir para Página 2</button></a>\n"
"    </div>\n"
"</body>\n"
"</html>";

// ==========================================
// Código HTML/JS da Página 2 (NOVO)
// ==========================================
const char html_page2[] = 
"<!DOCTYPE html>\n"
"<html lang=\"pt-BR\">\n"
"<head>\n"
"    <meta charset=\"UTF-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"    <title>Página Secundária</title>\n"
"    <style>\n"
"        body { font-family: sans-serif; text-align: center; margin-top: 50px; background-color: #e9ecef; }\n"
"        .card { background: white; padding: 40px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); display: inline-block; }\n"
"        .btn { padding: 10px 20px; font-size: 16px; cursor: pointer; border: none; border-radius: 5px; background-color: #6c757d; color: white; }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class=\"card\">\n"
"        <h1>Bem-vindo à Página 2</h1>\n"
"        <p>Esta é uma página secundária servida pelo ESP32-C3.</p>\n"
"        <a href=\"/\"><button class=\"btn\">Voltar para o Início</button></a>\n"
"    </div>\n"
"</body>\n"
"</html>";

// ==========================================
// Manipuladores de Rotas HTTP
// ==========================================
esp_err_t rota_raiz_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t rota_valor_handler(httpd_req_t *req) {
    char valor_str[16];
    snprintf(valor_str, sizeof(valor_str), "%lu", contador);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, valor_str, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// NOVO: Rota para tratar o clique no botão de Reset
esp_err_t rota_reset_handler(httpd_req_t *req) {
    contador = 0; // Zera a variável global
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN); // Envia confirmação
    return ESP_OK;
}

// NOVO: Rota para servir a Página 2
esp_err_t rota_pagina2_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page2, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

httpd_handle_t iniciar_servidor_web(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_raiz = { .uri = "/", .method = HTTP_GET, .handler = rota_raiz_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_raiz);

        httpd_uri_t uri_valor = { .uri = "/valor", .method = HTTP_GET, .handler = rota_valor_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_valor);

        // NOVO: Registrando as novas rotas no servidor
        httpd_uri_t uri_reset = { .uri = "/reset", .method = HTTP_POST, .handler = rota_reset_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_reset);

        httpd_uri_t uri_pagina2 = { .uri = "/pagina2", .method = HTTP_GET, .handler = rota_pagina2_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_pagina2);
    }
    return server;
}

// ==========================================
// Manipulador de Eventos Wi-Fi e Inicialização
// ==========================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Dispositivo conectado. MAC: " MACSTR ", AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Dispositivo desconectado. MAC: " MACSTR ", AID=%d", MAC2STR(event->mac), event->aid);
    }
}

void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = 1,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    if (strlen(ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Modo AP configurado.");
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << PINO_INTERRUPCAO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(PINO_INTERRUPCAO, gpio_isr_handler, (void*) PINO_INTERRUPCAO);

    wifi_init_softap();
    iniciar_servidor_web();

    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}