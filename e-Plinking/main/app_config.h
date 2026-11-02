#pragma once

#include "sdkconfig.h"

/*
 * app_config.h
 *
 * Arquivo central de configuração da aplicação.
 *
 * A ideia deste módulo é concentrar valores fixos que podem mudar
 * durante o desenvolvimento, como nome da rede Wi-Fi, senha, rotas HTTP,
 * comandos WebSocket e intervalo de envio dos pacotes de teste.
 *
 * Mantendo essas configurações em um único arquivo, evitamos procurar
 * valores espalhados por vários módulos do projeto.
 */

// ======================================================
// Configurações do Wi-Fi em modo Access Point
// ======================================================

// Nome da rede Wi-Fi criada pelo ESP32.
// O usuário deverá se conectar a essa rede para acessar a página do alvo.
#define WIFI_AP_SSID "ALVO_ELETRONICO"

// Senha da rede Wi-Fi criada pelo ESP32.
// Para WPA/WPA2, normalmente a senha precisa ter pelo menos 8 caracteres.
// Se esta string for vazia, o código muda automaticamente para rede aberta.
#define WIFI_AP_PASSWORD "12345678"

// Canal Wi-Fi utilizado pelo Access Point.
// O canal 1 costuma ser uma escolha simples para testes.
#define WIFI_AP_CHANNEL 1

// Número máximo de clientes Wi-Fi que podem se conectar ao ESP32.
// Mesmo que vários clientes se conectem ao Wi-Fi, este firmware mantém
// apenas um cliente WebSocket ativo por vez.
#define WIFI_AP_MAX_CLIENTS 1

// ======================================================
// Configurações HTTP e WebSocket
// ======================================================

// Rota HTTP principal.
// Quando o navegador acessar http://192.168.4.1/,
// o servidor deverá responder com o arquivo index.html embutido no firmware.
#define HTTP_ROUTE_INDEX "/"

// Rota usada para comunicação WebSocket.
// A página monta a URL com location.host e acrescenta esta rota.
#define WEBSOCKET_ROUTE "/ws"

// Tamanho máximo permitido para comandos recebidos via WebSocket.
// Os comandos da página são curtos; o limite impede que um cliente consuma
// desnecessariamente a pilha da task do servidor HTTP.
#define WEBSOCKET_COMMAND_MAX_LENGTH 32

// Comandos aceitos pelo firmware via WebSocket.
// Eles continuam em português porque provavelmente já são enviados assim
// pelo JavaScript da página index.html.
#define WS_COMMAND_START "INICIAR"
#define WS_COMMAND_PAUSE "PAUSAR"
#define WS_COMMAND_STOP  "PARAR"
#define WS_COMMAND_CLEAR "LIMPAR"

// ======================================================
// Configurações das entradas externas de hardware
// ======================================================

/*
 * GPIOs usados pelas três entradas externas do alvo eletrônico.
 *
 * IMPORTANTE — o número do GPIO precisa existir no chip selecionado.
 *
 * O firmware mostrado no log está sendo executado em um ESP32-C3. Esse chip
 * possui GPIOs de 0 a 21; portanto, os antigos GPIOs 25, 26 e 27 eram
 * impossíveis nesse alvo. Ao receber a máscara desses pinos, gpio_config()
 * retornava ESP_ERR_INVALID_ARG e o ESP_ERROR_CHECK reiniciava a placa.
 *
 * Para ESP32-C3 foram escolhidos GPIO3, GPIO4 e GPIO5 como valores padrão:
 *
 * - não são GPIOs de strapping;
 * - não pertencem normalmente à interface da memória flash;
 * - não são os GPIOs 18/19 do USB Serial/JTAG;
 * - não são os GPIOs 20/21 usados pelo console UART mostrado no log.
 *
 * Esses números resolvem a incompatibilidade com o chip, mas ainda precisam
 * corresponder às ligações elétricas da sua placa. Se o esquemático usa outros
 * GPIOs válidos, altere somente as três definições abaixo.
 *
 * Para o ESP32 clássico, os valores 25, 26 e 27 continuam disponíveis. O
 * bloco condicional evita que a mesma configuração inválida volte a ser
 * compilada por engano para o ESP32-C3.
 *
 * As entradas são ativas em nível baixo:
 *
 * - nível alto: entrada em repouso;
 * - transição de alto para baixo: evento de disparo.
 */
#if defined(CONFIG_IDF_TARGET_ESP32C3)
    #define HARDWARE_INPUT_1_GPIO 3
    #define HARDWARE_INPUT_2_GPIO 4
    #define HARDWARE_INPUT_3_GPIO 5
#else
    #define HARDWARE_INPUT_1_GPIO 25
    #define HARDWARE_INPUT_2_GPIO 26
    #define HARDWARE_INPUT_3_GPIO 27
#endif

// Habilita pull-up interno nas entradas.
//
// Use 1 quando o botão/sensor fecha para GND e não existe pull-up externo.
// Use 0 quando a placa já possui resistor de pull-up externo.
#define HARDWARE_INPUT_ENABLE_INTERNAL_PULLUP 1

/*
 * Quantidade máxima de eventos físicos aguardando processamento.
 *
 * Cada disparo ocupa uma posição independente. Portanto, dois disparos da
 * mesma entrada continuam sendo dois itens diferentes e a ordem é preservada.
 * Se a fila encher, o evento novo é descartado e o contador de overflow é
 * incrementado; a perda nunca fica silenciosa.
 */
#define HARDWARE_INPUT_EVENT_QUEUE_LENGTH 32

/*
 * Filtro de bouncing/ruído, em microssegundos.
 *
 * Uma nova borda da mesma entrada dentro desta janela é considerada ruído
 * elétrico. Use 0 para desabilitar o filtro quando o circuito externo já
 * entregar pulsos digitais limpos.
 */
#define HARDWARE_INPUT_DEBOUNCE_US 3000ULL

// ======================================================
// Configurações do modo "Free shoot"
// ======================================================

/*
 * A página possui exatamente cinco linhas de resultados.
 *
 * Cada linha corresponde a um estado explícito de free_shoot_mode:
 * ROUND_1, ROUND_2, ROUND_3, ROUND_4 e ROUND_5. Uma _Static_assert no módulo
 * impede compilar se essa quantidade deixar de corresponder aos estados.
 */
#define FREE_SHOOT_TABLE_ROW_COUNT 5

/*
 * Pontuação associada às três entradas físicas.
 *
 * Estes valores são regra de aplicação, não regra do módulo de GPIO. Se o
 * regulamento do alvo mudar, altere somente estas constantes.
 */
#define FREE_SHOOT_ZONE_1_SCORE 5
#define FREE_SHOOT_ZONE_2_SCORE 3
#define FREE_SHOOT_ZONE_3_SCORE 1

/*
 * Período máximo em que a task da máquina aguarda um evento físico.
 *
 * O valor só controla o bloqueio da task. Ele NÃO é usado como horário do
 * disparo: a ISR captura diretamente esp_timer_get_time().
 */
#define FREE_SHOOT_TASK_POLL_MS 20

// Tamanho da fila interna (INICIAR/PAUSAR/PARAR/LIMPAR/reconexão).
#define FREE_SHOOT_CONTROL_QUEUE_LENGTH 8

// Recursos da task que executa a máquina de estados.
#define FREE_SHOOT_TASK_STACK_SIZE 6144
#define FREE_SHOOT_TASK_PRIORITY 5
