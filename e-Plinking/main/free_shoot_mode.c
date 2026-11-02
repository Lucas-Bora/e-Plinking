#include "free_shoot_mode.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_config.h"
#include "hardware_inputs.h"
#include "target_protocol.h"
#include "websocket_service.h"

/* --------------------------------------------------------------------------
 * 1. ESTADOS, EVENTOS E CONTEXTO
 * -------------------------------------------------------------------------- */

/*
 * Cada ROUND_n representa uma linha da tabela.
 *
 * O estado seleciona uma linha. O campo blnRoundActive do contexto informa se
 * essa linha está recebendo disparos ou se aguarda um novo comando INICIAR.
 */
typedef enum {
    FREE_SHOOT_STATE_IDLE = 0,
    FREE_SHOOT_STATE_ROUND_1,
    FREE_SHOOT_STATE_ROUND_2,
    FREE_SHOOT_STATE_ROUND_3,
    FREE_SHOOT_STATE_ROUND_4,
    FREE_SHOOT_STATE_ROUND_5,
    FREE_SHOOT_STATE_STOPPED,
    FREE_SHOOT_STATE_COMPLETE,
    FREE_SHOOT_STATE_ERROR
} udtFreeShootState;

typedef enum {
    FREE_SHOOT_EVENT_START = 0,
    FREE_SHOOT_EVENT_PAUSE,
    FREE_SHOOT_EVENT_STOP,
    FREE_SHOOT_EVENT_CLEAR,
    FREE_SHOOT_EVENT_CONNECTED
} udtFreeShootEvent;

/*
 * Dados de uma linha.
 *
 * O tempo médio permanece zero enquanto a rodada está ativa. Ele só é
 * calculado por PAUSAR, usando:
 *
 *     tempo total da rodada / quantidade de disparos da rodada
 */
typedef struct {
    uint64_t l64ReactionTimeUs;
    uint64_t l64AverageTimeUs;
    uint32_t l32Shots;
    uint32_t l32Score;
    uint8_t bytZoneHits[HARDWARE_INPUT_COUNT];
} udtFreeShootRow;

/*
 * Único contexto do módulo.
 *
 * Tudo que precisa sobreviver a uma queda de conexão está aqui.
 *
 * O relógio usado é sempre esp_timer_get_time(). O contexto guarda apenas:
 *
 * - início da rodada;
 * - duração final da rodada.
 *
 * Não existe acumulador de sessão, pausa/resume de timer ou módulo intermediário
 * de cronômetro.
 *
 * Somente free_shoot_task() usa este contexto durante a operação normal.
 * Callbacks WebSocket apenas publicam eventos na fila.
 */
typedef struct {
    udtFreeShootState udtState;
    uint64_t l64RoundStartTimestampUs;
    uint64_t l64RoundElapsedTimeUs;
    uint32_t l32TotalShots;
    uint32_t l32TotalScore;
    uint32_t l32DroppedEvents;
    bool blnRoundActive;
    udtFreeShootRow udtRows[FREE_SHOOT_TABLE_ROW_COUNT];
} udtFreeShootContext;

_Static_assert(
    FREE_SHOOT_TABLE_ROW_COUNT ==
        (FREE_SHOOT_STATE_ROUND_5 - FREE_SHOOT_STATE_ROUND_1 + 1),
    "There must be one ROUND_n state for each table row"
);

static const char *mptrTag = "FREE_SHOOT";
static QueueHandle_t mptrControlQueue = NULL;
static TaskHandle_t mptrTaskHandle = NULL;
static udtFreeShootContext mudtContext;
static bool mblnInitialized = false;

static const uint32_t ml32ZoneScore[HARDWARE_INPUT_COUNT] = {
    [HARDWARE_INPUT_1] = FREE_SHOOT_ZONE_1_SCORE,
    [HARDWARE_INPUT_2] = FREE_SHOOT_ZONE_2_SCORE,
    [HARDWARE_INPUT_3] = FREE_SHOOT_ZONE_3_SCORE,
};

/* --------------------------------------------------------------------------
 * 2. FUNÇÕES BÁSICAS DO ESTADO E DA PÁGINA
 * -------------------------------------------------------------------------- */

/**
 * @brief Captura o relógio monotônico absoluto do ESP-IDF.
 *
 * @return Timestamp atual em microssegundos; zero se a API retornar um valor
 * negativo.
 *
 * @note A função não mantém estado. Início, disparos e fim da rodada são apenas
 * leituras do mesmo relógio absoluto.
 */
static uint64_t free_shoot_capture_time_us(void)
{
    const int64_t l64TimestampUs = esp_timer_get_time();
    return l64TimestampUs > 0 ? (uint64_t)l64TimestampUs : 0;
}

/**
 * @brief Verifica se um estado representa uma das cinco rodadas.
 *
 * @param udtState Estado que será classificado.
 *
 * @return true para ROUND_1...ROUND_5; false para os demais estados.
 *
 * @note A função não acessa hardware nem modifica o contexto.
 */
static bool free_shoot_is_round(udtFreeShootState udtState)
{
    return udtState >= FREE_SHOOT_STATE_ROUND_1 &&
           udtState <= FREE_SHOOT_STATE_ROUND_5;
}

/**
 * @brief Converte a rodada atual no índice da respectiva linha.
 *
 * @return Índice de 0 a 4 correspondente a ROUND_1...ROUND_5.
 *
 * @note Deve ser chamada somente quando free_shoot_is_round() for true.
 */
static uint8_t free_shoot_row_index(void)
{
    return (uint8_t)(
        mudtContext.udtState - FREE_SHOOT_STATE_ROUND_1
    );
}

/**
 * @brief Calcula o estado selecionado após o evento PAUSAR.
 *
 * @param udtState Rodada ativa que está sendo encerrada.
 *
 * @return Rodada seguinte para ROUND_1...ROUND_4 ou COMPLETE para ROUND_5.
 */
static udtFreeShootState free_shoot_next_round_state(
    udtFreeShootState udtState
)
{
    if (udtState < FREE_SHOOT_STATE_ROUND_5) {
        return (udtFreeShootState)(udtState + 1);
    }

    return FREE_SHOOT_STATE_COMPLETE;
}

/**
 * @brief Obtém o nome textual do estado armazenado no contexto.
 *
 * @return Ponteiro para literal estático; nunca precisa ser liberado.
 *
 * @note Retorna "UNKNOWN" se o valor do enum estiver fora do intervalo.
 */
static const char *free_shoot_state_name(void)
{
    static const char *const ptrNames[] = {
        "IDLE",
        "ROUND_1",
        "ROUND_2",
        "ROUND_3",
        "ROUND_4",
        "ROUND_5",
        "STOPPED",
        "COMPLETE",
        "ERROR",
    };

    /*
     * Depois de PAUSAR, o estado já aponta para a próxima ROUND_n, mas essa
     * nova rodada ainda não foi iniciada. "PAUSED" mantém o cronômetro superior
     * congelado no JavaScript sem alterar o HTML.
     */
    if (free_shoot_is_round(mudtContext.udtState) &&
        !mudtContext.blnRoundActive) {
        return "PAUSED";
    }

    if (mudtContext.udtState < FREE_SHOOT_STATE_IDLE ||
        mudtContext.udtState > FREE_SHOOT_STATE_ERROR) {
        return "UNKNOWN";
    }

    return ptrNames[mudtContext.udtState];
}

/**
 * @brief Zera integralmente a sessão e define um novo estado.
 *
 * @param udtState Estado que será gravado depois da limpeza.
 *
 * @note Como a task Free Shoot é a única proprietária de mudtContext, não é
 * necessário usar spinlock durante o memset.
 */
static void free_shoot_reset(udtFreeShootState udtState)
{
    memset(&mudtContext, 0, sizeof(mudtContext));
    mudtContext.udtState = udtState;
}

/*
 * Envia os campos gerais da página.
 * O próprio envio atualiza o tempo corrente da rodada e as perdas atuais.
 */
/**
 * @brief Atualiza e envia os campos gerais do contexto da página.
 *
 * @return ESP_OK quando o envio foi agendado ou o erro de formatação/rede.
 *
 * @note Atualiza l64RoundElapsedTimeUs e l32DroppedEvents antes de montar o
 * frame textual CTX. Deve executar na task Free Shoot, nunca em ISR.
 */
static esp_err_t free_shoot_send_status(void)
{
    if (mudtContext.blnRoundActive) {
        const uint64_t l64NowUs = free_shoot_capture_time_us();

        mudtContext.l64RoundElapsedTimeUs =
            l64NowUs >= mudtContext.l64RoundStartTimestampUs
                ? l64NowUs - mudtContext.l64RoundStartTimestampUs
                : 0;
    }

    mudtContext.l32DroppedEvents = hardware_inputs_get_dropped_count();

    uint8_t bytMessage[96];
    const int32_t l32Length = (int32_t)snprintf(
        (char *)bytMessage,
        sizeof(bytMessage),
        "CTX|%s|%llu|%lu",
        free_shoot_state_name(),
        (unsigned long long)(
            mudtContext.l64RoundElapsedTimeUs / 1000ULL
        ),
        (unsigned long)mudtContext.l32DroppedEvents
    );

    if (l32Length <= 0 ||
        (size_t)l32Length >= sizeof(bytMessage)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return websocket_send_text_copy(
        (const char *)bytMessage,
        (size_t)l32Length
    );
}

/**
 * @brief Converte e envia uma linha no pacote fixo do alvo.
 *
 * @param bytRowIndex Índice da linha do contexto, entre 0 e 4.
 * @param bytHitZone Zona que deve piscar, ou zero durante recuperação.
 *
 * @return ESP_OK quando o pacote foi agendado ou o erro correspondente.
 *
 * @note O serviço WebSocket copia o pacote antes do retorno desta função.
 */
static esp_err_t free_shoot_send_row(
    uint8_t bytRowIndex,
    uint8_t bytHitZone
)
{
    const udtFreeShootRow *ptrRow =
        &mudtContext.udtRows[bytRowIndex];
    udtTargetPacket udtPacket;

    if (!target_packet_build(
            &udtPacket,
            mudtContext.l32TotalShots,
            mudtContext.l32TotalScore,
            bytHitZone,
            (uint8_t)(bytRowIndex + 1),
            ptrRow->bytZoneHits[HARDWARE_INPUT_1],
            ptrRow->bytZoneHits[HARDWARE_INPUT_2],
            ptrRow->bytZoneHits[HARDWARE_INPUT_3],
            ptrRow->l64ReactionTimeUs,
            ptrRow->l32Shots,
            ptrRow->l64AverageTimeUs,
            ptrRow->l32Score
        )) {
        return ESP_ERR_INVALID_ARG;
    }

    return websocket_send_target_packet(&udtPacket);
}

/*
 * Recuperação completa da página.
 *
 * As cinco linhas são enviadas inclusive quando estão zeradas. Isso garante
 * que uma página reconectada não mantenha valores antigos.
 */
/**
 * @brief Envia o estado geral e todas as cinco linhas da página.
 *
 * @note É usada após transições e reconexões. Se não houver cliente, retorna
 * sem apagar ou modificar os resultados acumulados.
 */
static void free_shoot_send_all(void)
{
    if (!websocket_service_has_client()) {
        return;
    }

    esp_err_t udtResult = free_shoot_send_status();

    for (uint8_t bytRowIndex = 0;
         udtResult == ESP_OK &&
         bytRowIndex < FREE_SHOOT_TABLE_ROW_COUNT;
         ++bytRowIndex) {
        udtResult = free_shoot_send_row(bytRowIndex, 0);
    }

    if (udtResult != ESP_OK) {
        ESP_LOGW(mptrTag, "Page update failed: %s",
                 esp_err_to_name(udtResult));
    }
}

/* --------------------------------------------------------------------------
 * 3. ENTRADAS DA MÁQUINA
 * -------------------------------------------------------------------------- */

/**
 * @brief Publica um evento de controle para a task da máquina.
 *
 * @param udtEvent Evento interno que deve ser processado.
 *
 * @note A inserção usa timeout zero para não bloquear a task HTTP. Uma fila
 * cheia é registrada, mas nenhuma regra de estado é executada no callback.
 */
static void free_shoot_post(udtFreeShootEvent udtEvent)
{
    if (mptrControlQueue == NULL ||
        xQueueSend(mptrControlQueue, &udtEvent, 0) != pdPASS) {
        ESP_LOGE(mptrTag, "Control queue full");
    }
}

/**
 * @brief Traduz um comando WebSocket para um evento da máquina.
 *
 * @param udtCommand Comando já validado pelo transporte WebSocket.
 *
 * @note Executa na task HTTP e somente publica na fila de controle.
 */
static void free_shoot_command_callback(
    udtWebsocketCommand udtCommand
)
{
    switch (udtCommand) {
        case WEBSOCKET_COMMAND_START:
            free_shoot_post(FREE_SHOOT_EVENT_START);
            break;
        case WEBSOCKET_COMMAND_PAUSE:
            free_shoot_post(FREE_SHOOT_EVENT_PAUSE);
            break;
        case WEBSOCKET_COMMAND_STOP:
            free_shoot_post(FREE_SHOOT_EVENT_STOP);
            break;
        case WEBSOCKET_COMMAND_CLEAR:
            free_shoot_post(FREE_SHOOT_EVENT_CLEAR);
            break;
        default:
            break;
    }
}

/**
 * @brief Solicita o reenvio do contexto após um novo handshake.
 *
 * @note O contexto não é lido no callback; a recuperação será executada pela
 * task Free Shoot ao consumir FREE_SHOOT_EVENT_CONNECTED.
 */
static void free_shoot_connected_callback(void)
{
    free_shoot_post(FREE_SHOOT_EVENT_CONNECTED);
}

/*
 * Tenta habilitar os GPIOs. Em falha, a rodada e a captura são fechadas e o
 * estado ERROR fica disponível para recuperação pela página.
 */
/**
 * @brief Habilita as entradas físicas para a rodada selecionada.
 *
 * @return true quando todos os GPIOs foram habilitados; false em falha.
 *
 * @note Em falha, fecha a captura, desativa a rodada e publica ERROR para
 * impedir que a página mostre uma rodada incapaz de registrar disparos.
 */
static bool free_shoot_enable_capture(void)
{
    const esp_err_t udtResult =
        hardware_inputs_set_capture_enabled(true);

    if (udtResult == ESP_OK) {
        return true;
    }

    (void)hardware_inputs_set_capture_enabled(false);
    mudtContext.blnRoundActive = false;
    mudtContext.udtState = FREE_SHOOT_STATE_ERROR;

    ESP_LOGE(mptrTag, "Capture error: %s",
             esp_err_to_name(udtResult));
    return false;
}

/*
 * Atualiza o contexto com um disparo.
 * O estado escolhe a linha; o disparo nunca muda de estado.
 */
/**
 * @brief Aplica um disparo físico à linha indicada pelo estado atual.
 *
 * @param ptrShot Evento retirado da fila de hardware.
 *
 * @note Valida entrada e rodada ativa. A reação é calculada somente no primeiro
 * disparo da rodada; disparos posteriores atualizam apenas contadores,
 * pontuação e zonas. A média permanece inalterada até PAUSAR.
 */
static void free_shoot_process_shot(
    const udtHardwareInputEvent *ptrShot
)
{
    if (ptrShot == NULL ||
        ptrShot->udtInputId < HARDWARE_INPUT_1 ||
        ptrShot->udtInputId >= HARDWARE_INPUT_COUNT ||
        !free_shoot_is_round(mudtContext.udtState) ||
        !mudtContext.blnRoundActive) {
        return;
    }

    const uint8_t bytRowIndex = free_shoot_row_index();
    udtFreeShootRow *ptrRow =
        &mudtContext.udtRows[bytRowIndex];

    const uint32_t l32Score =
        ml32ZoneScore[ptrShot->udtInputId];

    /*
     * O tempo de reação é uma informação única da rodada.
     *
     * Antes do incremento, l32Shots igual a zero identifica precisamente o
     * primeiro disparo da linha. Somente ele grava l64ReactionTimeUs.
     *
     * Não existe comparação com o disparo anterior e, por isso, o contexto não
     * precisa armazenar "último timestamp".
     */
    if (ptrRow->l32Shots == 0) {
        ptrRow->l64ReactionTimeUs =
            ptrShot->l64TimestampUs >=
                mudtContext.l64RoundStartTimestampUs
                ? ptrShot->l64TimestampUs -
                    mudtContext.l64RoundStartTimestampUs
                : 0;
    }

    mudtContext.l32TotalShots++;
    mudtContext.l32TotalScore += l32Score;

    ptrRow->l32Shots++;
    ptrRow->l32Score += l32Score;
    ptrRow->bytZoneHits[ptrShot->udtInputId]++;

    if (websocket_service_has_client()) {
        const esp_err_t udtResult =
            free_shoot_send_row(
                bytRowIndex,
                (uint8_t)ptrShot->udtInputId + 1
            );

        if (udtResult == ESP_OK) {
            (void)free_shoot_send_status();
        }
    }
}

/*
 * Processa eventos já capturados depois que os GPIOs são fechados.
 * O timeout zero impede espera por eventos novos.
 */
/**
 * @brief Processa todos os disparos já existentes na fila física.
 *
 * @note Deve ser chamada depois de fechar os GPIOs e antes de capturar o fim da
 * rodada. A leitura usa timeout zero e termina assim que a fila estiver vazia.
 */
static void free_shoot_drain_shots(void)
{
    udtHardwareInputEvent udtShot;

    while (hardware_inputs_receive_event(&udtShot, 0)) {
        free_shoot_process_shot(&udtShot);
    }
}

/* --------------------------------------------------------------------------
 * 4. TRANSIÇÕES
 * -------------------------------------------------------------------------- */

/**
 * @brief Trata o evento INICIAR conforme o estado atual.
 *
 * @note Em ROUND_n inativo, inicia a rodada selecionada com tempo zero. Em
 * qualquer estado não pertencente a uma rodada, cria uma sessão limpa em
 * ROUND_1. Um INICIAR repetido durante rodada ativa é idempotente.
 */
static void free_shoot_start(void)
{
    /*
     * ROUND_n inativo significa "nova rodada selecionada".
     *
     * INICIAR captura a origem usada pelo primeiro tempo de reação, zera o
     * tempo superior e abre os GPIOs.
     */
    if (free_shoot_is_round(mudtContext.udtState)) {
        if (!mudtContext.blnRoundActive) {
            const uint64_t l64StartTimestampUs =
                free_shoot_capture_time_us();

            mudtContext.l64RoundStartTimestampUs =
                l64StartTimestampUs;
            mudtContext.l64RoundElapsedTimeUs = 0;
            mudtContext.blnRoundActive = true;
            (void)free_shoot_enable_capture();
        }

        free_shoot_send_all();
        return;
    }

    /* Qualquer estado que não seja ROUND_n inicia uma sessão limpa. */
    (void)hardware_inputs_set_capture_enabled(false);
    hardware_inputs_clear_queue();
    hardware_inputs_reset_dropped_count();
    free_shoot_reset(FREE_SHOOT_STATE_ROUND_1);

    const uint64_t l64StartTimestampUs =
        free_shoot_capture_time_us();

    mudtContext.l64RoundStartTimestampUs =
        l64StartTimestampUs;
    mudtContext.l64RoundElapsedTimeUs = 0;
    mudtContext.blnRoundActive = true;
    (void)free_shoot_enable_capture();
    free_shoot_send_all();
}

/**
 * @brief Encerra a rodada ativa e seleciona a rodada seguinte.
 *
 * @note Fecha GPIOs, drena eventos, captura a duração final, calcula a média e
 * então avança. PAUSAR em rodada inativa não avança novamente. ROUND_5 resulta
 * em COMPLETE.
 */
static void free_shoot_pause(void)
{
    /*
     * PAUSAR só avança uma rodada ativa. Assim, pressionar PAUSAR novamente
     * antes de INICIAR não salta outra linha.
     */
    if (!free_shoot_is_round(mudtContext.udtState) ||
        !mudtContext.blnRoundActive) {
        free_shoot_send_all();
        return;
    }

    (void)hardware_inputs_set_capture_enabled(false);

    /*
     * O fim é capturado imediatamente depois de fechar os GPIOs. O tempo gasto
     * processando a fila e enviando a página não pertence à duração da rodada.
     */
    const uint64_t l64PauseTimestampUs =
        free_shoot_capture_time_us();

    /*
     * Os eventos que entraram na fila antes do fechamento ainda pertencem à
     * rodada. blnRoundActive permanece verdadeiro durante a drenagem para que
     * free_shoot_process_shot() aceite esses registros.
     */
    free_shoot_drain_shots();

    mudtContext.l64RoundElapsedTimeUs =
        l64PauseTimestampUs >= mudtContext.l64RoundStartTimestampUs
            ? l64PauseTimestampUs -
                mudtContext.l64RoundStartTimestampUs
            : 0;
    mudtContext.blnRoundActive = false;

    /*
     * A média não é uma média das reações individuais. Ela é definida pelo
     * requisito como duração total da rodada dividida pelos disparos.
     */
    udtFreeShootRow *ptrCompletedRow =
        &mudtContext.udtRows[free_shoot_row_index()];

    ptrCompletedRow->l64AverageTimeUs =
        ptrCompletedRow->l32Shots > 0
            ? mudtContext.l64RoundElapsedTimeUs /
                ptrCompletedRow->l32Shots
            : 0;

    mudtContext.udtState =
        free_shoot_next_round_state(mudtContext.udtState);
    free_shoot_send_all();
}

/**
 * @brief Encerra antecipadamente a sessão atual.
 *
 * @note Preserva os resultados, captura o tempo corrente, limpa eventos
 * residuais e publica STOPPED. Não calcula média nem seleciona outra rodada.
 */
static void free_shoot_stop(void)
{
    if (!free_shoot_is_round(mudtContext.udtState)) {
        free_shoot_send_all();
        return;
    }

    (void)hardware_inputs_set_capture_enabled(false);

    if (mudtContext.blnRoundActive) {
        const uint64_t l64StopTimestampUs =
            free_shoot_capture_time_us();

        free_shoot_drain_shots();

        mudtContext.l64RoundElapsedTimeUs =
            l64StopTimestampUs >=
                mudtContext.l64RoundStartTimestampUs
                ? l64StopTimestampUs -
                    mudtContext.l64RoundStartTimestampUs
                : 0;
        mudtContext.blnRoundActive = false;
    }

    hardware_inputs_clear_queue();
    mudtContext.udtState = FREE_SHOOT_STATE_STOPPED;
    free_shoot_send_all();
}

/**
 * @brief Limpa sessão, capturas de tempo, filas e diagnósticos.
 *
 * @note A operação converge qualquer estado para IDLE e envia o contexto
 * zerado ao navegador conectado.
 */
static void free_shoot_clear(void)
{
    (void)hardware_inputs_set_capture_enabled(false);
    hardware_inputs_clear_queue();
    hardware_inputs_reset_dropped_count();
    free_shoot_reset(FREE_SHOOT_STATE_IDLE);
    free_shoot_send_all();
}

/**
 * @brief Despacha um evento para a transição correspondente.
 *
 * @param udtEvent Evento removido da fila de controle.
 *
 * @note Este é o único switch que relaciona eventos externos às ações da
 * máquina de estados.
 */
static void free_shoot_handle(udtFreeShootEvent udtEvent)
{
    switch (udtEvent) {
        case FREE_SHOOT_EVENT_START:
            free_shoot_start();
            break;
        case FREE_SHOOT_EVENT_PAUSE:
            free_shoot_pause();
            break;
        case FREE_SHOOT_EVENT_STOP:
            free_shoot_stop();
            break;
        case FREE_SHOOT_EVENT_CLEAR:
            free_shoot_clear();
            break;
        case FREE_SHOOT_EVENT_CONNECTED:
            free_shoot_send_all();
            break;
        default:
            break;
    }
}

/*
 * Única task proprietária de mudtContext.
 *
 * Primeiro ela drena os comandos da página. Depois aguarda, por no máximo
 * FREE_SHOOT_TASK_POLL_MS, o próximo disparo físico.
 */
/**
 * @brief Executa continuamente a máquina Free Shoot.
 *
 * @param ptrArgument Parâmetro de criação da task; não utilizado.
 *
 * @note A task é a única proprietária de mudtContext. Primeiro drena comandos
 * e depois aguarda um disparo pelo período configurado.
 */
static void free_shoot_task(void *ptrArgument)
{
    (void)ptrArgument;

    while (true) {
        udtFreeShootEvent udtEvent;

        while (xQueueReceive(
                   mptrControlQueue,
                   &udtEvent,
                   0
               ) == pdPASS) {
            free_shoot_handle(udtEvent);
        }

        udtHardwareInputEvent udtShot;

        if (hardware_inputs_receive_event(
                &udtShot,
                pdMS_TO_TICKS(FREE_SHOOT_TASK_POLL_MS)
            )) {
            free_shoot_process_shot(&udtShot);
        }
    }
}

/**
 * @brief Inicializa todos os recursos do modo Free Shoot.
 *
 * @return ESP_OK em sucesso ou ESP_ERR_NO_MEM para falha de fila/task.
 *
 * @note hardware_inputs_init() deve ser executada antes. A função é idempotente
 * depois da primeira inicialização bem-sucedida.
 */
esp_err_t free_shoot_mode_init(void)
{
    if (mblnInitialized) {
        return ESP_OK;
    }

    mptrControlQueue = xQueueCreate(
        FREE_SHOOT_CONTROL_QUEUE_LENGTH,
        sizeof(udtFreeShootEvent)
    );

    if (mptrControlQueue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    free_shoot_reset(FREE_SHOOT_STATE_IDLE);

    websocket_service_set_callbacks(
        free_shoot_command_callback,
        free_shoot_connected_callback
    );

    const BaseType_t udtTaskCreated = xTaskCreate(
        free_shoot_task,
        "free_shoot_mode",
        FREE_SHOOT_TASK_STACK_SIZE,
        NULL,
        FREE_SHOOT_TASK_PRIORITY,
        &mptrTaskHandle
    );

    if (udtTaskCreated != pdPASS) {
        websocket_service_set_callbacks(NULL, NULL);
        vQueueDelete(mptrControlQueue);
        mptrControlQueue = NULL;
        return ESP_ERR_NO_MEM;
    }

    mblnInitialized = true;
    ESP_LOGI(mptrTag, "Free Shoot initialized");
    return ESP_OK;
}
