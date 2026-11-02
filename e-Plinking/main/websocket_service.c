#include "websocket_service.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "app_config.h"

static const char *mptrTag = "WEBSOCKET";

/*
 * O lock protege somente o cadastro do servidor, do cliente e dos callbacks.
 * Nenhuma operação de rede ou callback é executada dentro da seção crítica.
 */
static portMUX_TYPE mudtWebsocketLock = portMUX_INITIALIZER_UNLOCKED;
static httpd_handle_t mptrHttpServer = NULL;
static int32_t ml32WebsocketClientFd = -1;
static udtWebsocketCommandCallback mudtCommandCallback = NULL;
static udtWebsocketClientConnectedCallback
    mudtClientConnectedCallback = NULL;

/*
 * O envio é agendado na task HTTP. Esta estrutura mantém uma cópia própria dos
 * metadados e do payload até o trabalho ser executado. O array final flexível
 * permite fazer uma única alocação e um único free().
 */
typedef struct {
    httpd_handle_t ptrServer;
    int32_t l32ClientFd;
    size_t udtLength;
    uint8_t bytPayload[];
} udtWebsocketSendWork;

/* Não apaga um cliente novo quando um trabalho antigo falha. */
/**
 * @brief Invalida o cliente somente se o descritor ainda for o esperado.
 *
 * @param l32ClientFd Descritor associado ao envio que falhou.
 *
 * @note Evita que um trabalho antigo apague um cliente conectado depois.
 */
static void websocket_clear_client_if_matches(int32_t l32ClientFd)
{
    portENTER_CRITICAL(&mudtWebsocketLock);

    if (ml32WebsocketClientFd == l32ClientFd) {
        ml32WebsocketClientFd = -1;
    }

    portEXIT_CRITICAL(&mudtWebsocketLock);
}

/* Aceita somente os quatro textos exatos definidos em app_config.h. */
/**
 * @brief Converte um texto recebido no enum interno de comando.
 *
 * @param ptrCommand String terminada em NUL recebida da página.
 * @param ptrParsedCommand Destino do comando reconhecido.
 *
 * @return true somente para INICIAR, PAUSAR, PARAR ou LIMPAR.
 */
static bool websocket_parse_command(
    const char *ptrCommand,
    udtWebsocketCommand *ptrParsedCommand
)
{
    if (ptrCommand == NULL || ptrParsedCommand == NULL) {
        return false;
    }

    if (strcmp(ptrCommand, WS_COMMAND_START) == 0) {
        *ptrParsedCommand = WEBSOCKET_COMMAND_START;
        return true;
    }

    if (strcmp(ptrCommand, WS_COMMAND_PAUSE) == 0) {
        *ptrParsedCommand = WEBSOCKET_COMMAND_PAUSE;
        return true;
    }

    if (strcmp(ptrCommand, WS_COMMAND_STOP) == 0) {
        *ptrParsedCommand = WEBSOCKET_COMMAND_STOP;
        return true;
    }

    if (strcmp(ptrCommand, WS_COMMAND_CLEAR) == 0) {
        *ptrParsedCommand = WEBSOCKET_COMMAND_CLEAR;
        return true;
    }

    return false;
}

/*
 * Executa na task do servidor HTTP e assume a propriedade de ptrArgument.
 * Todos os caminhos posteriores à conversão liberam o bloco recebido.
 */
/**
 * @brief Executa um envio agendado dentro da task HTTP.
 *
 * @param ptrArgument Bloco udtWebsocketSendWork pertencente a esta função.
 *
 * @note Libera o bloco em todos os caminhos posteriores à validação.
 */
static void websocket_send_work(void *ptrArgument)
{
    udtWebsocketSendWork *ptrWork =
        (udtWebsocketSendWork *)ptrArgument;

    if (ptrWork == NULL) {
        return;
    }

    if (httpd_ws_get_fd_info(
            ptrWork->ptrServer,
            ptrWork->l32ClientFd
        ) != HTTPD_WS_CLIENT_WEBSOCKET) {
        websocket_clear_client_if_matches(ptrWork->l32ClientFd);
        free(ptrWork);
        return;
    }

    httpd_ws_frame_t udtFrame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = ptrWork->bytPayload,
        .len = ptrWork->udtLength,
    };

    const esp_err_t udtResult = httpd_ws_send_frame_async(
        ptrWork->ptrServer,
        ptrWork->l32ClientFd,
        &udtFrame
    );

    if (udtResult != ESP_OK) {
        ESP_LOGW(mptrTag, "WebSocket send failed: %s",
                 esp_err_to_name(udtResult));
        websocket_clear_client_if_matches(ptrWork->l32ClientFd);
    }

    free(ptrWork);
}

/**
 * @brief Registra os consumidores de comandos e novas conexões.
 *
 * @param udtCommandCallback Callback de comando, ou NULL para remover.
 * @param udtClientConnectedCallback Callback de conexão, ou NULL para remover.
 *
 * @note Os callbacks são copiados sob lock e executados fora dele.
 */
void websocket_service_set_callbacks(
    udtWebsocketCommandCallback udtCommandCallback,
    udtWebsocketClientConnectedCallback udtClientConnectedCallback
)
{
    portENTER_CRITICAL(&mudtWebsocketLock);
    mudtCommandCallback = udtCommandCallback;
    mudtClientConnectedCallback = udtClientConnectedCallback;
    portEXIT_CRITICAL(&mudtWebsocketLock);
}

/**
 * @brief Associa o serviço ao servidor HTTP iniciado.
 *
 * @param ptrHttpServer Handle válido devolvido por httpd_start().
 *
 * @note A associação invalida qualquer descritor de cliente anterior.
 */
void websocket_service_init(httpd_handle_t ptrHttpServer)
{
    portENTER_CRITICAL(&mudtWebsocketLock);
    mptrHttpServer = ptrHttpServer;
    ml32WebsocketClientFd = -1;
    portEXIT_CRITICAL(&mudtWebsocketLock);
}

/**
 * @brief Invalida incondicionalmente o cliente WebSocket atual.
 */
void websocket_service_clear_client(void)
{
    portENTER_CRITICAL(&mudtWebsocketLock);
    ml32WebsocketClientFd = -1;
    portEXIT_CRITICAL(&mudtWebsocketLock);
}

/**
 * @brief Consulta se existe servidor e cliente cadastrados.
 *
 * @return true quando ambos estão presentes no estado do serviço.
 *
 * @note A validade real do socket é confirmada novamente no envio.
 */
bool websocket_service_has_client(void)
{
    bool blnHasClient;

    portENTER_CRITICAL(&mudtWebsocketLock);
    blnHasClient =
        mptrHttpServer != NULL && ml32WebsocketClientFd >= 0;
    portEXIT_CRITICAL(&mudtWebsocketLock);
    return blnHasClient;
}

/**
 * @brief Finaliza o cadastro do cliente após o handshake WebSocket.
 *
 * @param ptrRequest Requisição aceita pelo servidor HTTP.
 *
 * @return ESP_OK em sucesso ou ESP_ERR_INVALID_ARG para ponteiro nulo.
 *
 * @note O callback apenas solicita recuperação; não envia o contexto aqui.
 */
esp_err_t websocket_handle_connection(httpd_req_t *ptrRequest)
{
    if (ptrRequest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int32_t l32ClientFd =
        (int32_t)httpd_req_to_sockfd(ptrRequest);
    udtWebsocketClientConnectedCallback udtCallback;

    portENTER_CRITICAL(&mudtWebsocketLock);
    ml32WebsocketClientFd = l32ClientFd;
    udtCallback = mudtClientConnectedCallback;
    portEXIT_CRITICAL(&mudtWebsocketLock);

    ESP_LOGI(mptrTag, "WebSocket client connected. FD=%ld",
             (long)l32ClientFd);

    /*
     * O callback não envia o snapshot na task HTTP. Ele apenas publica o
     * evento de reconexão para a task Free shoot.
     */
    if (udtCallback != NULL) {
        udtCallback();
    }

    return ESP_OK;
}

/**
 * @brief Recebe o handshake ou um frame de comando da rota WebSocket.
 *
 * @param ptrRequest Requisição fornecida pelo servidor HTTP.
 *
 * @return ESP_OK para frame tratado/ignorado ou erro de validação/recepção.
 *
 * @note O payload é lido em duas etapas para validar seu tamanho antes da
 * cópia para a pilha.
 */
esp_err_t websocket_handle_request(httpd_req_t *ptrRequest)
{
    if (ptrRequest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* O GET inicial representa somente o upgrade para WebSocket. */
    if (ptrRequest->method == HTTP_GET) {
        return ESP_OK;
    }

    httpd_ws_frame_t udtFrame;
    memset(&udtFrame, 0, sizeof(udtFrame));

    /*
     * Primeira leitura: obtém o tamanho sem copiar. Isso permite rejeitar um
     * payload excessivo antes de reservar espaço na pilha.
     */
    esp_err_t udtResult =
        httpd_ws_recv_frame(ptrRequest, &udtFrame, 0);

    if (udtResult != ESP_OK) {
        return udtResult;
    }

    if (udtFrame.len == 0) {
        return ESP_OK;
    }

    if (udtFrame.type != HTTPD_WS_TYPE_TEXT) {
        ESP_LOGW(mptrTag, "Ignored non-text WebSocket frame");
        return ESP_OK;
    }

    if (udtFrame.len > WEBSOCKET_COMMAND_MAX_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t bytCommand[WEBSOCKET_COMMAND_MAX_LENGTH + 1] = {0};
    udtFrame.payload = bytCommand;

    udtResult = httpd_ws_recv_frame(
        ptrRequest,
        &udtFrame,
        udtFrame.len
    );

    if (udtResult != ESP_OK) {
        return udtResult;
    }

    bytCommand[udtFrame.len] = '\0';
    udtWebsocketCommand udtParsedCommand;

    if (!websocket_parse_command(
            (const char *)bytCommand,
            &udtParsedCommand
        )) {
        ESP_LOGW(mptrTag, "Unknown WebSocket command: %s",
                 (const char *)bytCommand);
        return ESP_OK;
    }

    udtWebsocketCommandCallback udtCallback;

    portENTER_CRITICAL(&mudtWebsocketLock);
    udtCallback = mudtCommandCallback;
    portEXIT_CRITICAL(&mudtWebsocketLock);

    if (udtCallback == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * O callback é deliberadamente curto: ele coloca o comando na fila da
     * máquina, que executará a transição fora da task HTTP.
     */
    udtCallback(udtParsedCommand);
    return ESP_OK;
}

/**
 * @brief Copia um payload textual e agenda seu envio na task HTTP.
 *
 * @param ptrPayload Origem válida durante esta chamada.
 * @param udtLength Quantidade exata de bytes que deve ser copiada.
 *
 * @return ESP_OK quando o trabalho foi agendado ou erro de estado/memória.
 *
 * @note O chamador pode reutilizar ou liberar sua origem depois do retorno.
 */
esp_err_t websocket_send_text_copy(
    const char *ptrPayload,
    size_t udtLength
)
{
    if (ptrPayload == NULL || udtLength == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_handle_t ptrServer;
    int32_t l32ClientFd;

    portENTER_CRITICAL(&mudtWebsocketLock);
    ptrServer = mptrHttpServer;
    l32ClientFd = ml32WebsocketClientFd;
    portEXIT_CRITICAL(&mudtWebsocketLock);

    if (ptrServer == NULL || l32ClientFd < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    udtWebsocketSendWork *ptrWork =
        malloc(sizeof(*ptrWork) + udtLength);

    if (ptrWork == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ptrWork->ptrServer = ptrServer;
    ptrWork->l32ClientFd = l32ClientFd;
    ptrWork->udtLength = udtLength;
    memcpy(ptrWork->bytPayload, ptrPayload, udtLength);

    const esp_err_t udtResult = httpd_queue_work(
        ptrServer,
        websocket_send_work,
        ptrWork
    );

    if (udtResult != ESP_OK) {
        free(ptrWork);
        return udtResult;
    }

    return ESP_OK;
}

/**
 * @brief Valida e envia um pacote fixo do alvo.
 *
 * @param ptrPacket Pacote de 40 bytes já construído.
 *
 * @return ESP_OK quando agendado ou ESP_ERR_INVALID_ARG para pacote inválido.
 */
esp_err_t websocket_send_target_packet(
    const udtTargetPacket *ptrPacket
)
{
    if (!target_packet_is_valid(ptrPacket)) {
        return ESP_ERR_INVALID_ARG;
    }

    return websocket_send_text_copy(
        (const char *)ptrPacket,
        sizeof(*ptrPacket)
    );
}
