#pragma once

/*
 * web_server.h
 *
 * Interface pública do servidor HTTP.
 *
 * Este módulo cria o servidor HTTP do ESP-IDF e registra as rotas usadas
 * pela aplicação:
 * - /   para servir o index.html embutido no firmware;
 * - /ws para comunicação WebSocket com a página.
 */

#include "esp_http_server.h"

/**
 * @brief Inicia o servidor HTTP e registra as rotas da aplicação.
 *
 * @return Handle do servidor em sucesso ou NULL em falha.
 *
 * Inicia o servidor HTTP e registra as rotas da aplicação.
 *
 * Retorna:
 * - handle do servidor, em caso de sucesso;
 * - NULL, em caso de falha.
 */
httpd_handle_t web_server_start(void);
