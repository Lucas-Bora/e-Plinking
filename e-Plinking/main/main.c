#include <stdio.h>   // Inclui funções padrão de entrada/saída da linguagem C.
#include <string.h>  // Inclui funções para manipulação de strings, como strlen() e strcmp().
#include <stdint.h>  // Inclui tipos inteiros com tamanho fixo, como uint8_t.
#include <stdbool.h> // Inclui o tipo bool e os valores true/false.

#include "freertos/FreeRTOS.h" // Inclui definições básicas do FreeRTOS usadas pelo ESP-IDF.
#include "freertos/task.h"     // Inclui funções de task, como xTaskCreate() e vTaskDelay().

#include "esp_err.h"         // Inclui o tipo esp_err_t e funções de erro, como esp_err_to_name().
#include "esp_wifi.h"        // Inclui funções para configurar e iniciar o Wi-Fi.
#include "esp_event.h"       // Inclui o sistema de eventos do ESP-IDF.
#include "esp_log.h"         // Inclui macros de log, como ESP_LOGI(), ESP_LOGW() e ESP_LOGE().
#include "esp_netif.h"       // Inclui a camada de rede usada pelo Wi-Fi.
#include "esp_mac.h"         // Inclui macros para imprimir MAC, como MACSTR e MAC2STR.
#include "nvs_flash.h"       // Inclui funções para inicializar a memória NVS.
#include "esp_http_server.h" // Inclui servidor HTTP e suporte a WebSocket.

// ======================================================
// Configurações do Access Point
// ======================================================

#define WIFI_AP_SSID "ALVO_ELETRONICO" // Nome da rede Wi-Fi criada pelo ESP32.
#define WIFI_AP_PASSWORD "12345678"    // Senha da rede Wi-Fi criada pelo ESP32.
#define WIFI_AP_CHANNEL 1              // Canal Wi-Fi usado pelo Access Point.
#define WIFI_AP_MAX_CLIENTS 4          // Número máximo de dispositivos conectados ao ESP32.

// ======================================================
// Configurações HTTP / WebSocket
// ======================================================

#define HTTP_ROUTE_INDEX "/"  // Rota HTTP principal; entrega o arquivo index.html.
#define WEBSOCKET_ROUTE "/ws" // Rota WebSocket usada pela página para trocar dados com o ESP32.

#define TEST_PACKET_INTERVAL_MS 5000 // Intervalo, em milissegundos, entre os pacotes de teste.

// ======================================================
// Protocolo do alvo
// ======================================================

#define TARGET_PACKET_STX 0x02 // Byte STX: indica início do pacote.
#define TARGET_PACKET_ETX 0x03 // Byte ETX: indica fim do pacote.
#define TARGET_PACKET_SIZE 37  // Tamanho total do pacote em bytes.

// ======================================================
// TAG usada nos logs
// ======================================================

static const char *TAG = "ALVO_WS"; // Texto que aparece nos logs para identificar este módulo.

// ======================================================
// HTML embarcado
// ======================================================
//
// O arquivo index.html deve estar no diretório main/.
// No CMakeLists.txt ele deve estar em EMBED_TXTFILES.
// Isso faz o ESP-IDF embutir o HTML no firmware como texto.
// O símbolo _binary_index_html_start aponta para o início desse HTML.
// ======================================================

extern const uint8_t index_html_start[] asm("_binary_index_html_start"); // Ponteiro para o início do index.html embutido.

// ======================================================
// Estrutura do pacote enviado para a página
// ======================================================
//
// O protocolo é:
//
// STX
// B1[4] = número de disparos geral
// B2[4] = pontuação geral
// B3    = zona alvejada
// B4    = linha da tabela
// B5[8] = tempo de reação da tabela
// B6[4] = disparos da tabela
// B7[8] = tempo médio da tabela
// B8[5] = pontuação da tabela
// ETX
//
// Total:
//
// 1 + 4 + 4 + 1 + 1 + 8 + 4 + 8 + 5 + 1 = 37 bytes
//
// O atributo packed impede o compilador de inserir bytes extras
// de alinhamento entre os campos da struct.
// ======================================================

typedef struct
{                // Define uma struct empacotada para representar exatamente o pacote.
    uint8_t stx; // Byte inicial do pacote; deve conter 0x02.

    char disparos_geral[4];  // B1: quatro caracteres com o total geral de disparos.
    char pontuacao_geral[4]; // B2: quatro caracteres com a pontuação geral.

    char zona_alvejada; // B3: um caractere indicando a zona alvejada.
    char linha_tabela;  // B4: um caractere indicando a linha da tabela.

    char tempo_reacao[8];     // B5: oito caracteres com o tempo de reação.
    char disparos_tabela[4];  // B6: quatro caracteres com os disparos daquela linha.
    char tempo_medio[8];      // B7: oito caracteres com o tempo médio daquela linha.
    char pontuacao_tabela[5]; // B8: cinco caracteres com a pontuação daquela linha.

    uint8_t etx; // Byte final do pacote; deve conter 0x03.
} pacote_alvo_t; // Nome do tipo criado para representar o pacote.

// Esta verificação acontece em tempo de compilação.
// Se o tamanho da struct não for 37 bytes, o compilador gera erro.
_Static_assert(                                  // Inicia uma checagem estática em tempo de compilação.
    sizeof(pacote_alvo_t) == TARGET_PACKET_SIZE, // Confere se a struct tem exatamente 37 bytes.
    "Tamanho invalido para pacote_alvo_t"        // Mensagem exibida se a verificação falhar.
);

// ======================================================
// Pacotes hard coded para teste
// ======================================================
//
// zona_alvejada:
//
// '0' = nenhuma zona pisca
// '1' = zona A
// '2' = zona C
// '3' = zona D
//
// linha_tabela:
//
// '1' = primeira linha
// '2' = segunda linha
// '3' = terceira linha
// '4' = quarta linha
// '5' = quinta linha
//
// Importante:
// Os campos char[4], char[5] e char[8] não são strings C completas.
// Eles não têm '\0' no final.
// Eles são campos de tamanho fixo do protocolo.
// ======================================================

static pacote_alvo_t pacotes_teste[] = {
    {.stx = TARGET_PACKET_STX,
     .disparos_geral = {'0', '0', '0', '1'},
     .pontuacao_geral = {'0', '0', '0', '5'},
     .zona_alvejada = '1',
     .linha_tabela = '1',
     .tempo_reacao = {'0', '0', ':', '0', '0', '3', '2', '0'},
     .disparos_tabela = {'0', '0', '0', '1'},
     .tempo_medio = {'0', '0', ':', '0', '0', '3', '2', '0'},
     .pontuacao_tabela = {'0', '0', '0', '0', '5'},
     .etx = TARGET_PACKET_ETX},
    {.stx = TARGET_PACKET_STX,
     .disparos_geral = {'0', '0', '0', '2'},
     .pontuacao_geral = {'0', '0', '1', '0'},
     .zona_alvejada = '2',
     .linha_tabela = '2',
     .tempo_reacao = {'0', '0', ':', '0', '0', '4', '1', '0'},
     .disparos_tabela = {'0', '0', '0', '2'},
     .tempo_medio = {'0', '0', ':', '0', '0', '3', '6', '5'},
     .pontuacao_tabela = {'0', '0', '0', '1', '0'},
     .etx = TARGET_PACKET_ETX},
    {.stx = TARGET_PACKET_STX,
     .disparos_geral = {'0', '0', '0', '3'},
     .pontuacao_geral = {'0', '0', '1', '5'},
     .zona_alvejada = '3',
     .linha_tabela = '3',
     .tempo_reacao = {'0', '0', ':', '0', '0', '5', '2', '0'},
     .disparos_tabela = {'0', '0', '0', '3'},
     .tempo_medio = {'0', '0', ':', '0', '0', '4', '1', '6'},
     .pontuacao_tabela = {'0', '0', '0', '1', '5'},
     .etx = TARGET_PACKET_ETX},
    {.stx = TARGET_PACKET_STX,
     .disparos_geral = {'0', '0', '0', '4'},
     .pontuacao_geral = {'0', '0', '2', '0'},
     .zona_alvejada = '1',
     .linha_tabela = '4',
     .tempo_reacao = {'0', '0', ':', '0', '0', '6', '0', '0'},
     .disparos_tabela = {'0', '0', '0', '4'},
     .tempo_medio = {'0', '0', ':', '0', '0', '4', '6', '2'},
     .pontuacao_tabela = {'0', '0', '0', '2', '0'},
     .etx = TARGET_PACKET_ETX},
    {.stx = TARGET_PACKET_STX,
     .disparos_geral = {'0', '0', '0', '5'},
     .pontuacao_geral = {'0', '0', '2', '5'},
     .zona_alvejada = '0',
     .linha_tabela = '5',
     .tempo_reacao = {'0', '0', ':', '0', '0', '7', '1', '0'},
     .disparos_tabela = {'0', '0', '0', '5'},
     .tempo_medio = {'0', '0', ':', '0', '0', '5', '1', '2'},
     .pontuacao_tabela = {'0', '0', '0', '2', '5'},
     .etx = TARGET_PACKET_ETX}};

#define TOTAL_PACOTES_TESTE (sizeof(pacotes_teste) / sizeof(pacotes_teste[0]))
// ======================================================
// Variáveis globais simples
// ======================================================
//
// Esta versão considera apenas um navegador conectado.
// Se outro navegador conectar, ele substitui o anterior.
// ======================================================

static httpd_handle_t servidor_http = NULL; // Guarda o handle do servidor HTTP.
static int websocket_fd = -1;               // Guarda o socket do cliente WebSocket conectado.
static bool envio_teste_ativo = true;       // Indica se o envio automático dos pacotes está ativo.
static size_t indice_pacote_teste = 0;      // Guarda o índice do próximo pacote de teste.

// ======================================================
// Inicialização da NVS
// ======================================================
//
// O Wi-Fi do ESP-IDF precisa da NVS inicializada.
// Por isso esta função deve rodar antes de iniciar o Wi-Fi.
// ======================================================

static void inicializar_nvs(void) // Define a função que inicializa a NVS.
{
    esp_err_t resultado = nvs_flash_init(); // Tenta inicializar a memória NVS.

    if (                                           // Verifica se a NVS precisa ser apagada.
        resultado == ESP_ERR_NVS_NO_FREE_PAGES ||  // Caso a NVS esteja sem páginas livres.
        resultado == ESP_ERR_NVS_NEW_VERSION_FOUND // Caso a versão da NVS seja incompatível.
    )
    {
        ESP_ERROR_CHECK(nvs_flash_erase()); // Apaga a NVS.
        resultado = nvs_flash_init();       // Tenta inicializar novamente a NVS.
    }

    ESP_ERROR_CHECK(resultado); // Se ainda houver erro, interrompe a execução.
}

// ======================================================
// Tratamento de eventos Wi-Fi
// ======================================================
//
// Esta função é chamada automaticamente pelo ESP-IDF
// quando algum evento Wi-Fi acontece.
// ======================================================

static void tratar_evento_wifi(  // Define a função de callback para eventos Wi-Fi.
    void *arg,                   // Argumento opcional; não será usado.
    esp_event_base_t event_base, // Base do evento; aqui será WIFI_EVENT.
    int32_t event_id,            // Código identificando qual evento ocorreu.
    void *event_data             // Dados adicionais do evento.
)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {                                                   // Verifica se um cliente conectou ao Access Point.
        wifi_event_ap_staconnected_t *evento =          // Cria ponteiro para interpretar os dados do evento.
            (wifi_event_ap_staconnected_t *)event_data; // Converte event_data para o tipo correto.

        ESP_LOGI(                                               // Imprime informação no monitor serial.
            TAG,                                                // Usa a tag definida no início do arquivo.
            "Cliente Wi-Fi conectado. MAC: " MACSTR ", AID=%d", // Mensagem com MAC e AID.
            MAC2STR(evento->mac),                               // Converte o MAC para o formato imprimível.
            evento->aid                                         // Mostra o AID do cliente conectado.
        );

        return; // Sai da função depois de tratar o evento.
    }

    if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {                                                      // Verifica se um cliente desconectou do AP.
        wifi_event_ap_stadisconnected_t *evento =          // Cria ponteiro para interpretar os dados do evento.
            (wifi_event_ap_stadisconnected_t *)event_data; // Converte event_data para o tipo correto.

        ESP_LOGI(                                                  // Imprime informação no monitor serial.
            TAG,                                                   // Usa a tag do módulo.
            "Cliente Wi-Fi desconectado. MAC: " MACSTR ", AID=%d", // Mensagem com MAC e AID.
            MAC2STR(evento->mac),                                  // Converte MAC para texto.
            evento->aid                                            // Mostra o AID do cliente desconectado.
        );

        websocket_fd = -1; // Invalida o WebSocket porque o cliente Wi-Fi caiu.

        return; // Sai da função depois de tratar o evento.
    }
}

// ======================================================
// Inicialização do Wi-Fi em modo Access Point
// ======================================================
//
// Esta função cria a rede Wi-Fi do ESP32.
// O usuário conecta nela e acessa http://192.168.4.1/
// ======================================================

static void inicializar_wifi_access_point(void) // Define a função que inicializa o Wi-Fi AP.
{
    ESP_ERROR_CHECK(esp_netif_init()); // Inicializa a camada de rede do ESP-IDF.

    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Cria o loop padrão de eventos do ESP-IDF.

    esp_netif_create_default_wifi_ap(); // Cria a interface padrão para Wi-Fi em modo AP.

    wifi_init_config_t configuracao_inicial_wifi = // Declara estrutura de configuração inicial do Wi-Fi.
        WIFI_INIT_CONFIG_DEFAULT();                // Preenche a estrutura com valores padrão.

    ESP_ERROR_CHECK(                              // Verifica se a inicialização do Wi-Fi deu certo.
        esp_wifi_init(&configuracao_inicial_wifi) // Inicializa o driver Wi-Fi com a configuração padrão.
    );

    ESP_ERROR_CHECK(                         // Verifica se o registro do handler deu certo.
        esp_event_handler_instance_register( // Registra função que receberá eventos Wi-Fi.
            WIFI_EVENT,                      // Base de eventos Wi-Fi.
            ESP_EVENT_ANY_ID,                // Aceita qualquer evento Wi-Fi.
            tratar_evento_wifi,              // Função chamada quando um evento ocorrer.
            NULL,                            // Argumento opcional; não usado.
            NULL                             // Handle da instância; não será armazenado.
            ));

    wifi_config_t configuracao_access_point = {
        // Declara a configuração do Access Point.
        .ap = {
            // Seleciona a parte da struct referente ao modo AP.
            .ssid = WIFI_AP_SSID,                  // Define o nome da rede Wi-Fi.
            .ssid_len = strlen(WIFI_AP_SSID),      // Define o tamanho do nome da rede.
            .channel = WIFI_AP_CHANNEL,            // Define o canal Wi-Fi.
            .password = WIFI_AP_PASSWORD,          // Define a senha da rede.
            .max_connection = WIFI_AP_MAX_CLIENTS, // Define o máximo de clientes conectados.
            .authmode = WIFI_AUTH_WPA2_PSK         // Define autenticação WPA2.
        } // Fecha configuração .ap.
    }; // Fecha configuração do Wi-Fi.

    if (strlen(WIFI_AP_PASSWORD) == 0)
    {                                                           // Verifica se a senha está vazia.
        configuracao_access_point.ap.authmode = WIFI_AUTH_OPEN; // Se não houver senha, deixa a rede aberta.
    }

    ESP_ERROR_CHECK(                    // Verifica se a configuração do modo Wi-Fi deu certo.
        esp_wifi_set_mode(WIFI_MODE_AP) // Coloca o Wi-Fi em modo Access Point.
    );

    ESP_ERROR_CHECK(                   // Verifica se a configuração do AP deu certo.
        esp_wifi_set_config(           // Aplica a configuração do Wi-Fi.
            WIFI_IF_AP,                // Indica que estamos configurando a interface AP.
            &configuracao_access_point // Passa o endereço da configuração.
            ));

    ESP_ERROR_CHECK(     // Verifica se o Wi-Fi iniciou corretamente.
        esp_wifi_start() // Inicia o Wi-Fi.
    );

    ESP_LOGI(TAG, "Access Point iniciado");         // Informa que o AP foi iniciado.
    ESP_LOGI(TAG, "SSID: %s", WIFI_AP_SSID);        // Mostra o nome da rede no log.
    ESP_LOGI(TAG, "Senha: %s", WIFI_AP_PASSWORD);   // Mostra a senha da rede no log.
    ESP_LOGI(TAG, "Endereco: http://192.168.4.1/"); // Mostra o endereço padrão do AP.
}


// ======================================================
// Rota HTTP /
// ======================================================
//
// Esta função responde quando o navegador acessa:
//
// http://192.168.4.1/
//
// Ela envia o index.html para o navegador.
// ======================================================

static esp_err_t responder_pagina_principal(httpd_req_t *requisicao) // Handler da rota HTTP principal.
{
    httpd_resp_set_type( // Define o tipo de conteúdo da resposta HTTP.
        requisicao,      // Requisição recebida pelo servidor.
        "text/html"      // Tipo MIME da resposta: HTML.
    );

    httpd_resp_send(                    // Envia uma resposta HTTP ao navegador.
        requisicao,                     // Requisição que será respondida.
        (const char *)index_html_start, // Conteúdo do HTML embutido.
        HTTPD_RESP_USE_STRLEN           // Pede para o ESP-IDF calcular o tamanho via strlen().
    );

    return ESP_OK; // Retorna sucesso ao servidor HTTP.
}

// ======================================================
// Envio direto de pacote WebSocket
// ======================================================
//
// Esta versão não usa malloc().
// Esta versão não usa httpd_queue_work().
// Esta versão envia direto usando httpd_ws_send_frame_async().
//
// Ela é adequada para teste simples com um cliente WebSocket.
// ======================================================

static esp_err_t enviar_pacote_websocket(const pacote_alvo_t *pacote) // Função que envia um pacote para a página.
{
    if (servidor_http == NULL)
    {                                 // Verifica se o servidor HTTP ainda não foi iniciado.
        return ESP_ERR_INVALID_STATE; // Retorna erro de estado inválido.
    }

    if (websocket_fd < 0)
    {                                 // Verifica se não existe cliente WebSocket conectado.
        return ESP_ERR_INVALID_STATE; // Retorna erro de estado inválido.
    }

    if (pacote == NULL)
    {                               // Verifica se o ponteiro do pacote é inválido.
        return ESP_ERR_INVALID_ARG; // Retorna erro de argumento inválido.
    }

    if (                                   // Verifica se o socket salvo ainda é um WebSocket válido.
        httpd_ws_get_fd_info(              // Consulta o tipo de cliente associado ao socket.
            servidor_http,                 // Servidor HTTP onde o WebSocket está registrado.
            websocket_fd                   // Socket do cliente WebSocket.
            ) != HTTPD_WS_CLIENT_WEBSOCKET // Compara com o tipo esperado.
    )
    {
        ESP_LOGW(TAG, "Cliente WebSocket desconectado"); // Avisa que o cliente não está mais conectado.

        websocket_fd = -1; // Invalida o socket salvo.

        return ESP_ERR_INVALID_STATE; // Retorna erro de estado inválido.
    }

    httpd_ws_frame_t frame = {
        // Declara e inicializa o frame WebSocket.
        .final = true,                // Indica que este frame é uma mensagem completa.
        .fragmented = false,          // Indica que a mensagem não está fragmentada.
        .type = HTTPD_WS_TYPE_TEXT,   // Envia como frame de texto.
        .payload = (uint8_t *)pacote, // Ponteiro para os 37 bytes do pacote.
        .len = sizeof(pacote_alvo_t)  // Tamanho do payload enviado.
    };

    esp_err_t resultado = httpd_ws_send_frame_async( // Envia o frame WebSocket.
        servidor_http,                               // Handle do servidor HTTP.
        websocket_fd,                                // Socket do cliente WebSocket.
        &frame                                       // Endereço da estrutura do frame.
    );

    if (resultado != ESP_OK)
    {                                        // Verifica se houve falha no envio.
        ESP_LOGW(                            // Imprime aviso no monitor serial.
            TAG,                             // Tag do log.
            "Falha ao enviar WebSocket: %s", // Mensagem de erro.
            esp_err_to_name(resultado)       // Converte o código de erro em texto.
        );

        websocket_fd = -1; // Invalida o WebSocket após falha.
    }

    return resultado; // Retorna o resultado do envio.
}

// ======================================================
// Tratamento dos comandos recebidos da página
// ======================================================
//
// A página envia comandos pelo WebSocket:
//
// INICIAR
// PARAR
// LIMPAR
// ======================================================

static void tratar_comando_da_pagina(const char *comando) // Função que interpreta comandos vindos do HTML/JS.
{
    if (comando == NULL)
    {           // Verifica se o ponteiro do comando é inválido.
        return; // Sai sem fazer nada.
    }

    ESP_LOGI(TAG, "Comando recebido: %s", comando); // Mostra o comando recebido no monitor serial.

    if (strcmp(comando, "INICIAR") == 0)
    {                                             // Verifica se o comando é INICIAR.
        envio_teste_ativo = true;                 // Ativa o envio automático dos pacotes de teste.
        ESP_LOGI(TAG, "Envio de teste iniciado"); // Mostra log informativo.
        return;                                   // Sai da função.
    }

    if (strcmp(comando, "PARAR") == 0)
    {                                            // Verifica se o comando é PARAR.
        envio_teste_ativo = false;               // Pausa o envio automático dos pacotes de teste.
        ESP_LOGI(TAG, "Envio de teste pausado"); // Mostra log informativo.
        return;                                  // Sai da função.
    }

    if (strcmp(comando, "LIMPAR") == 0)
    {                                                // Verifica se o comando é LIMPAR.
        indice_pacote_teste = 0;                     // Reinicia a sequência de pacotes de teste.
        ESP_LOGI(TAG, "Indice de teste reiniciado"); // Mostra log informativo.
        return;                                      // Sai da função.
    }

    ESP_LOGW(TAG, "Comando desconhecido: %s", comando); // Avisa se recebeu comando inesperado.
}

// ======================================================
// Rota WebSocket /ws
// ======================================================
//
// Esta função trata:
//
// 1. A conexão inicial do WebSocket.
// 2. As mensagens enviadas pela página.
//
// Quando o navegador faz:
//
// new WebSocket("ws://192.168.4.1/ws")
//
// O ESP32 recebe um GET /ws.
// Depois do upgrade, a conexão passa a ser WebSocket.
// ======================================================

static esp_err_t tratar_requisicao_websocket(httpd_req_t *requisicao) // Handler da rota /ws.
{
    // Todo: Remover o log abaixo depois de testar a conexão.
    ESP_LOGI(TAG, "Valor de websocket_fd: %d", websocket_fd);

        ESP_LOGI(
        TAG,
        "Entrei em tratar_requisicao_websocket(). method=%d",
        requisicao->method
    );

    httpd_ws_frame_t frame; // Declara a estrutura que representa um frame recebido.

    memset(           // Limpa a estrutura antes de usar.
        &frame,       // Endereço da estrutura.
        0,            // Valor usado para preencher.
        sizeof(frame) // Quantidade de bytes a limpar.
    );

    frame.type = HTTPD_WS_TYPE_TEXT; // Informa que esperamos receber um frame de texto.

    esp_err_t resultado = httpd_ws_recv_frame( // Primeira chamada para receber frame.
        requisicao,                            // Requisição WebSocket.
        &frame,                                // Estrutura onde o ESP-IDF preencherá dados do frame.
        0                                      // Zero indica que queremos apenas descobrir o tamanho.
    );

    if (resultado != ESP_OK)
    {                                                        // Verifica se houve erro ao obter tamanho do frame.
        ESP_LOGE(                                            // Mostra erro no monitor serial.
            TAG,                                             // Tag do log.
            "Falha ao obter tamanho do frame WebSocket: %s", // Mensagem.
            esp_err_to_name(resultado)                       // Código de erro convertido para texto.
        );

        return resultado; // Retorna o erro ao servidor HTTP.
    }

    if (frame.len == 0)
    {                  // Verifica se o frame está vazio.
        return ESP_OK; // Ignora frame vazio.
    }

    if (frame.len > 32)
    {                                                                         // Limita tamanho máximo dos comandos aceitos.
        ESP_LOGW(TAG, "Comando WebSocket muito grande: %d bytes", frame.len); // Avisa comando grande.
        return ESP_ERR_INVALID_SIZE;                                          // Retorna erro de tamanho inválido.
    }

    char comando[33]; // Cria buffer local para comando de até 32 caracteres + '\0'.

    memset(             // Limpa o buffer do comando.
        comando,        // Endereço do buffer.
        0,              // Valor usado para limpar.
        sizeof(comando) // Tamanho total do buffer.
    );

    frame.payload = (uint8_t *)comando; // Informa onde o ESP-IDF deve colocar os dados recebidos.

    resultado = httpd_ws_recv_frame( // Segunda chamada para receber frame.
        requisicao,                  // Requisição WebSocket.
        &frame,                      // Estrutura do frame.
        frame.len                    // Quantidade de bytes a ler.
    );

    if (resultado == ESP_OK)
    {                              // Verifica se o conteúdo foi recebido com sucesso.
        comando[frame.len] = '\0'; // Garante que o comando termine como string C.

        if (frame.type == HTTPD_WS_TYPE_TEXT)
        {                                      // Verifica se o frame recebido é texto.
            tratar_comando_da_pagina(comando); // Interpreta o comando recebido.
        }
        else
        {                                                         // Caso o frame não seja texto.
            ESP_LOGW(TAG, "Frame ignorado. Tipo=%d", frame.type); // Avisa que o frame foi ignorado.
        }
    }
    else
    {                                               // Caso a leitura do conteúdo tenha falhado.
        ESP_LOGE(                                   // Mostra erro no monitor serial.
            TAG,                                    // Tag do log.
            "Falha ao receber frame WebSocket: %s", // Mensagem de erro.
            esp_err_to_name(resultado)              // Código de erro convertido em texto.
        );
    }

    return resultado; // Retorna o resultado final ao servidor HTTP.
}


// ======================================================
// Inicialização do servidor HTTP
// ======================================================
//
// Esta função inicia o servidor HTTP e registra:
//
// /   -> página HTML
// /ws -> WebSocket
// ======================================================

static esp_err_t tratar_websocket_conectado(httpd_req_t *requisicao)
{
    websocket_fd = httpd_req_to_sockfd(requisicao);

    ESP_LOGI(
        TAG,
        "WebSocket conectado no post-handshake.................................... FD=%d",
        websocket_fd
    );

    ESP_LOGI(
        TAG,
        "Handshake................................. method=%d",
        requisicao->method      
    );
    return ESP_OK;
      
}

// ======================================================
// Inicialização do servidor HTTP
// ======================================================
//
// Esta função inicia o servidor HTTP e registra:
//
// /   -> página HTML
// /ws -> WebSocket
// ======================================================

static httpd_handle_t inicializar_servidor_http(void) // Função que inicia o servidor HTTP.
{
    httpd_config_t configuracao_http = // Declara a configuração do servidor HTTP.
        HTTPD_DEFAULT_CONFIG();        // Preenche com os valores padrão do ESP-IDF.

    configuracao_http.max_uri_handlers = 4; // Define quantidade máxima de rotas registradas.

    configuracao_http.lru_purge_enable = true; // Permite remover conexões antigas se necessário.

    httpd_handle_t servidor = NULL; // Variável que receberá o handle do servidor iniciado.

    esp_err_t resultado = httpd_start( // Inicia o servidor HTTP.
        &servidor,                     // Endereço onde o handle será armazenado.
        &configuracao_http             // Configuração usada pelo servidor.
    );

    if (resultado != ESP_OK)
    {                                             // Verifica se houve falha ao iniciar o servidor.
        ESP_LOGE(                                 // Mostra erro no monitor serial.
            TAG,                                  // Tag do log.
            "Falha ao iniciar servidor HTTP: %s", // Mensagem.
            esp_err_to_name(resultado)            // Código de erro convertido para texto.
        );

        return NULL; // Retorna NULL para indicar falha.
    }

    httpd_uri_t rota_index = {
        // Define a rota da página principal.
        .uri = HTTP_ROUTE_INDEX,               // Caminho da rota: "/".
        .method = HTTP_GET,                    // Método aceito: GET.
        .handler = responder_pagina_principal, // Função chamada quando a rota for acessada.
        .user_ctx = NULL                       // Contexto opcional; não usado.
    };

    resultado = httpd_register_uri_handler( // Registra a rota "/" no servidor.
        servidor,                           // Handle do servidor HTTP.
        &rota_index                         // Endereço da configuração da rota.
    );

    if (resultado != ESP_OK)
    {                                               // Verifica se falhou ao registrar a rota.
        ESP_LOGE(TAG, "Falha ao registrar rota /"); // Mostra erro no monitor serial.

        httpd_stop(servidor); // Para o servidor porque a inicialização ficou incompleta.

        return NULL; // Retorna NULL para indicar falha.
    }

    httpd_uri_t rota_websocket = {
        // Define a rota WebSocket.
        .uri = WEBSOCKET_ROUTE,                 // Caminho da rota: "/ws".
        .method = HTTP_GET,                     // WebSocket começa como GET.
        .handler = tratar_requisicao_websocket, // Função chamada para conexão e mensagens WebSocket.
        .user_ctx = NULL,                      // Contexto opcional; não usado.
        .is_websocket = true,                    // Informa ao ESP-IDF que esta rota é WebSocket.
        .ws_post_handshake_cb = tratar_websocket_conectado         // 
    };

    resultado = httpd_register_uri_handler( // Registra a rota WebSocket no servidor.
        servidor,                           // Handle do servidor HTTP.
        &rota_websocket                     // Endereço da configuração da rota WebSocket.
    );

    if (resultado != ESP_OK)
    {                                                       // Verifica se falhou ao registrar a rota WebSocket.
        ESP_LOGE(TAG, "Falha ao registrar rota WebSocket"); // Mostra erro no monitor serial.

        httpd_stop(servidor); // Para o servidor porque a inicialização ficou incompleta.

        return NULL; // Retorna NULL para indicar falha.
    }

    ESP_LOGI(TAG, "Servidor HTTP iniciado");               // Mostra que o servidor iniciou com sucesso.
    ESP_LOGI(TAG, "Rota %s registrada", HTTP_ROUTE_INDEX); // Mostra que a rota "/" foi registrada.
    ESP_LOGI(TAG, "Rota %s registrada", WEBSOCKET_ROUTE);  // Mostra que a rota "/ws" foi registrada.

    return servidor; // Retorna o handle do servidor HTTP.
}



// ======================================================
// Task de envio dos pacotes hard coded
// ======================================================
//
// Esta task roda continuamente.
// A cada 1000 ms, se houver WebSocket conectado,
// envia o próximo pacote hard coded.
// ======================================================

static void tarefa_enviar_pacotes_teste(void *parametros) // Função executada pela task de envio.
{
    while (true)
    { // Loop infinito da task.

        if (envio_teste_ativo && websocket_fd >= 0)
        {                                            // Verifica se pode enviar e se há cliente conectado.
            pacote_alvo_t *pacote =            // Declara ponteiro para o pacote que será enviado.
                &pacotes_teste[indice_pacote_teste]; // Seleciona o pacote atual do array.

                pacotes_teste[indice_pacote_teste].linha_tabela = '1';

            esp_err_t resultado =                // Declara variável para guardar resultado do envio.
                enviar_pacote_websocket(pacote); // Envia o pacote via WebSocket.

            if (resultado == ESP_OK)
            {                                                                        // Verifica se o envio foi bem-sucedido.
                ESP_LOGI(                                                            // Mostra log do pacote enviado.
                    TAG,                                                             // Tag do log.
                    "Pacote enviado: disparos=%.4s pontuacao=%.4s zona=%c linha=%c", // Formato do log.
                    pacote->disparos_geral,                                          // Imprime B1 com limite de 4 caracteres.
                    pacote->pontuacao_geral,                                         // Imprime B2 com limite de 4 caracteres.
                    pacote->zona_alvejada,                                           // Imprime B3.
                    pacote->linha_tabela                                             // Imprime B4.
                );

                indice_pacote_teste++; // Avança para o próximo pacote.

                if (indice_pacote_teste >= TOTAL_PACOTES_TESTE)
                {                            // Verifica se chegou ao fim do array.
                    indice_pacote_teste = 0; // Volta para o primeiro pacote.
                }
            }
            else
            {                                     // Caso o envio tenha falhado.
                ESP_LOGW(                         // Mostra aviso no monitor serial.
                    TAG,                          // Tag do log.
                    "Falha ao enviar pacote: %s", // Mensagem de falha.
                    esp_err_to_name(resultado)    // Código de erro convertido para texto.
                );
            }
        }

        vTaskDelay(                                // Suspende a task por um tempo.
            pdMS_TO_TICKS(TEST_PACKET_INTERVAL_MS) // Converte milissegundos para ticks do FreeRTOS.
        );
    }
}

// ======================================================
// app_main
// ======================================================
//
// Ponto de entrada principal do firmware.
// ======================================================

void app_main(void) // Função principal chamada pelo ESP-IDF.
{
    inicializar_nvs(); // Inicializa a NVS necessária para o Wi-Fi.

    inicializar_wifi_access_point(); // Cria a rede Wi-Fi em modo Access Point.

    servidor_http = inicializar_servidor_http(); // Inicia o servidor HTTP e guarda o handle global.

    if (servidor_http == NULL)
    {                                                // Verifica se o servidor não iniciou.
        ESP_LOGE(TAG, "Servidor HTTP nao iniciado"); // Mostra erro no monitor serial.
        return;                                      // Sai de app_main porque não há servidor.
    }

    xTaskCreate(                     // Cria uma task FreeRTOS.
        tarefa_enviar_pacotes_teste, // Função que será executada pela task.
        "enviar_pacotes_teste",      // Nome da task para debug.
        4096,                        // Tamanho da pilha da task.
        NULL,                        // Parâmetro passado para a task; não usado.
        5,                           // Prioridade da task.
        NULL                         // Handle da task; não será guardado.
    );
}
