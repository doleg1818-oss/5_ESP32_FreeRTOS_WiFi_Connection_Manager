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
#include "esp_wifi_types.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"

#include "esp_random.h"
#include "esp_mac.h"


static const char *TAG = "WIFI STA";
static const char *MANAGER_TAG = "WIFI MANAGER TASK";

typedef enum{
	WIFI_STATE_INIT = 0,
	WIFI_STATE_CONNECTING,
	WIFI_STATE_SELECTING_AP,
	WIFI_STATE_CONNECTED,
	WIFI_STATE_ONLINE,
	WIFI_STATE_DISCONNECTED	
}wifi_state_t;

static wifi_state_t wifi_state = WIFI_STATE_INIT;


#define WIFI_MANAGER_EVENT_START 				(1UL << 0)
#define WIFI_MANAGER_EVENT_CONNECTED 			(1UL << 1)
#define WIFI_MANAGER_EVENT_GOT_IP 				(1UL << 2)
#define WIFI_MANAGER_EVENT_DISCONNECTED 		(1UL << 3)
#define WIFI_MANAGER_EVENT_SCAN_DONE			(1UL << 4)
#define WIFI_MANAGER_EVENT_SCAN_FAILED			(1UL << 5)
#define WIFI_MANAGER_EVENT_ALL (WIFI_MANAGER_EVENT_START | WIFI_MANAGER_EVENT_CONNECTED | WIFI_MANAGER_EVENT_GOT_IP | WIFI_MANAGER_EVENT_DISCONNECTED)

uint32_t WIFI_RECONECT_DELAY_MS = 2000U;
uint32_t MAX_RECONNEKT_ATTEMPTS = 15U;
uint32_t WIFI_MAX_RECONNECT_DELAY_MS = 30000U;

static wifi_err_reason_t last_disconnect_reason = WIFI_REASON_UNSPECIFIED;
static uint32_t reconnect_attempts = 0;

static esp_netif_t *wifi_sta_netif = NULL; 
wifi_ap_record_t selected_ap;
bool selected_ap_valid = false;


static void wifi_manager_set_state(wifi_state_t new_state);
static bool wifi_manager_send_event(uint32_t event_bit);





static esp_err_t wifi_manager_select_beast_ap(void);



static TaskHandle_t wifi_manager_task_handle = NULL;
#define WIFI_MANAGER_TASK_SIZE				4096
#define WIFI_MANAGER_TASK_PRIORITY			5
#define TEST_CORE							1

static esp_err_t wifi_manager_connect_selected_ap(void)
{
	if(selected_ap_valid == false)
	{
		ESP_LOGE(MANAGER_TAG, "AP not found");
		return ESP_FAIL;
	}
	
	wifi_config_t wifi_config = {0};
	
	strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid)-1);
	strncpy((char*)wifi_config.sta.password, WIFI_PASSWORD, sizeof(wifi_config.sta.password)-1);
	
	// Connect only to AP selected by scan algorithms
	wifi_config.sta.bssid_set = true;
	memcpy(wifi_config.sta.bssid, selected_ap.bssid, sizeof(wifi_config.sta.bssid));
	
	// Start searching from the chanel where selected AP was found
	wifi_config.sta.channel = selected_ap.primary;
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
	
	ESP_LOGI(MANAGER_TAG, 
		"Configure selected AP: SSID=%s, RSSI=%d, CH=%u BSSID= " MACSTR,
		selected_ap.ssid,
		selected_ap.rssi,
		selected_ap.primary,
		MAC2STR(selected_ap.bssid));
	
	esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
	if(err != ESP_OK)
	{
		ESP_LOGE(MANAGER_TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
		return err;
	}
	
	reconnect_attempts++;
	wifi_manager_set_state(WIFI_STATE_CONNECTING);
	
	ESP_LOGI(MANAGER_TAG, "Connecting to selected AP, attempt=%" PRIu32, reconnect_attempts);
	
	err= esp_wifi_connect();
	if(err != ESP_OK)
	{
		ESP_LOGE(MANAGER_TAG, "Failed to connect to AP. err:%s", esp_err_to_name(err));
		wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
		return err;
	}
	return ESP_OK;
}

static bool wifi_scan_and_select_ap(wifi_ap_record_t *selected_ap)
{
	static const char *TAG = "WIFI SCAN";
	
	ESP_LOGI(TAG, "Start WiFi scan...");
	
	wifi_scan_config_t scan_config = {
		.ssid = NULL,
		.bssid = NULL,
		.channel = 0,
		.show_hidden = true,
		.scan_type = WIFI_SCAN_TYPE_ACTIVE,
		.scan_time.active.min = 100,
		.scan_time.active.max = 300,
	};
	
	esp_err_t err = esp_wifi_scan_start(&scan_config, true);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
		return false;
	}
	
	uint16_t ap_count = 0;
	err = esp_wifi_scan_get_ap_num(&ap_count);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(err));
		return false;
	}

	if(ap_count > 0)
	{
		ESP_LOGI(TAG, "Scan completed. Found APs: %u ", ap_count);	
	}
	else
	{
		ESP_LOGW(TAG, "No WiFi networks found");
	}
	
	wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
	if(ap_records == NULL)
	{
		ESP_LOGE(TAG, "Failed to allocate memory");
		return false;
	}
	
	uint16_t records_to_get = ap_count;
	err = esp_wifi_scan_get_ap_records(&records_to_get, ap_records);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records Failed");
		return false;
	}
	
	// Show all faunded AP
	for(uint16_t i = 0; i < records_to_get; i++)
	{
		ESP_LOGI(TAG, "[%u] SSID: %-32s | RSSI: %" PRId8" | CH: %u | BSSID: " MACSTR, 
			i,
			(char *)ap_records[i].ssid,
			ap_records[i].rssi,
			ap_records[i].primary,
			MAC2STR(ap_records[i].bssid)
		);
	}
	
	bool found = false;
	// Found target AP
	for(uint16_t i = 0; i < records_to_get; i++)
	{
		if(ap_records[i].ssid[0] == '\0')		// Idnore AP witn hiden SSID
		{
			continue;
		}
		
		if(strcmp((char *)ap_records[i].ssid, WIFI_SSID) != 0)		// Find target SSID
		{
			continue;
		}
		
		if(found == false)
		{
			*selected_ap = ap_records[i];
			found = true;
			continue;
		}
		// If the same PAs have same SSID, select none witth the strongest RSSI
		if(ap_records[i].rssi > selected_ap->rssi)
		{
			*selected_ap = ap_records[i];
		}	
	}
	
	free(ap_records);
	
	if(found == false)
	{
		ESP_LOGW(TAG, "Target AP SSID \"%s\" not found", WIFI_SSID);
		return false;
	}
	
	ESP_LOGI(TAG, "Selected AP:");
	ESP_LOGI(TAG, " SSID : %s", selected_ap->ssid);
	ESP_LOGI(TAG, " RSSI : %d dBm", selected_ap->rssi);
	ESP_LOGI(TAG, " CH : %u", selected_ap->primary);
	ESP_LOGI(TAG, " BSSID: " MACSTR, MAC2STR(selected_ap->bssid));
	
	return true;	
}


static esp_err_t wifi_manager_scan(void)
{
	static const char *TAG = "WIFI SCAN";
	
	ESP_LOGI(TAG, "Start WiFi scan...");
	
	wifi_scan_config_t scan_config = {
		.ssid = NULL,
		.bssid = NULL,
		.channel = 0,
		.show_hidden = true,
		.scan_type = WIFI_SCAN_TYPE_ACTIVE,
		.scan_time.active.min = 100,
		.scan_time.active.max = 300,
	};
	
	esp_err_t err = esp_wifi_scan_start(&scan_config, true);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
		return err;
	}
	
	uint16_t ap_count = 0;
	err = esp_wifi_scan_get_ap_num(&ap_count);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(err));
		return err;
	}

	if(ap_count > 0)
	{
		ESP_LOGI(TAG, "Scan completed. Found APs: %u ", ap_count);	
	}
	else
	{
		ESP_LOGW(TAG, "No WiFi networks found");
	}
	
	wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
	if(ap_records == NULL)
	{
		ESP_LOGE(TAG, "Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	
	uint16_t records_to_get = ap_count;
	err = esp_wifi_scan_get_ap_records(&records_to_get, ap_records);
	if(ap_records == NULL)
	{
		ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records Failed");
		return ESP_ERR_NO_MEM;
	}
	
	// Show all faunded AP
	for(uint16_t i = 0; i < records_to_get; i++)
	{
		ESP_LOGI(TAG, "[%u] SSID: %-32s | RSSI: %" PRId8" | CH: %u | BSSID: " MACSTR, 
			i,
			(char *)ap_records[i].ssid,
			ap_records[i].rssi,
			ap_records[i].primary,
			MAC2STR(ap_records[i].bssid)
		);
	}

	free(ap_records);
	return true;	
}

static uint32_t wifi_get_jitter(uint32_t max_jitter)
{
	return esp_random()%(max_jitter + 1);
}

static uint32_t wifi_get_backoff_delay(uint32_t attempt)
{
	uint32_t delay = WIFI_RECONECT_DELAY_MS;
	for(uint32_t i = 1; i < attempt; i++)
	{
		if(delay >= WIFI_MAX_RECONNECT_DELAY_MS/2)
		{
			delay = WIFI_MAX_RECONNECT_DELAY_MS;
			break;
		}
		delay = delay*2;
	}
	if(delay > WIFI_MAX_RECONNECT_DELAY_MS)
	{
		delay = WIFI_MAX_RECONNECT_DELAY_MS;
	}
	
	return delay;
}
static uint32_t wifi_get_retry_delay(uint32_t attempt)
{
	uint32_t backoff = wifi_get_backoff_delay(attempt);
	uint32_t jitter = wifi_get_jitter(500);
	
	//ESP_LOGI("TEST RANDOM ","backoff :%" PRIu32 " ms" "jitter :%" PRIu32 "ms" , backoff, jitter);
	
	return backoff + jitter;
}

static const char *wifi_disconnect_reason_to_string(wifi_err_reason_t reason)
{
	switch(reason)
	{
		case WIFI_REASON_AUTH_EXPIRE:	
			return "AUTH_EXPIRE";
			
		case WIFI_REASON_AUTH_LEAVE:	
			return "AUTH_LEAVE";
			
		case WIFI_REASON_ASSOC_EXPIRE:	
			return "ASSOC_EXPIRE";			
			
		case WIFI_REASON_ASSOC_TOOMANY:	
			return "ASSOC_TOOMANY";			
			
		case WIFI_REASON_NOT_AUTHED:	
			return "NOT_AUTHED";			
			
		case WIFI_REASON_NOT_ASSOCED:	
			return "NOT_ASSOCED";
			
		case WIFI_REASON_ASSOC_LEAVE:	
			return "ASSOC_LEAVE";
			
		case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:	
			return "4WAY_HANDSHAKE_TIMEOUT";
			
		case WIFI_REASON_BEACON_TIMEOUT:	
			return "BEACON_TIMEOUT";
			
		case WIFI_REASON_NO_AP_FOUND:	
			return "NO_AP_FOUND";			
		
		default:
			return "UNKNOWN";
	}
}

static const char *wifi_state_to_string(wifi_state_t state)
{
	switch(state)
	{
		case WIFI_STATE_INIT:
			return "INIT";
			
		case WIFI_STATE_CONNECTING:
			return "CONNECTING";
			
		case WIFI_STATE_SELECTING_AP:
			return "SELECTING_AP";
			
		case WIFI_STATE_CONNECTED:
			return "CONNECTED";
			
		case WIFI_STATE_ONLINE:
			return "ONLINE";
			
		case WIFI_STATE_DISCONNECTED:
			return "DISCONNECTED";
		
		default:
			return "UNKNOWN";
	}
}

static bool wifi_should_reconnect(wifi_err_reason_t reason)
{
	switch(reason)	
	{
		case WIFI_REASON_BEACON_TIMEOUT:
		case WIFI_REASON_NO_AP_FOUND:
		case WIFI_REASON_AUTH_EXPIRE:
		case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
			return true;
			
		default:
			return true;
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
	
	reconnect_attempts++;
	ESP_LOGI(TAG, "MANAGER: Startint WiFi connection, attempt :%lu", (unsigned long)reconnect_attempts);
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
					
					ESP_LOGW(TAG, "STA_DISCONNECTED: reason=%d (%s)",
					last_disconnect_reason,
					wifi_disconnect_reason_to_string(last_disconnect_reason));
				}
					
				ESP_LOGW(TAG, "EVENT: STA_DISCONNECTED reason :%u" , last_disconnect_reason);
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
			if((wifi_state == WIFI_STATE_INIT) || (wifi_state == WIFI_STATE_DISCONNECTED))
			{
				ESP_LOGI(MANAGER_TAG, "MANAGER: start event");
				
				wifi_manager_set_state(WIFI_STATE_SELECTING_AP);
				
				selected_ap_valid = false;
			
				if(wifi_scan_and_select_ap(&selected_ap) == true)	// Fount target AP
				{
					selected_ap_valid = true;
					ESP_LOGI(TAG, "AP selection successful");
					
					esp_err_t err = wifi_manager_connect_selected_ap();
					if(err != ESP_OK)
					{
						ESP_LOGE(MANAGER_TAG, "Connected to selected AP failed. err: %s", esp_err_to_name(err));
						wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
					}
				}
				else
				{
					// selected_ap_valid = false;
					ESP_LOGW(TAG, "Selection AP failed");
				}
			}
			else
			{
				ESP_LOGW(TAG, "Start ignored in state =%s", wifi_state_to_string(wifi_state));
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
				reconnect_attempts = 0;
			}
			else
			{
				ESP_LOGW(MANAGER_TAG, "GOT_IP ignored in state=%s", wifi_state_to_string(wifi_state));
			}
		}
		
		// DISCONNECTED
		if(received_events & WIFI_MANAGER_EVENT_DISCONNECTED)
		{
			ESP_LOGW(MANAGER_TAG, "MANAGER: disconnet, reason %u (%s)", last_disconnect_reason, wifi_disconnect_reason_to_string(last_disconnect_reason));
			
			wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
			
			// Cteate delay depent of attempt. 2,4,8,16,30, 30, 30 seconda + random part from 0 to 500 mSec
			uint32_t delay = wifi_get_retry_delay(reconnect_attempts);
			
			// For example, reconect after 2 seconds
			ESP_LOGI(MANAGER_TAG, "MANAGER: reconnect after %" PRIu32 " ms ....", delay);
			
			if(wifi_should_reconnect(last_disconnect_reason))
			{
				if(reconnect_attempts >= MAX_RECONNEKT_ATTEMPTS)
				{
					ESP_LOGW(MANAGER_TAG, "Maximum reconnect attempts reached :%" PRIu32 " from %" PRIu32, 
					reconnect_attempts,
					MAX_RECONNEKT_ATTEMPTS);
					continue;
				}
				
				ESP_LOGW(MANAGER_TAG, "Reconnect sheduled in %" PRIu32 " ms, next attempt=%" PRIu32 "/%" PRIu32, 
				delay, 
				reconnect_attempts+1,
				MAX_RECONNEKT_ATTEMPTS);
				
				vTaskDelay(pdMS_TO_TICKS(delay));
				// Start new connection attempt
				wifi_manager_start_connection();
			}
			else
			{
				ESP_LOGW(MANAGER_TAG, "Reconnect disabled for reason=%u (%s)", last_disconnect_reason, wifi_disconnect_reason_to_string(last_disconnect_reason));
			}
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
	
	err = esp_wifi_set_mode(WIFI_MODE_STA);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_set_mode failed. err: %s", esp_err_to_name(err));
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
