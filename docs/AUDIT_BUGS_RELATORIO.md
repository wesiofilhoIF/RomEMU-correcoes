# Relatório de Auditoria Técnica e Bugs Críticos — RomEMU

**Data:** 25/10/2026  
**Repositório:** `wesiofilhoIF/RomEMU-correcoes`  
**Escopo:** Emulação de BIOS SPI Flash (W25Q128JV / Placa-Mãe X99-XD4), Gerenciamento de Memória PSRAM, Concorrência e Conectividade.

---

## 1. Visão Geral da Auditoria

A revisão estática e dinâmica do firmware identificou **13 vulnerabilidades e defeitos de projeto**, sendo **5 falhas críticas/fatais** que impedem o funcionamento da placa-mãe ou causam corrupção de memória permanente na ingestão de imagens de BIOS.

```
┌────────────────────────────────────────────────────────────────────────┐
│                        FLUXO CRÍTICO DE FALHAS                         │
├────────────────────────────────────────────────────────────────────────┤
│ 1. SPI Slave: Resposta montada pós-transação (Hardware envia 0xFF)     │
│ 2. Task Stalling: vTaskDelay de 2s (Rainbow) no loop de alta prioridade│
│ 3. Memory Corruption: memmove destrói cabeçalho comp_sizes no LZ4      │
│ 4. Stack Overflow: >12 KB em arrays locais de pilha (Task stack: 4 KB) │
│ 5. Data Race: cstore_t compartilhado entre Core 0 e Core 1 sem locks   │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Inventário Detalhado de Bugs

### 2.1. Bugs Críticos / Fatais

#### BUG-01: Inversão Temporal do Buffer SPI (Placa recebe apenas 0xFF)
* **Localização:** `main/spi_flash_emu.c`, linhas 483–526
* **Classificação:** Crítico (Impede Boot)
* **Descrição:** O periférico de hardware SPI do ESP32 (`spi_slave_transmit`) é bloqueante e transmite o buffer DMA `ctx->tx_buf` em tempo real enquanto o mestre envia os pulsos de clock. O código inicializa `ctx->tx_buf` com `0xFF` (exceto o JEDEC ID nos bytes 1..3). A função `process_transaction()` que decodifica o comando e copia os dados da ROM para `tx_buf` só é chamada **após** a transação terminar e o CS subir. Na iteração seguinte do loop, o buffer é sobrescrito com `0xFF` via `memset`.
* **Impacto:** A placa-mãe recebe $100\%$ de bytes `0xFF` em todos os comandos de leitura de dados (`0x03`, `0x0B`), travando a máquina em bootloop.
* **Remediação:** O emulador SPI em microcontrolador sem hardware QSPI Slave dedicado precisa pré-armar descritores ou utilizar a interface SPI Slave com DMA contínuo ou interrupção de início de transação.

---

#### BUG-02: Bloqueio Bloqueante de 2 Segundos no Core 1 com Efeito Rainbow
* **Localização:** `main/spi_flash_emu.c`, linhas 528–533 e `main/status_led.c`, linhas 266–282
* **Classificação:** Crítico (Watchdog / Timeout de Barramento)
* **Descrição:** A chamada `status_led_celebrate_rainbow(2000)` é disparada na primeira transação de leitura da BIOS. Essa função executa um loop com múltiplos `vTaskDelay(pdMS_TO_TICKS(15))`, bloqueando a tarefa `spi_emu_task` (Core 1, prioridade 24) por $2.000	ext{ ms}$ contínuos.
* **Impacto:** Durante os 2 segundos de pausa, nenhuma nova transação SPI é enfileirada no periférico. O chipset Intel C612 emite as leituras seguintes em microssegundos, encontra o barramento desarmado e aborta o POST.
* **Remediação:** O controle de efeitos visuais de LED deve ser delegado a uma fila assíncrona (`xQueueSend`) executada em uma tarefa separada de baixa prioridade no Core 0.

---

#### BUG-03: Corrupção de Memória por `memmove` Sobreposto no Ingest Zero-Copy
* **Localização:** `main/compressed_store.c`, linhas 687–706
* **Classificação:** Crítico (Corrupção de Dados / Falha de Descompressão)
* **Descrição:** No método `cstore_create_from_lz4_archive()`:
  ```c
  const uint16_t *comp_sizes = (const uint16_t *)(archive + sizeof(uint32_t));
  uint8_t *data_payload = archive + header_size;
  memmove(archive, data_payload, data_payload_size);
  for (uint32_t i = 0; i < num_blocks; i++) {
      uint16_t csz = comp_sizes[i]; // Lendo área sobrescrita por memmove!
  ```
  O `memmove` transfere o payload de dados comprimidos diretamente para o início do buffer `archive`, sobrescrevendo o array `comp_sizes` antes da leitura do loop.
* **Impacto:** Todos os tamanhos de blocos passam a conter lixo de dados comprimidos (valores inválidos como 48.000 bytes). Qualquer leitura subsequente acusa erro de descompressão e retorna `0xFF`.
* **Remediação:** Copiar os tamanhos para a estrutura `cs->blocks[i]` antes de realizar o deslocamento de memória com `memmove`.

---

#### BUG-04: Tamanho de Partição Hardcoded (4.978.386 bytes) no Boot Automático
* **Localização:** `main/main.c`, linha 131
* **Classificação:** Crítico (Falha de Inicialização Persistente)
* **Descrição:** A variável `uint32_t archive_size = 4978386;` foi chumbada no código-fonte baseada em um único arquivo de teste.
* **Impacto:** Qualquer BIOS gravada com compressão diferente resulta em leitura de dados incompletos ou leitura de setores apagados (`0xFF`) da Flash interna.
* **Remediação:** Gravar o tamanho real nos primeiros 4 bytes de metadados da partição `bios_storage` ou deduzir o tamanho da estrutura do cabeçalho LZ4.

---

#### BUG-05: Rejeição de BIOS de 32MB / 64MB no Auto-Mount
* **Localização:** `main/main.c`, linha 130
* **Classificação:** Crítico (Incompatibilidade de Chips Maiores)
* **Descrição:** A condição `magic_header > 0 && magic_header <= 4096` assume que o número de blocos nunca ultrapassa 4096 (16 MB).
* **Impacto:** Chips de 32 MB (8.192 blocos) e 64 MB (16.384 blocos) falham na verificação e não são montados no boot.
* **Remediação:** Alterar o limite para `CSTORE_MAX_BLOCKS` (16.384).

---

### 2.2. Bugs de Alta Severidade (Estabilidade, Concorrência e Kernel)

#### BUG-06: Estouro de Pilha (Stack Overflow) por Buffers Gigantes
* **Localização:** `main/compressed_store.c` (linhas 93, 143, 310) e `main/lz4_block.c` (linha 50)
* **Classificação:** Alto (Panic de Kernel / Guru Meditation)
* **Descrição:** Arrays alocados na pilha:
  - `uint8_t block_buf[4096]` em `cstore_create` (duas ocorrências).
  - `uint8_t temp[4352]` em `flush_cache_line`.
  - `uint16_t hash_table[4096]` (8.192 bytes) em `lz4_compress`.
  Total alocado na pilha ultrapassa 12 KB, enquanto pilhas padrão de tarefas HTTP têm 4 KB a 8 KB.
* **Impacto:** Falha imediata de *Stack Canary* ou pânico de memória ao processar imagens pela web.
* **Remediação:** Alocar buffers temporários no heap interno via `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)`.

---

#### BUG-07: Concorrência e Data Race no Cache LRU `cstore_t`
* **Localização:** `main/compressed_store.c` (linhas 201–286, 340–401)
* **Classificação:** Alto (Race Condition / Despejo Inválido)
* **Descrição:** O cache LRU (`cs->cache`) e os contadores de acesso não possuem mecanismos de proteção (spinlock ou mutex). O Core 1 (`spi_emu_task`) e o Core 0 (`nvram_sync_task` ou endpoint de download) invocam `cstore_read()` concorrentemente.
* **Impacto:** Despejo de linha de cache em leitura ativa e corrupção de ponteiros.
* **Remediação:** Implementar proteção por spinlock (`portMUX_TYPE`) nas operações de cache.

---

#### BUG-08: Bloqueio do System Event Loop (`sys_evt`) no Wi-Fi Manager
* **Localização:** `main/wifi_manager.c`, linhas 90 e 100
* **Classificação:** Alto (Congelamento de Stack de Rede)
* **Descrição:** Execução de `vTaskDelay(pdMS_TO_TICKS(1000))` no evento `WIFI_EVENT_STA_DISCONNECTED` e chamada de `status_led_flash_wifi_orange()` (400 ms de delay) no evento `IP_EVENT_STA_GOT_IP`.
* **Impacto:** Congela a tarefa do despachante central de eventos do sistema operacional, atrasando DHCP e conexões de sockets.
* **Remediação:** Remover delays síncronos de dentro dos manipuladores de eventos.

---

#### BUG-09: Congelamento Síncrono do Servidor HTTP em Funções de Energia
* **Localização:** `main/gpio_control.c`, linhas 86–95, 115–125 e `main/web_handlers.c`, linha 865
* **Classificação:** Alto (Timeout de Requisição HTTP)
* **Descrição:** `gpio_control_power_off()` segura a linha por 5 segundos com `vTaskDelay(5000)`, e `power_cycle` bloqueia por 6,8 segundos dentro da thread de atendimento do servidor web.
* **Impacto:** O navegador acusa erro 504 Gateway Timeout ou timeout de socket TCP.
* **Remediação:** Executar ciclos de energia em uma tarefa FreeRTOS em segundo plano.

---

#### BUG-10: Condição de Corrida no Buffer Circular do Access Log
* **Localização:** `main/access_log.c`, linhas 12–22 e 39–45
* **Classificação:** Alto (Leitura Corrompida de Log)
* **Descrição:** Quando o buffer de log atinge o limite, `access_log_push` (Core 1) e `access_log_pop` (Core 0) realizam escrita simultânea em `s_log.read_idx` sem operações atômicas.
* **Impacto:** Perda de registros e potencial acesso fora dos limites válidos do array.
* **Remediação:** Utilizar primitivas atômicas ou máscaras de interrupção portENTER_CRITICAL.

---

### 2.3. Bugs de Média Severidade (Portabilidade e Otimização)

#### BUG-11: Conflito de Hardware do LED no ESP32-P4 (GPIO 48)
* **Localização:** `main/status_led.c`, linha 16 vs `main/pin_config.h`, linha 35
* **Descrição:** O driver RMT inicializa os pinos GPIO 48 e 38 para WS2812. No ESP32-P4, o pino GPIO 48 é exatamente o `PIN_SPI_MISO`.
* **Impacto:** O driver de LED colide com a linha de dados de saída da BIOS em placas P4.
* **Remediação:** Condicionar a lista de pinos ao target `#if CONFIG_IDF_TARGET_ESP32S3`.

---

#### BUG-12: Declaração Duplicada de Tabela em Arquivo de Cabeçalho
* **Localização:** `main/spi_flash_commands.h`, linha 44
* **Descrição:** A tabela `spi_chip_db[]` está declarada como `static const` no arquivo `.h`, gerando cópias duplicadas em rodata em cada unidade de compilação.
* **Remediação:** Declarar `extern const spi_chip_info_t spi_chip_db[]` e definir em um único arquivo `.c`.

---

#### BUG-13: Truncamento de Transações Longas (> 4KB)
* **Localização:** `main/spi_flash_emu.c`, linha 30
* **Descrição:** `SPI_BUF_SIZE` está fixo em 4096 bytes. Em leituras contínuas com `CS#` mantido em nível baixo durante descompactação de volumes UEFI volumosos, a transação trunca.
* **Remediação:** Suporte a encadeamento de buffers DMA (*linked list DMA*) ou redimensionamento do buffer.

---

## 3. Matriz de Severidade e Priorização

| ID | Módulo | Severidade | Impacto Primário | Status |
|:---|:---|:---:|:---|:---:|
| **BUG-01** | `spi_flash_emu.c` | **CRÍTICO** | Placa recebe apenas 0xFF (Bootloop) | Identificado |
| **BUG-02** | `spi_flash_emu.c` | **CRÍTICO** | Bloqueio de 2s da tarefa SPI pelo LED | Identificado |
| **BUG-03** | `compressed_store.c`| **CRÍTICO** | `memmove` corrompe tabela de índices | Identificado |
| **BUG-04** | `main.c` | **CRÍTICO** | Tamanho chumbado (4978386) no auto-mount | Identificado |
| **BUG-05** | `main.c` | **CRÍTICO** | Limite rígido de 16MB no boot | Identificado |
| **BUG-06** | `compressed_store.c`| **ALTO** | Estouro de pilha (>12KB stack) | Identificado |
| **BUG-07** | `compressed_store.c`| **ALTO** | Data race no cache LRU sem mutex | Identificado |
| **BUG-08** | `wifi_manager.c` | **ALTO** | Bloqueio do loop `sys_evt` por delays | Identificado |
| **BUG-09** | `gpio_control.c` | **ALTO** | Timeout HTTP em power off (5s delay) | Identificado |
| **BUG-10** | `access_log.c` | **ALTO** | Concorrência sem atomic em buffer de log | Identificado |
| **BUG-11** | `status_led.c` | **MÉDIO** | Colisão de GPIO 48 (MISO) no ESP32-P4 | Identificado |
| **BUG-12** | `spi_flash_commands.h`| **MÉDIO** | Desperdício de flash com rodata estático | Identificado |
| **BUG-13** | `spi_flash_emu.c` | **MÉDIO** | Truncamento de leituras > 4KB | Identificado |
