#include "hardware_inputs.h"

#include <stddef.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"

#include "app_config.h"


static const char *TAG = "HW_INPUTS"; // Texto que aparece nos logs para identificar este módulo.

/*
 * Um descritor reúne tudo que a ISR precisa saber sobre uma entrada.
 *
 * O mesmo endereço passado a gpio_isr_handler_add() volta no parâmetro
 * ptrArgument da ISR. Portanto, não são necessários três tratadores, uma
 * função seletora e outra função comum.
 */
typedef struct {
    udtHardwareInputId udtInputId;
    gpio_num_t udtGpio;
    uint64_t l64LastTimestampUs;
    bool blnHasPreviousEvent;
} udtHardwareInputDescriptor;

/*
 * Tabela modular que associa o identificador lógico ao GPIO físico.
 * Ela não é const porque os dois últimos campos guardam o debounce de cada
 * entrada. Os endereços dos três elementos permanecem estáveis durante toda a
 * execução e podem ser usados como argumentos das interrupções.
 */
static udtHardwareInputDescriptor mudtInputs[HARDWARE_INPUT_COUNT] = {
    [HARDWARE_INPUT_1] = {
        .udtInputId = HARDWARE_INPUT_1,
        .udtGpio = (gpio_num_t)HARDWARE_INPUT_1_GPIO,
    },
    [HARDWARE_INPUT_2] = {
        .udtInputId = HARDWARE_INPUT_2,
        .udtGpio = (gpio_num_t)HARDWARE_INPUT_2_GPIO,
    },
    [HARDWARE_INPUT_3] = {
        .udtInputId = HARDWARE_INPUT_3,
        .udtGpio = (gpio_num_t)HARDWARE_INPUT_3_GPIO,
    },
};

/* Fila FreeRTOS que transfere registros completos da ISR para a task. */
static QueueHandle_t mptrEventQueue = NULL;

/* Protege somente os dados curtos compartilhados entre task e ISR. */
static portMUX_TYPE mudtStateLock = portMUX_INITIALIZER_UNLOCKED;

/* Metadados globais da captura, todos privados deste módulo. */
static volatile uint32_t ml32NextSequence = 0;
static volatile uint32_t ml32DroppedEventCount = 0;
static bool mblnInitialized = false;

/* Tag usada somente por funções de task; a ISR permanece sem qualquer log. */
static const char *mptrTag = "HARDWARE_INPUTS";

/**
 * @brief Valida os GPIOs definidos em app_config.h antes de acessar o driver.
 *
 * A validação possui duas responsabilidades independentes:
 *
 * 1. confirmar que cada número representa um GPIO existente no chip para o
 *    qual o projeto foi compilado;
 * 2. confirmar que duas entradas lógicas não foram ligadas acidentalmente ao
 *    mesmo GPIO.
 *
 * A verificação acontece antes da criação da fila e antes do deslocamento
 * usado para montar pin_bit_mask. Isso é importante porque deslocar 1ULL por
 * um número de GPIO inválido já produziria uma máscara sem significado para
 * o driver.
 *
 * @return ESP_OK quando os três GPIOs são válidos e distintos, ou
 * ESP_ERR_INVALID_ARG quando a configuração não pode ser usada.
 *
 * @note Executa durante a inicialização, em contexto de task. Por isso os
 * logs de diagnóstico são seguros aqui e nunca são executados pela ISR.
 */
static esp_err_t hardware_inputs_validate_configuration(void)
{
    for (uint8_t bytInputIndex = 0;
         bytInputIndex < (uint8_t)HARDWARE_INPUT_COUNT;
         ++bytInputIndex) {
        const gpio_num_t udtGpio = mudtInputs[bytInputIndex].udtGpio;

        /*
         * GPIO_IS_VALID_GPIO() vem da camada SoC do ESP-IDF e considera o
         * alvo selecionado no build. Assim, GPIO25 é válido no ESP32 clássico,
         * mas é rejeitado corretamente quando o alvo é ESP32-C3.
         */
        if (!GPIO_IS_VALID_GPIO(udtGpio)) {
            ESP_LOGE(
                mptrTag,
                "Invalid GPIO %d configured for hardware input %u",
                (int)udtGpio,
                (unsigned int)(bytInputIndex + 1U)
            );
            return ESP_ERR_INVALID_ARG;
        }

        /*
         * Compara a entrada atual apenas com as anteriores. Isso encontra
         * duplicidades sem repetir a mesma comparação nos dois sentidos.
         */
        for (uint8_t bytPreviousIndex = 0;
             bytPreviousIndex < bytInputIndex;
             ++bytPreviousIndex) {
            if (udtGpio == mudtInputs[bytPreviousIndex].udtGpio) {
                ESP_LOGE(
                    mptrTag,
                    "GPIO %d is assigned to hardware inputs %u and %u",
                    (int)udtGpio,
                    (unsigned int)(bytPreviousIndex + 1U),
                    (unsigned int)(bytInputIndex + 1U)
                );
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    return ESP_OK;
}

/*
 * ÚNICA ISR DO MÓDULO
 *
 * A função é registrada três vezes. O que muda em cada registro é somente o
 * endereço de mudtInputs[0], [1] ou [2]. A chamada do driver chega diretamente
 * aqui; não existe tratador intermediário.
 *
 * Trabalho executado na interrupção:
 * 1. valida o descritor;
 * 2. captura diretamente o relógio absoluto do ESP-IDF;
 * 3. aplica o debounce da própria entrada;
 * 4. atribui a sequência e copia o evento para a fila;
 * 5. solicita troca de contexto apenas se uma task mais prioritária acordou.
 *
 * Não há log, alocação dinâmica, espera, cálculo de pontuação ou WebSocket.
 */
/**
 * @brief Captura uma borda física e publica seu evento na fila.
 *
 * @param ptrArgument Ponteiro para o descritor estático da entrada acionada.
 *
 * @note Executa em contexto de ISR. Não bloqueia, não aloca memória, não faz
 * logs e não executa regras do Free Shoot.
 */
static void hardware_input_isr(void *ptrArgument)
{
    udtHardwareInputDescriptor *ptrInput =
        (udtHardwareInputDescriptor *)ptrArgument;

    if (ptrInput == NULL) {
        return;
    }

    /*
     * A mesma base de tempo é usada pela máquina ao capturar o início e o fim
     * da rodada. Assim, uma simples subtração fornece tanto o tempo de reação
     * quanto o tempo total da rodada.
     */
    const int64_t l64AbsoluteTimestampUs = esp_timer_get_time();
    const uint64_t l64TimestampUs = l64AbsoluteTimestampUs > 0 ? (uint64_t)l64AbsoluteTimestampUs : 0;
    bool blnAcceptEvent = true;
    uint32_t l32Sequence = 0;

    portENTER_CRITICAL_ISR(&mudtStateLock);

    if (ptrInput->blnHasPreviousEvent &&
        l64TimestampUs >= ptrInput->l64LastTimestampUs &&
        (l64TimestampUs - ptrInput->l64LastTimestampUs) <
            HARDWARE_INPUT_DEBOUNCE_US) {
        blnAcceptEvent = false;
    }

    if (blnAcceptEvent) {
        ptrInput->blnHasPreviousEvent = true;
        ptrInput->l64LastTimestampUs = l64TimestampUs;
        l32Sequence = ++ml32NextSequence;
    }

    portEXIT_CRITICAL_ISR(&mudtStateLock);

    if (!blnAcceptEvent) {
        return;
    }

    const udtHardwareInputEvent udtEvent = {
        .udtInputId = ptrInput->udtInputId,
        .l32Sequence = l32Sequence,
        .l64TimestampUs = l64TimestampUs,
    };

    BaseType_t udtHigherPriorityTaskWoken = pdFALSE;

    
            
    
    if (xQueueSendFromISR(
            mptrEventQueue,
            &udtEvent,
            &udtHigherPriorityTaskWoken
        ) != pdPASS) {
        portENTER_CRITICAL_ISR(&mudtStateLock);
        ml32DroppedEventCount++;
        ESP_LOGI(TAG, "EVENTO DROPADO - FILA CHEIA");
        portEXIT_CRITICAL_ISR(&mudtStateLock);
    }

    if (udtHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Cria a fila, configura os GPIOs e registra a ISR genérica.
 *
 * @return ESP_OK em sucesso, ESP_ERR_NO_MEM sem fila, ou erro do driver GPIO.
 *
 * @note As interrupções terminam desabilitadas e só serão abertas por INICIAR.
 */
esp_err_t hardware_inputs_init(void)
{
    if (mblnInitialized) {
        return ESP_OK;
    }

    /*
     * Valida primeiro. Em caso de configuração errada, nenhum recurso do
     * FreeRTOS ou do driver GPIO terá sido criado parcialmente.
     */
    esp_err_t udtResult = hardware_inputs_validate_configuration();

    if (udtResult != ESP_OK) {
        return udtResult;
    }

    mptrEventQueue = xQueueCreate(
        HARDWARE_INPUT_EVENT_QUEUE_LENGTH,
        sizeof(udtHardwareInputEvent)
    );

    if (mptrEventQueue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint64_t l64PinBitMask = 0;

    for (uint8_t bytIndex = 0;
         bytIndex < (uint8_t)HARDWARE_INPUT_COUNT;
         ++bytIndex) {
        l64PinBitMask |= (1ULL << mudtInputs[bytIndex].udtGpio);
    }

    /*
     * ALTERAÇÃO NO MÓDULO DE HARDWARE
     *
     * GPIO_INTR_NEGEDGE produz uma interrupção por transição alto -> baixo.
     * Diferentemente da interrupção por nível baixo, ela não precisa ficar
     * desabilitada até um acknowledge. Assim, outro pulso posterior pode ser
     * capturado mesmo que a task ainda esteja processando o evento anterior.
     */
    const gpio_config_t udtInputConfiguration = {
        .pin_bit_mask = l64PinBitMask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = HARDWARE_INPUT_ENABLE_INTERNAL_PULLUP
            ? GPIO_PULLUP_ENABLE
            : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    udtResult = gpio_config(&udtInputConfiguration);

    if (udtResult != ESP_OK) {
        return udtResult;
    }

    udtResult = gpio_install_isr_service(0);

    if (udtResult != ESP_OK && udtResult != ESP_ERR_INVALID_STATE) {
        return udtResult;
    }

    for (uint8_t bytIndex = 0;
         bytIndex < (uint8_t)HARDWARE_INPUT_COUNT;
         ++bytIndex) {
        udtHardwareInputDescriptor *ptrInput = &mudtInputs[bytIndex];

        /*
         * O terceiro argumento é o descritor que o driver devolverá à ISR.
         * Logo, a mesma função identifica o GPIO sem switch e sem wrappers.
         */
        udtResult = gpio_isr_handler_add(
            ptrInput->udtGpio,
            hardware_input_isr,
            ptrInput
        );

        if (udtResult != ESP_OK) {
            return udtResult;
        }

        /* O firmware começa em IDLE; INICIAR abrirá as interrupções. */
        udtResult = gpio_intr_disable(ptrInput->udtGpio);

        if (udtResult != ESP_OK) {
            return udtResult;
        }
    }

    mblnInitialized = true;
    return ESP_OK;
}

/**
 * @brief Habilita ou desabilita as três interrupções de entrada.
 *
 * @param blnEnabled true para aceitar novas bordas; false para fechar captura.
 *
 * @return ESP_OK quando todos os GPIOs foram alterados ou o erro do driver.
 *
 * @note A função não limpa eventos que já estejam armazenados na fila.
 */
esp_err_t hardware_inputs_set_capture_enabled(bool blnEnabled)
{
    if (!mblnInitialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (blnEnabled) {
        portENTER_CRITICAL(&mudtStateLock);

        for (uint8_t bytIndex = 0;
             bytIndex < (uint8_t)HARDWARE_INPUT_COUNT;
             ++bytIndex) {
            mudtInputs[bytIndex].blnHasPreviousEvent = false;
            mudtInputs[bytIndex].l64LastTimestampUs = 0;
        }

        portEXIT_CRITICAL(&mudtStateLock);

        for (uint8_t bytIndex = 0;
             bytIndex < (uint8_t)HARDWARE_INPUT_COUNT;
             ++bytIndex) {
            const esp_err_t udtResult =
                gpio_intr_enable(mudtInputs[bytIndex].udtGpio);

            if (udtResult != ESP_OK) {
                (void)hardware_inputs_set_capture_enabled(false);
                return udtResult;
            }
        }

        return ESP_OK;
    }

    esp_err_t udtFinalResult = ESP_OK;

    for (uint8_t bytIndex = 0;
         bytIndex < (uint8_t)HARDWARE_INPUT_COUNT;
         ++bytIndex) {
        const esp_err_t udtResult =
            gpio_intr_disable(mudtInputs[bytIndex].udtGpio);

        if (udtResult != ESP_OK) {
            udtFinalResult = udtResult;
        }
    }

    return udtFinalResult;
}

/**
 * @brief Retira o evento mais antigo da fila física.
 *
 * @param ptrEvent Destino que receberá uma cópia completa do evento.
 * @param udtTicksToWait Timeout de espera da task em ticks do FreeRTOS.
 *
 * @return true quando um evento foi copiado; false em timeout ou uso inválido.
 *
 * @note O timeout não participa do timestamp armazenado no evento.
 */
bool hardware_inputs_receive_event(
    udtHardwareInputEvent *ptrEvent,
    TickType_t udtTicksToWait
)
{
    if (ptrEvent == NULL || mptrEventQueue == NULL) {
        return false;
    }

    return xQueueReceive(mptrEventQueue, ptrEvent, udtTicksToWait) == pdPASS;
}

/**
 * @brief Descarta eventos pendentes e reinicia sequência e debounce.
 *
 * @note Deve ser chamada com a captura fechada para delimitar sessões.
 */
void hardware_inputs_clear_queue(void)
{
    if (mptrEventQueue != NULL) {
        xQueueReset(mptrEventQueue);
    }

    portENTER_CRITICAL(&mudtStateLock);
    ml32NextSequence = 0;

    for (uint8_t bytIndex = 0;
         bytIndex < (uint8_t)HARDWARE_INPUT_COUNT;
         ++bytIndex) {
        mudtInputs[bytIndex].blnHasPreviousEvent = false;
        mudtInputs[bytIndex].l64LastTimestampUs = 0;
    }

    portEXIT_CRITICAL(&mudtStateLock);
}

/**
 * @brief Obtém o contador de eventos físicos descartados.
 *
 * @return Quantidade acumulada de perdas por fila cheia.
 */
uint32_t hardware_inputs_get_dropped_count(void)
{
    uint32_t l32Count;

    portENTER_CRITICAL(&mudtStateLock);
    l32Count = ml32DroppedEventCount;
    portEXIT_CRITICAL(&mudtStateLock);
    return l32Count;
}

/**
 * @brief Zera atomicamente o contador de eventos descartados.
 */
void hardware_inputs_reset_dropped_count(void)
{
    portENTER_CRITICAL(&mudtStateLock);
    ml32DroppedEventCount = 0;
    portEXIT_CRITICAL(&mudtStateLock);
}
