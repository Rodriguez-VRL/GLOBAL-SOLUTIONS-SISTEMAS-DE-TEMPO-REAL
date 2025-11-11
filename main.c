/*
 * Global Solution 2 - Monitor de Redes Wi-Fi Seguras (ESP-IDF Nativo)
 * Aluno: VINICIUS RODRIGUES
 * RM: 89192
 * * Este código usa o framework ESP-IDF nativo (app_main) e
 * foi simulado no wokwi.
 */

// --- Bibliotecas ---
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h" // Para o Semáforo
#include "freertos/event_groups.h" // Para eventos Wi-Fi
#include "esp_task_wdt.h" // Para o Watchdog Timer (Robustez)
#include "esp_system.h"
#include "esp_wifi.h"       // Biblioteca Wi-Fi nativa do ESP-IDF
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"      // Para o Wi-Fi
#include "driver/gpio.h"    // Para o LED

// --- Definições do Projeto ---
#define LED_VERMELHO GPIO_NUM_18 // Pino 18
#define NOME_ALUNO "VINICIUS RODRIGUES-RM:89192"

// --- Configurações de Tempo ---
#define TEMPO_SCAN_MS 10000     // 10 segundos
#define TEMPO_ALERTA_MS 500     // LED pisca rápido
#define TIMEOUT_FILA_MS 30000   // 30 segundos (Robustez 1)
#define WDT_TIMEOUT_S 5         // Watchdog de 5s (Robustez 2)

// --- Credenciais Wi-Fi ---
// No Wokwi, o ESP32 se conecta à rede simulada "Wokwi-GUEST".
#define WIFI_SSID      "Wokwi-GUEST"
#define WIFI_PASSWORD  ""

// --- Lista Segura de Redes ---
/*
 * !!! IMPORTANTE PARA O TESTE NO WOKWI !!!
 * O Wokwi sempre se conecta à rede "Wokwi-GUEST".
 *
 * Cenário 1: TESTE SEGURO
 * Deixe "Wokwi-GUEST" na lista..
 *
 * Cenário 2: TESTE DE ALERTA (NÃO PERMITIDA / Inseguro)
  remova a linha "Wokwi-GUEST" da lista.
 */
const char* LISTA_SEGURA[5] = {
    "Wokwi-GUEST",        // <- Rede simulada do Wokwi
    "WIFI_FIAP",
    "WIFI_DO_TRABALHO",
    "Rede_Casa_5G",
    "Celular_Vinicius"
};

// --- Handles Globais do FreeRTOS ---
QueueHandle_t fila_ssid;
SemaphoreHandle_t semaforo_lista_segura;
EventGroupHandle_t s_wifi_event_group; // Event group para sinalizar conexão

// Bits do Event Group do Wi-Fi
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* * Flag global de status para o LED:
 * 0 = OK (Seguro)
 * 1 = ALERTA (Rede Insegura)
 * 2 = FALHA (Scanner travou / Timeout)
 */
volatile int g_status_rede = 0; 
static const char *TAG = "WIFI_MONITOR"; // Tag para logs do sistema


// --- Event Handler (Função para o Wi-Fi) ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        ESP_LOGI(TAG, "Conexao falhou. Tentando reconectar...");
        esp_wifi_connect(); // Tenta reconectar
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- Função de Conexão Wi-Fi (Versão ESP-IDF) ---
void setup_wifi() {
    // 1. Inicializa NVS (Necessário para o Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    // 2. Inicializa a pilha de rede e o loop de eventos
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. Configura o Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. Registra os handlers de evento
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // 5. Define a configuração (SSID e Senha)
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    
    // 6. Inicia o Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start() );
    ESP_LOGI(TAG, "Conectando ao %s...", WIFI_SSID);

    // 7. Espera até a conexão ser estabelecida (ou falhar)
    xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);
}


// --- TAREFA 1: Scanner (Prioridade Baixa) ---
// Verifica o SSID atual e envia para a fila
void task_scanner(void *pvParameters) {
    esp_task_wdt_add(NULL); 
    char ssid_atual[64];

    for(;;) {
        // Verifica se o bit de "conectado" está ativo
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            
            // Pega o SSID da configuração atual
            wifi_config_t conf;
            esp_wifi_get_config(ESP_IF_WIFI_STA, &conf);
            strncpy(ssid_atual, (char*)conf.sta.ssid, 64);
            
            // Envia para a Fila
            if (xQueueSend(fila_ssid, &ssid_atual, pdMS_TO_TICKS(1000)) != pdTRUE) {
                printf("{%s} [SCANNER] ERRO: Fila de SSID cheia!\n", NOME_ALUNO);
            }
        } else {
            // Se não está conectado, envia "DESCONECTADO"
            strncpy(ssid_atual, "DESCONECTADO", 64);
            xQueueSend(fila_ssid, &ssid_atual, pdMS_TO_TICKS(1000));
        }
        
        esp_task_wdt_reset(); 
        vTaskDelay(pdMS_TO_TICKS(TEMPO_SCAN_MS)); 
    }
}


// --- TAREFA 2: Validador (Prioridade Média) ---
// Recebe da fila, usa o Semáforo, checa a lista
void task_validador(void *pvParameters) {
    esp_task_wdt_add(NULL); 
    char ssid_recebido[64];
    
    for(;;) {
        // Espera na Fila com Timeout de 30s (Robustez 1)
        if (xQueueReceive(fila_ssid, &ssid_recebido, pdMS_TO_TICKS(TIMEOUT_FILA_MS)) == pdTRUE) {
            
            bool encontrado = false;
            
            // Pede o Semáforo para acessar a lista (Requisito PDF)
            if (xSemaphoreTake(semaforo_lista_segura, pdMS_TO_TICKS(1000)) == pdTRUE) {
                
                // --- Seção Crítica (Protegida) ---
                for (int i = 0; i < 5; i++) {
                    if (strcmp(ssid_recebido, LISTA_SEGURA[i]) == 0) {
                        encontrado = true;
                        break;
                    }
                }
                // --- Fim da Seção Crítica ---
                
                xSemaphoreGive(semaforo_lista_segura); // Devolve o Semáforo
            
            } else {
                printf("{%s} ERRO: Nao conseguiu pegar o semaforo!\n", NOME_ALUNO);
            }

            // Atualiza o status global para a task_alerta
            if (encontrado) {
                g_status_rede = 0; // OK
                printf("{%s} Rede Segura: %s\n", NOME_ALUNO, ssid_recebido);
            } else {
                g_status_rede = 1; // ALERTA
                printf("{%s} REDE NÃO PERMITIDA/INSEGURA: %s\n", NOME_ALUNO, ssid_recebido);
            }

        } else {
            // Fila deu timeout (Scanner deve ter travado)
            printf("{%s} FALHA: Scanner nao envia dados ha %ds!\n", NOME_ALUNO, TIMEOUT_FILA_MS / 1000);
            g_status_rede = 2; // FALHA
        }

        esp_task_wdt_reset(); 
    }
}


// --- TAREFA 3: Alerta (Prioridade Alta) ---
// Controla o LED Vermelho (Pino 18)
void task_alerta(void *pvParameters) {
    esp_task_wdt_add(NULL); 
    
    for(;;) {
        // Lê o status global definido pelo Validador
        switch(g_status_rede) {
            case 0: // OK (Seguro)
                gpio_set_level(LED_VERMELHO, 0); // LED desligado
                vTaskDelay(pdMS_TO_TICKS(100)); 
                break;
            case 1: // ALERTA (Inseguro)
                // Pisca rápido
                gpio_set_level(LED_VERMELHO, 1);
                vTaskDelay(pdMS_TO_TICKS(TEMPO_ALERTA_MS / 2));
                gpio_set_level(LED_VERMELHO, 0);
                vTaskDelay(pdMS_TO_TICKS(TEMPO_ALERTA_MS / 2));
                break;
            case 2: // FALHA (Scanner Travou)
                // Pisca lento (S.O.S)
                gpio_set_level(LED_VERMELHO, 1);
                vTaskDelay(pdMS_TO_TICKS(1500));
                gpio_set_level(LED_VERMELHO, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
        }
        
        esp_task_wdt_reset(); 
    }
}


// --- Ponto de Entrada (app_main) ---
void app_main(void)
{
    // 1. Configura o LED (Equivalente ao pinMode)
    gpio_reset_pin(LED_VERMELHO);
    gpio_set_direction(LED_VERMELHO, GPIO_MODE_OUTPUT);
    
    // 2. Conecta ao Wi-Fi (Função complexa do ESP-IDF)
    setup_wifi(); 
    
    // 3. Configura WDT (Robustez 2)
    // Usamos 'init' pois este é o início do programa
    esp_task_wdt_config_t configWDT = {
        .timeout_ms = WDT_TIMEOUT_S * 1000, 
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true
    };
    esp_task_wdt_init(&configWDT);
    
    // 4. Cria Fila e Semáforo
    fila_ssid = xQueueCreate(10, sizeof(char[64]));
    semaforo_lista_segura = xSemaphoreCreateMutex();
    
    // 5. Cria as 3 Tarefas (Requisito do PDF)
    xTaskCreate(
        task_scanner,   
        "Scanner",      
        4096,           
        NULL,           
        1, // Prioridade Baixa
        NULL            
    );
    xTaskCreate(
        task_validador, "Validador", 4096, NULL, 2, NULL // Prioridade Média
    );
    xTaskCreate(
        task_alerta,    "Alerta",    2048, NULL, 3, NULL // Prioridade Alta
    );
    
    printf("\n>>> Sistema de Monitoramento de Wi-Fi Iniciado <<<\n");
    // A app_main termina aqui, mas as tarefas continuam rodando.
}
