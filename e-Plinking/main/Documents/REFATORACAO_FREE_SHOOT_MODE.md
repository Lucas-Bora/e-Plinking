# Refatoração do Free Shoot Mode

## 1. Objetivo desta versão

Esta versão corrige o conceito de tempo do modo Free Shoot e remove a camada
de cronômetro que tornava uma operação simples difícil de acompanhar.

O comportamento implementado agora é exatamente:

1. `INICIAR` captura o instante inicial da rodada e habilita os disparos;
2. cada disparo captura seu próprio instante;
3. a reação é capturada uma única vez por rodada;
4. ela é a diferença entre o primeiro disparo e o `INICIAR` da rodada;
5. o cronômetro superior representa somente a duração da rodada atual;
6. `PAUSAR` captura o fim da rodada e congela esse cronômetro;
7. a média é calculada somente em `PAUSAR`;
8. a média é `tempo total da rodada / disparos da rodada`;
9. `PAUSAR` seleciona a rodada seguinte;
10. o próximo `INICIAR` começa essa rodada com o tempo novamente em zero.

Nenhum arquivo HTML foi alterado.

## 2. Modelo de tempo

Existe uma única fonte de tempo:

```c
esp_timer_get_time()
```

Ela fornece um relógio monotônico absoluto em microssegundos.

Não existe mais um “objeto timer” com acumulador, origem, pausa e retomada.
Início, disparos e fim são somente capturas do mesmo relógio.

### 2.1 Início da rodada

Ao receber `INICIAR`:

```text
início da rodada = relógio atual
tempo da rodada  = 0
rodada ativa     = true
```

### 2.2 Primeiro disparo

```text
reação = timestamp do primeiro disparo - início da rodada
```

### 2.3 Demais disparos

```text
não alteram o tempo de reação
```

Eles atualizam somente quantidade de disparos, zona e pontuação.

### 2.4 Pausa

Ao receber `PAUSAR`:

```text
fim da rodada   = relógio atual
tempo da rodada = fim da rodada - início da rodada
```

Se houve disparos:

```text
média = tempo da rodada / disparos da rodada
```

Se não houve disparos:

```text
média = 0
```

## 3. Estrutura removida

Foram removidos integralmente:

```text
free_shoot_timer.h
free_shoot_timer.c
```

Também foram removidas as funções:

```c
free_shoot_timer_init();
free_shoot_timer_start_new();
free_shoot_timer_pause();
free_shoot_timer_resume();
free_shoot_timer_reset();
free_shoot_timer_get_elapsed_us();
free_shoot_timer_get_elapsed_us_from_isr();
free_shoot_timer_is_running();
```

O hardware não recebe mais um ponteiro de função para consultar o tempo.
Consequentemente, também foram removidos:

```c
typedef uint64_t (*udtHardwareInputTimestampProvider)(void);

hardware_inputs_set_timestamp_provider();
```

## 4. Hardware simplificado

A ISR chama diretamente:

```c
esp_timer_get_time()
```

O evento colocado na fila continua contendo:

```c
typedef struct {
    udtHardwareInputId udtInputId;
    uint32_t l32Sequence;
    uint64_t l64TimestampUs;
} udtHardwareInputEvent;
```

O timestamp é absoluto. A ISR não conhece início de rodada, reação ou média.

Também foram removidas da interface pública funções que não eram usadas pela
aplicação:

```c
hardware_inputs_get_queued_count();
hardware_inputs_get_level();
hardware_inputs_get_name();
```

A interface física ficou limitada às operações necessárias:

```c
hardware_inputs_init();
hardware_inputs_set_capture_enabled();
hardware_inputs_receive_event();
hardware_inputs_clear_queue();
hardware_inputs_get_dropped_count();
hardware_inputs_reset_dropped_count();
```

## 5. Contexto simplificado

O contexto possui agora:

```c
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
```

Não existe mais tempo acumulado de sessão. O cronômetro superior é sempre o
tempo da rodada atual.

`blnRoundActive` diferencia:

```text
ROUND_2 + true  = rodada 2 em andamento
ROUND_2 + false = rodada 2 selecionada, aguardando INICIAR
```

## 6. Linha simplificada

A linha não guarda mais a soma das reações:

```c
typedef struct {
    uint64_t l64ReactionTimeUs;
    uint64_t l64AverageTimeUs;
    uint32_t l32Shots;
    uint32_t l32Score;
    uint8_t bytZoneHits[HARDWARE_INPUT_COUNT];
} udtFreeShootRow;
```

`l64AverageTimeUs` permanece zero durante a rodada. Ela recebe um valor somente
quando `free_shoot_pause()` conclui a rodada.

## 7. Fluxo de INICIAR

Quando já existe uma rodada selecionada:

1. captura `esp_timer_get_time()`;
2. grava a captura como início da rodada;
3. zera o tempo visual da rodada;
4. marca a rodada como ativa;
5. habilita as interrupções;
6. envia o contexto à página.

Quando o estado é `IDLE`, `STOPPED`, `COMPLETE` ou `ERROR`, antes dessas etapas
o contexto é limpo e `ROUND_1` é selecionada.

## 8. Fluxo de disparo

A ISR:

1. recebe diretamente o descritor da entrada;
2. captura `esp_timer_get_time()`;
3. aplica debounce;
4. atribui sequência;
5. coloca o evento na fila.

A task Free Shoot:

1. retira o evento mais antigo;
2. valida rodada e entrada;
3. verifica se é o primeiro disparo da rodada;
4. somente no primeiro, calcula `disparo - início da rodada`;
5. nos demais, preserva a reação já armazenada;
6. atualiza disparos, zonas e pontuação;
7. envia a linha e o status.

Nenhum cálculo de média ocorre nesse fluxo.

## 9. Fluxo de PAUSAR

`free_shoot_pause()` executa:

1. confirma que a rodada está ativa;
2. fecha as interrupções;
3. captura imediatamente o fim da rodada;
4. processa disparos que já estavam na fila;
5. calcula a duração total;
6. marca a rodada como inativa;
7. calcula a média da linha concluída;
8. avança para a próxima rodada;
9. envia o contexto completo.

Um segundo `PAUSAR`, antes de outro `INICIAR`, não avança novamente.

Depois de `ROUND_5`, o próximo estado é `COMPLETE`.

## 10. Cronômetro superior da página

Enquanto `blnRoundActive` for verdadeiro:

```text
tempo mostrado = agora - início da rodada
```

Depois de `PAUSAR`, o ESP32 envia:

```text
CTX|PAUSED|tempo_final_da_rodada|perdas
```

O JavaScript já existente entende `PAUSED` e congela o valor. Não foi
necessário alterar o HTML.

Quando a próxima rodada recebe `INICIAR`, o firmware envia:

```text
CTX|ROUND_n|0|perdas
```

Assim o cronômetro superior recomeça visualmente em zero.

## 11. PARAR e LIMPAR

### PARAR

`PARAR`:

1. fecha as interrupções;
2. captura imediatamente o tempo corrente da rodada;
3. processa disparos já capturados;
4. marca a rodada como inativa;
5. não calcula média;
6. muda para `STOPPED`.

A média não é calculada porque o requisito determina que ela só existe após
uma conclusão por `PAUSAR`.

### LIMPAR

`LIMPAR`:

1. fecha as interrupções;
2. limpa a fila;
3. zera o contador de perdas;
4. zera o contexto;
5. muda para `IDLE`.

## 12. Estados

Cada linha continua sendo um estado:

```text
ROUND_1
ROUND_2
ROUND_3
ROUND_4
ROUND_5
```

As transições são:

```text
IDLE --INICIAR--> ROUND_1 ativa
ROUND_1 --PAUSAR--> ROUND_2 selecionada
ROUND_2 --INICIAR--> ROUND_2 ativa
ROUND_2 --PAUSAR--> ROUND_3 selecionada
...
ROUND_5 --PAUSAR--> COMPLETE
```

Os disparos nunca mudam o estado.

## 13. GPIOs do ESP32-C3

Para ESP32-C3, os padrões continuam:

```text
Entrada 1 -> GPIO3
Entrada 2 -> GPIO4
Entrada 3 -> GPIO5
```

Antes de criar a fila, `hardware_inputs_validate_configuration()` verifica se
os GPIOs existem no alvo e se não estão duplicados.

## 14. HTML

Nenhum HTML foi alterado. O SHA-256 de `index.html` permanece:

```text
4f5a6531f79861faeca38e61868ab374e4f26971d624af41e23f91d0d9b72cbf
```

## 15. Validação

Foram validados:

- compilação de todos os módulos com `-Wall -Wextra -Werror`;
- reação medida desde `INICIAR` somente no primeiro disparo;
- disparos seguintes preservando a primeira reação;
- média permanecendo zero durante a rodada;
- média calculada em `PAUSAR`;
- fórmula `duração total / disparos`;
- avanço para a rodada seguinte;
- `PAUSAR` repetido sem novo avanço;
- cronômetro zerado no `INICIAR` seguinte;
- conclusão depois de `ROUND_5`;
- integridade do pacote de 40 bytes;
- preservação do HTML.
