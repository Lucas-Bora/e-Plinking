// Interface pública do próprio servidor.
#include "web_server.h"

#include <string.h>
#include "esp_timer.h"

#include "esp_err.h" // esp_err_t e nomes de erro retornados pelo ESP-IDF.
#include "esp_log.h" // Logs de inicialização e falha das rotas.

#include "app_config.h"        // URIs configuráveis "/" e "/ws".
#include "websocket_service.h" // Handlers e inicialização do transporte.

// Tag constante usada nos logs deste módulo.
static const char *mptrTag = "WEB_SERVER";

// Eventos apenas observam as sessoes; o HTTPD continua gerenciando sockets.
static void web_server_log_event(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *data)
{
    (void)arg;
    (void)base;
    if (data == NULL) return;
    if (event_id == HTTP_SERVER_EVENT_ON_CONNECTED ||
        event_id == HTTP_SERVER_EVENT_DISCONNECTED ||
        event_id == HTTP_SERVER_EVENT_HEADERS_SENT) {
        const char *name = event_id == HTTP_SERVER_EVENT_ON_CONNECTED ? "TCP connected" :
            event_id == HTTP_SERVER_EVENT_DISCONNECTED ? "TCP disconnected" : "HTTP headers sent";
        ESP_LOGI(mptrTag, "%s FD=%d", name, *(int *)data);
    } else if (event_id == HTTP_SERVER_EVENT_ERROR) {
        ESP_LOGW(mptrTag, "HTTP error enum=%d", (int)*(httpd_err_code_t *)data);
    }
}

static void web_server_log_request(httpd_req_t *request)
{
    ESP_LOGI(mptrTag, "Request FD=%d method=%d URI=%s",
             httpd_req_to_sockfd(request), (int)request->method, request->uri);
    const char *headers[] = {"Host", "User-Agent", "Upgrade"};
    for (size_t i = 0; i < sizeof(headers) / sizeof(headers[0]); ++i) {
        char value[160];
        esp_err_t result = httpd_req_get_hdr_value_str(request, headers[i], value, sizeof(value));
        if (result == ESP_OK) {
            ESP_LOGI(mptrTag, "%s: %s", headers[i], value);
        } else if (result == ESP_ERR_HTTPD_RESULT_TRUNC) {
            ESP_LOGI(mptrTag, "%s: [header too long for diagnostic buffer]", headers[i]);
        }
    }
}

static esp_err_t web_server_log_not_found(httpd_req_t *request, httpd_err_code_t error)
{
    web_server_log_request(request);
    ESP_LOGW(mptrTag, "Route not found FD=%d", httpd_req_to_sockfd(request));
    return httpd_resp_send_err(request, error, NULL);
}

/*
 * Símbolo gerado pelo ESP-IDF para o arquivo index.html embutido.
 *
 * Para que esse símbolo exista, o arquivo index.html precisa estar listado
 * no CMakeLists.txt em EMBED_TXTFILES.
 *
 * Exemplo:
 * idf_component_register(... EMBED_TXTFILES "index.html")
 */
extern const uint8_t gbytIndexHtmlStart[]
    __asm__("_binary_index_html_start");

/*
 * Handler da rota HTTP principal: /
 *
 * Quando o usuário acessa http://192.168.4.1/, este handler envia
 * o conteúdo do index.html embutido no firmware.
 */
/**
 * @brief Responde à rota principal com o index.html embarcado.
 *
 * @param ptrRequest Requisição HTTP fornecida pelo servidor.
 *
 * @return ESP_OK depois de entregar o conteúdo.
 *
 * @note O HTML é lido do símbolo gerado pelo linker; não há alocação dinâmica.
 */
static esp_err_t handle_index_request(httpd_req_t *ptrRequest)
{
    web_server_log_request(ptrRequest);
    const int64_t started_us = esp_timer_get_time();
    ESP_LOGI(mptrTag, "HTML send begin FD=%d bytes=%u",
             httpd_req_to_sockfd(ptrRequest), (unsigned)strlen((const char *)gbytIndexHtmlStart));

    // request pertence ao servidor e permanece válido durante todo o handler.

    // Informa ao navegador que a resposta é HTML.
    httpd_resp_set_type(ptrRequest, "text/html");

    // Envia o HTML embutido. HTTPD_RESP_USE_STRLEN pede ao servidor
    // para calcular o tamanho do conteúdo como string terminada em '\0'.
    const esp_err_t send_result = httpd_resp_send(
        ptrRequest,
        (const char *)gbytIndexHtmlStart,
        HTTPD_RESP_USE_STRLEN
    );

    ESP_LOGI(mptrTag, "HTML send end FD=%d result=%s elapsed_ms=%lld",
             httpd_req_to_sockfd(ptrRequest), esp_err_to_name(send_result),
             (long long)((esp_timer_get_time() - started_us) / 1000));

    // O corpo já foi entregue; nenhuma memória dinâmica precisa ser liberada.
    return ESP_OK;
}

/*
 * Inicia o servidor HTTP e registra as rotas da aplicação.
 */
/**
 * @brief Inicia o servidor HTTP e registra as rotas "/" e "/ws".
 *
 * @return Handle do servidor em sucesso ou NULL se alguma etapa falhar.
 *
 * @note Se uma rota obrigatória não puder ser registrada, o servidor
 * parcialmente configurado é encerrado antes do retorno.
 */
httpd_handle_t web_server_start(void)
{
    // Carrega a configuração padrão do servidor HTTP do ESP-IDF.
    httpd_config_t udtHttpConfiguration = HTTPD_DEFAULT_CONFIG();

    // Define quantos handlers de URI o servidor poderá registrar.
    // Atualmente usamos duas rotas principais, mas deixamos margem.
    udtHttpConfiguration.max_uri_handlers = 4;

    // Permite que o servidor descarte conexões antigas se os recursos acabarem.
    udtHttpConfiguration.lru_purge_enable = true;

    // Handle que será preenchido por httpd_start().
    httpd_handle_t ptrServer = NULL;

    // O loop de eventos ja foi criado por wifi_ap_init().
    esp_err_t log_result = esp_event_handler_register(
        ESP_HTTP_SERVER_EVENT, ESP_EVENT_ANY_ID, web_server_log_event, NULL);
    if (log_result != ESP_OK) {
        ESP_LOGW(mptrTag, "Diagnostic event registration failed: %s", esp_err_to_name(log_result));
    }

    // Inicia efetivamente o servidor HTTP.
    esp_err_t udtResult =
        httpd_start(&ptrServer, &udtHttpConfiguration);

    if (udtResult != ESP_OK) {
        ESP_LOGE(
            mptrTag,
            "Failed to start HTTP server: %s",
            esp_err_to_name(udtResult)
        );

        return NULL;
    }

    /*
     * Inicializa o transporte somente depois de httpd_start() ter fornecido um
     * handle válido. Os callbacks da FSM já podem ter sido registrados antes.
     */
    websocket_service_init(ptrServer);

    // Configuração da rota HTTP principal: /.
    httpd_uri_t udtIndexRoute = {
        // Caminho configurado em app_config.h.
        .uri = HTTP_ROUTE_INDEX,

        // A página é obtida por requisição HTTP GET.
        .method = HTTP_GET,

        // Função que envia o index.html embarcado.
        .handler = handle_index_request,

        // Nenhum estado adicional precisa ser passado ao handler.
        .user_ctx = NULL
    };

    // Registra a rota / no servidor.
    udtResult = httpd_register_uri_handler(
        ptrServer,
        &udtIndexRoute
    );

    if (udtResult != ESP_OK) {
        // Sem a página principal, encerra o servidor parcialmente configurado.
        ESP_LOGE(mptrTag, "Failed to register route /");
        httpd_stop(ptrServer);
        return NULL;
    }

    // Configuração da rota WebSocket: /ws.
    httpd_uri_t udtWebsocketRoute = {
        // Caminho configurado para o upgrade WebSocket.
        .uri = WEBSOCKET_ROUTE,

        // O handshake WebSocket começa como GET.
        .method = HTTP_GET,

        // Função que recebe handshake e frames posteriores.
        .handler = websocket_handle_request,

        // O estado está encapsulado em websocket_service.c.
        .user_ctx = NULL,

        // Informa ao ESP-IDF que esta rota deve fazer upgrade para WebSocket.
        .is_websocket = true,

        // Callback chamado após o handshake WebSocket.
        .ws_post_handshake_cb = websocket_handle_connection
    };

    // Registra a rota /ws no servidor.
    udtResult = httpd_register_uri_handler(
        ptrServer,
        &udtWebsocketRoute
    );

    if (udtResult != ESP_OK) {
        // Evita deixar apenas a página ativa sem seu canal de comunicação.
        ESP_LOGE(mptrTag, "Failed to register WebSocket route");
        httpd_stop(ptrServer);
        return NULL;
    }

    log_result = httpd_register_err_handler(ptrServer, HTTPD_404_NOT_FOUND, web_server_log_not_found);
    if (log_result != ESP_OK) {
        ESP_LOGW(mptrTag, "Diagnostic 404 registration failed: %s", esp_err_to_name(log_result));
    }
    ESP_LOGI(mptrTag, "HTTP port=%u sockets=%u recv_timeout=%u send_timeout=%u",
             (unsigned)udtHttpConfiguration.server_port, (unsigned)udtHttpConfiguration.max_open_sockets,
             (unsigned)udtHttpConfiguration.recv_wait_timeout, (unsigned)udtHttpConfiguration.send_wait_timeout);
    ESP_LOGI(mptrTag, "HTTP server started");
    ESP_LOGI(mptrTag, "Route %s registered", HTTP_ROUTE_INDEX);
    ESP_LOGI(mptrTag, "Route %s registered", WEBSOCKET_ROUTE);

    // O chamador mantém o handle para verificar sucesso da inicialização.
    return ptrServer;
}
