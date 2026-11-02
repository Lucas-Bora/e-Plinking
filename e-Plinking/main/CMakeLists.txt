# CMakeLists.txt do componente main.
#
# Este arquivo informa ao ESP-IDF quais arquivos .c fazem parte do firmware,
# quais diretórios possuem headers e quais arquivos devem ser embutidos
# dentro do binário final.

idf_component_register(
    SRCS
        # Ponto de entrada e ordem de inicialização dos módulos.
        "app_main.c"

        # Inicialização da memória não volátil exigida pelo Wi-Fi.
        "storage.c"

        # Criação da rede local em modo Access Point.
        "wifi_ap.c"

        # Servidor HTTP, página embarcada e registro da rota /ws.
        "web_server.c"

        # Transporte WebSocket desacoplado das regras do Free shoot.
        "websocket_service.c"

        # Layout e formatação do pacote fixo de 40 bytes.
        "target_protocol.c"

        # GPIOs, ISRs, debounce e fila FIFO de disparos.
        "hardware_inputs.c"

        # Máquina de estados, capturas de tempo, contexto, cálculos e envios.
        "free_shoot_mode.c"

    INCLUDE_DIRS
        # Permite que os módulos incluam headers do diretório main.
        "."

    EMBED_TXTFILES
        # Gera o símbolo _binary_index_html_start usado pelo web_server.c.
        "index.html"

    PRIV_REQUIRES
        # Driver legado que fornece driver/gpio.h nesta organização do ESP-IDF.
        esp_driver_gpio

        # Infraestrutura de eventos utilizada pelo módulo Wi-Fi.
        esp_event

        # Servidor HTTP e suporte a frames WebSocket.
        esp_http_server

        # Interface de rede necessária ao Access Point.
        esp_netif

        # Relógio monotônico usado diretamente pela ISR e pela máquina.
        esp_timer

        # Driver do rádio e eventos Wi-Fi.
        esp_wifi

        # Armazenamento não volátil necessário à pilha Wi-Fi.
        nvs_flash
)
