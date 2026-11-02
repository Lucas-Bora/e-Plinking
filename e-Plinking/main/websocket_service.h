#pragma once

/*
 * websocket_service.h
 *
 * Transporte WebSocket da aplicação.
 *
 * Este módulo conhece sockets, frames e comandos textuais, mas não contém
 * regras do modo Free shoot. A máquina registra callbacks curtos e decide o
 * efeito de INICIAR, PAUSAR, PARAR, LIMPAR e de uma reconexão.
 */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "target_protocol.h"

typedef enum {
    // Resultado da mensagem textual WS_COMMAND_START.
    WEBSOCKET_COMMAND_START = 0,

    // Resultado da mensagem textual WS_COMMAND_PAUSE.
    WEBSOCKET_COMMAND_PAUSE,

    // Resultado da mensagem textual WS_COMMAND_STOP.
    WEBSOCKET_COMMAND_STOP,

    // Resultado da mensagem textual WS_COMMAND_CLEAR.
    WEBSOCKET_COMMAND_CLEAR
} udtWebsocketCommand;

/*
 * Callback executado na task HTTP depois que um comando foi validado.
 * Sua implementação deve retornar rapidamente e não aplicar regra demorada.
 */
typedef void (*udtWebsocketCommandCallback)(udtWebsocketCommand udtCommand);

/*
 * Callback executado após o handshake. É usado para publicar uma solicitação
 * de snapshot, sem montar ou enviar o snapshot dentro da task HTTP.
 */
typedef void (*udtWebsocketClientConnectedCallback)(void);

/**
 * @brief Registra callbacks de comandos e conexão.
 *
 * @param udtCommandCallback Consumidor de comandos ou NULL.
 * @param udtClientConnectedCallback Consumidor de conexão ou NULL.
 *
 * Registra os consumidores dos eventos recebidos.
 *
 * Pode ser chamada antes de o servidor HTTP existir. Os callbacks executam na
 * task do servidor e, portanto, devem somente postar um evento para outra task.
 */
void websocket_service_set_callbacks(
    udtWebsocketCommandCallback udtCommandCallback,
    udtWebsocketClientConnectedCallback udtClientConnectedCallback
);

/**
 * @brief Associa o transporte ao servidor HTTP.
 *
 * @param ptrHttpServer Handle devolvido por httpd_start().
 */
void websocket_service_init(httpd_handle_t ptrHttpServer);

/**
 * @brief Invalida o cliente WebSocket cadastrado.
 */
void websocket_service_clear_client(void);

/**
 * @brief Consulta se existe um possível cliente cadastrado.
 *
 * @return true quando servidor e descritor estão presentes.
 */
bool websocket_service_has_client(void);

/**
 * @brief Cadastra o cliente após o handshake.
 *
 * @param ptrRequest Requisição aceita pelo servidor.
 *
 * @return ESP_OK ou ESP_ERR_INVALID_ARG.
 */
esp_err_t websocket_handle_connection(httpd_req_t *ptrRequest);

/**
 * @brief Recebe e valida frames da rota WebSocket.
 *
 * @param ptrRequest Requisição/frame fornecido pelo servidor.
 *
 * @return ESP_OK ou erro de recepção/validação.
 */
esp_err_t websocket_handle_request(httpd_req_t *ptrRequest);

/**
 * @brief Copia e agenda o envio de um payload textual.
 *
 * @param ptrPayload Origem dos bytes.
 * @param udtLength Quantidade de bytes.
 *
 * @return ESP_OK quando agendado ou erro correspondente.
 *
 * Agenda o envio de uma cópia do payload.
 *
 * Copiar é essencial: a máquina normalmente monta a mensagem em sua pilha,
 * enquanto o servidor só fará o envio depois, na própria task HTTP.
 */
esp_err_t websocket_send_text_copy(
    const char *ptrPayload,
    size_t udtLength
);

/**
 * @brief Valida e agenda um pacote fixo do alvo.
 *
 * @param ptrPacket Pacote de 40 bytes.
 *
 * @return ESP_OK ou ESP_ERR_INVALID_ARG.
 */
esp_err_t websocket_send_target_packet(const udtTargetPacket *ptrPacket);
