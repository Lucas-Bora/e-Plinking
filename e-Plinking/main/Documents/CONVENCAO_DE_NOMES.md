# Convenção de nomes do firmware

Esta convenção é aplicada a todos os arquivos C e headers do firmware.

## 1. Prefixo de visibilidade

| Prefixo | Significado | Onde usar |
|---|---|---|
| `g` | global | variável com ligação externa, visível para outros módulos |
| `m` | modular | variável `static` pertencente exclusivamente a um módulo |
| sem prefixo | local | variável local, parâmetro de função ou campo de estrutura |

O prefixo de visibilidade vem antes do prefixo de tipo.

Exemplos:

```c
static bool mblnInitialized;
static uint32_t ml32DroppedEvents;
uint8_t bytZone;
uint64_t l64TimestampUs;
```

O firmware atual encapsula o estado de cada módulo com `static`. Por isso, não
há variáveis de aplicação com prefixo `g`. Símbolos globais fornecidos pelo
ESP-IDF ou pelo linker não são renomeados na origem externa; quando declarados
localmente, recebem um nome C compatível com esta convenção.

## 2. Prefixo de tipo

| Prefixo | Tipo representado |
|---|---|
| `bln` | `bool` |
| `byt` | `uint8_t` |
| `w` | `uint16_t` |
| `l32` | `uint32_t` |
| `l64` | `uint64_t` |
| `ptr` | ponteiro |
| `udt` | tipo declarado com `typedef` |

Exemplos:

```c
bool blnCaptureEnabled;
uint8_t bytInputIndex;
uint16_t wPort;
uint32_t l32Sequence;
uint64_t l64ElapsedUs;
const char *ptrName;
udtHardwareInputEvent udtEvent;
```

## 3. Ponteiros

`ptr` informa que a variável armazena um endereço, independentemente do tipo
apontado:

```c
udtHardwareInputEvent *ptrEvent;
const uint8_t *ptrPayload;
void *ptrArgument;
```

Quando um ponteiro é uma variável modular, os prefixos são combinados:

```c
static QueueHandle_t mptrEventQueue;
```

## 4. Typedefs

Todo identificador criado por `typedef` começa com `udt` e usa PascalCase:

```c
typedef enum {
    ...
} udtHardwareInputId;

typedef struct {
    ...
} udtFreeShootPageContext;

typedef void (*udtCallback)(void);
```

Variáveis cujo tipo é um `typedef` também recebem `udt`, salvo quando o próprio
tipo representa claramente um ponteiro opaco; nesse caso, usa-se `ptr`.

## 5. Campos de estruturas

Campos seguem somente o prefixo de tipo, pois a estrutura já determina seu
escopo:

```c
typedef struct {
    udtHardwareInputId udtInputId;
    uint32_t l32Sequence;
    uint64_t l64TimestampUs;
} udtHardwareInputEvent;
```

## 6. Funções, macros e constantes

Os nomes das funções públicas permanecem em `snake_case` para preservar a API
do projeto. Macros e valores de enum permanecem em letras maiúsculas. A
convenção desta página se aplica aos identificadores de dados e aos tipos.
