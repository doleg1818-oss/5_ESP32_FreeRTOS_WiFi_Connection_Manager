#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/projdefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "secrets.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi_types_generic.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

typedef enum{
	WIFI_STATE_INIT = 0,
	WIFI_STATE_CONNECTING,
	WIFI_STATE_CONNECTED,
	WIFI_STATE_ONLINE,
	WIFI_STATE_DISCONNECTED	
}wifi_state_t;

static wifi_state_t wifi_state = WIFI_STATE_INIT;


#define WIFI_MANAGER_EVENT_START 				(1UL << 0)
#define WIFI_MANAGER_EVENT_CONNECTED 			(1UL << 1)
#define WIFI_MANAGER_EVENT_GOT_IP 				(1UL << 2)
#define WIFI_MANAGER_EVENT_DISCONNECTED 		(1UL << 3)
#define WIFI_MANAGER_EVENT_ALL (WIFI_MANAGER_EVENT_START | WIFI_MANAGER_EVENT_CONNECTED | WIFI_MANAGER_EVENT_GOT_IP | WIFI_MANAGER_EVENT_DISCONNECTED)

#define WIFI_RECONECT_DELAY_MS					2000U

static uint8_t last_disconnect_reason = 0;


static const char *TAG = "WIFI STA";
static const char *MANAGER_TAG = "WIFI MANAGER TASK";


static esp_netif_t *wifi_sta_netif = NULL; 

static bool wifi_manager_send_event(uint32_t event_bit);

static TaskHandle_t wifi_manager_task_handle = NULL;
#define WIFI_MANAGER_TASK_SIZE				4096
#define WIFI_MANAGER_TASK_PRIORITY			5
#define TEST_CORE							1


static const char *wifi_state_to_string(wifi_state_t state)
{
	switch(state)
	{
		case WIFI_STATE_INIT:
			return "INIT";
			
		case WIFI_STATE_CONNECTING:
			return "CONNECTING";
			
		case WIFI_STATE_CONNECTED:
			return "CONNECTED";
			
		case WIFI_STATE_ONLINE:
			return "ONLINE";
			
		case WIFI_STATE_DISCONNECTED:
			return "DISCONECTED";
		
		default:
			return "UNKNOWN";
	}
}

static void wifi_manager_set_state(wifi_state_t new_state)
{
	if(wifi_state == new_state)   // if no changing
	{
		return;
	}
	ESP_LOGI(MANAGER_TAG, "STATE: %s -> %s", wifi_state_to_string(wifi_state), wifi_state_to_string(new_state));
	wifi_state = new_state;
}

static bool wifi_manager_send_event(uint32_t event_bit)
{
	if(wifi_manager_task_handle == NULL)
	{
		ESP_LOGE(TAG, "Cannot send event: wifi_manager_task_handle == NULL");
		return false;
	}
	BaseType_t status = xTaskNotify(wifi_manager_task_handle, event_bit, eSetBits);
	return (status == pdPASS);
}

static void wifi_manager_start_connection(void)
{
	if(wifi_state == WIFI_STATE_CONNECTING)
	{
		ESP_LOGW(TAG, "Already connecting, skip request");
		return;
	}
	if(wifi_state == WIFI_STATE_ONLINE)
	{
		ESP_LOGW(TAG, "Already online, skip request");
		return;
	}
	wifi_manager_set_state(WIFI_STATE_CONNECTING);
	
	ESP_LOGI(TAG, "MANAGER: Starting WiFi connection ...");
	
	esp_err_t err = esp_wifi_connect(); 
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "MANAGER: reconnect FAILED err: %s", esp_err_to_name(err));
		wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
	}	
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	(void)arg;
	
	static const char *TAG = "WIFI_EVENT_HANDLER";
	
	if(event_base == WIFI_EVENT)
	{
		switch(event_id)
		{
			case WIFI_EVENT_STA_START:
			{
				ESP_LOGI(TAG, "EVENT: STA_STARTED");
				wifi_manager_send_event(WIFI_MANAGER_EVENT_START);
				
				break;
			}
			case WIFI_EVENT_STA_CONNECTED:
			{
				ESP_LOGI(TAG, "EVENT: STA_CONNECTED");
				wifi_manager_send_event(WIFI_MANAGER_EVENT_CONNECTED);
				
				break;
			}
					
			case WIFI_EVENT_STA_DISCONNECTED:
			{
				// Get reason of disconnect
				wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
				if(event != NULL)
				{
					last_disconnect_reason = event->reason;
				}
					
				ESP_LOGW(TAG, "EVENT: STA_CONNECTED reason :%u" , last_disconnect_reason);
				wifi_manager_send_event(WIFI_MANAGER_EVENT_DISCONNECTED);
				
				break;
			}
			default:
			{
				break;
			}
		}
		return;
	}
	
	if(event_base == IP_EVENT)
	{
		if(event_id == IP_EVENT_STA_GOT_IP)
		{
			ip_event_got_ip_t *event = (ip_event_got_ip_t*)event_data;
			
			if(event != NULL)
			{
				ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
				ESP_LOGI(TAG, "NETMASK: " IPSTR, IP2STR(&event->ip_info.netmask));
				ESP_LOGI(TAG, "GATEWAY: " IPSTR, IP2STR(&event->ip_info.gw));
			}
			ESP_LOGI(TAG, "EVENT: GOT IP");
			
			wifi_manager_send_event(WIFI_MANAGER_EVENT_GOT_IP);	
		}
	}
}



static void wifi_manager_task(void *parameters)
{
	(void)parameters;

	uint32_t received_events = 0;
	
	ESP_LOGI(TAG, "Manager task started");
	
	for(;;)
	{
		BaseType_t status = xTaskNotifyWait(0, WIFI_MANAGER_EVENT_ALL, &received_events, portMAX_DELAY);
		if(status != pdTRUE)
		{
			continue;
		}
		ESP_LOGI(TAG, "MANAGER received ivents: 0x%08" PRIX32, received_events);
		
		// START
		if(received_events & WIFI_MANAGER_EVENT_START)
		{
			ESP_LOGI(MANAGER_TAG, "MANAGER: start event");
			
			// Initial connection
			if((wifi_state == WIFI_STATE_INIT) || (wifi_state == WIFI_STATE_DISCONNECTED))
			{
				wifi_manager_start_connection();
			}
			else
			{
				ESP_LOGW(MANAGER_TAG, "START ignored in state=%s", wifi_state_to_string(wifi_state));
			}
		}
		
		// CONNECTED
		if(received_events & WIFI_MANAGER_EVENT_CONNECTED)
		{
			ESP_LOGI(MANAGER_TAG, "MANAGER: CONNECTED event");
			
			// ATA is associated with AP
			// But we don't have an IP yet
			if(wifi_state == WIFI_STATE_CONNECTING)	
			{
				wifi_manager_set_state(WIFI_STATE_CONNECTED);
			}		
			else
			{
				ESP_LOGW(MANAGER_TAG, "CONNECTED ignored in state=%s", wifi_state_to_string(wifi_state));
			}
		}
		
		// GOT IP
		if(received_events & WIFI_MANAGER_EVENT_GOT_IP)
		{
			ESP_LOGI(MANAGER_TAG, "MANAGER: GOT_IP event");
			
			// GOT_ID is vilid after wifi connection
			// 
			if(wifi_state == WIFI_STATE_CONNECTED) 
			{
				wifi_manager_set_state(WIFI_STATE_ONLINE);
				ESP_LOGI(MANAGER_TAG, "MANAGER: network is ONLINE");
			}
			else
			{
				ESP_LOGW(MANAGER_TAG, "GOT_IP ignored in state=%s", wifi_state_to_string(wifi_state));
			}
			/*if((wifi_state == WIFI_STATE_CONNECTED) || (wifi_state == WIFI_STATE_CONNECTING))
			{
				wifi_manager_set_state(WIFI_STATE_ONLINE);
				ESP_LOGI(MANAGER_TAG, "MANAGER: network is ONLINE");
			}
			else
			{
				ESP_LOGW(MANAGER_TAG, "GOT_IP ignored in state=%s", wifi_state_to_string(wifi_state));
			}*/
		}
		
		// DISCONNECTED
		if(received_events & WIFI_MANAGER_EVENT_DISCONNECTED)
		{
			ESP_LOGW(MANAGER_TAG, "MANAGER: disconnet, reason %u", last_disconnect_reason);
			
			wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
			
			// For example, reconect after 2 seconds
			ESP_LOGI(MANAGER_TAG, "MANAGER: reconnect after %u ms ....", WIFI_RECONECT_DELAY_MS);
			
			vTaskDelay(pdMS_TO_TICKS(WIFI_RECONECT_DELAY_MS));
			
			// Start new connection attempt
			wifi_manager_start_connection();
		}
	}
}


static esp_err_t wifi_init(void)
{
	static const char *TAG = "INIT WIFI";
	
	ESP_LOGI(TAG, "INIT WIFI");
	
	esp_err_t err;
	
	// TCP/IP networt interface infrastructure
	err = esp_netif_init();	
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
		return err;
	}
	
	// Create default init loop
	err = esp_event_loop_create_default();	
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
		return err;
	}
	
	// Create STA network interfaace
	wifi_sta_netif = esp_netif_create_default_wifi_sta(); 
	if(wifi_sta_netif == NULL)
	{
		ESP_LOGE(TAG, "Failed to esp_netif_create_default_wifi_sta");
		return ESP_FAIL;
	}

	// Initialize WiFi
	wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
	err = esp_wifi_init(&wifi_init_config);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to WIFI_INIT_CONFIG_DEFAULT err: %s", esp_err_to_name(err));
		return err;
	}
	
	// Register event handler
	err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to esp_event_handler_instance_register err: %s", esp_err_to_name(err));
		return err;
	}
	
	err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed to esp_event_handler_instance_register err: %s", esp_err_to_name(err));
		return err;
	}
	
	// Configure STA mode
	wifi_config_t wifi_config = {0};
	
	strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
	strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);
	
	// Use normal station mode
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	
	err = esp_wifi_set_mode(WIFI_MODE_STA);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_set_mode failed. err: %s", esp_err_to_name(err));
	}
	
	
	err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_set_config failed. err: %s", esp_err_to_name(err));
	}
	
	
	// Create manager task
	esp_err_t status = xTaskCreatePinnedToCore(wifi_manager_task, "wifi_manager_task", WIFI_MANAGER_TASK_SIZE, NULL, WIFI_MANAGER_TASK_PRIORITY, &wifi_manager_task_handle, TEST_CORE);
	if(status != pdPASS)
	{
		ESP_LOGE(TAG, "Failed create wifi_manager_task");
		return ESP_FAIL;
	}
	
	
	// Start wifi
	err = esp_wifi_start();
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "Failed create esp_wifi_start err:%s", esp_err_to_name(err));
		return err;
	}
	
	ESP_LOGI(TAG, "WiFi STA initialized");
	
	return ESP_OK;
}

void app_main(void)
{
	ESP_LOGI(TAG, "Aplication started");
	
	// Init NVS
	esp_err_t err = nvs_flash_init();
	if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ESP_ERROR_CHECK(nvs_flash_init());
	}
	
	// Initialize WiFi
	err = wifi_init();
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "FAILED INIT WiFi !!! ");
	}
	ESP_LOGI(TAG, "WiFi STA initialized complited");
	
	
	
	
	
	
	
	

}
