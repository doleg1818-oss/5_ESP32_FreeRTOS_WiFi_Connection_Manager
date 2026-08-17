#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>


#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "portmacro.h"
#include "secrets.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

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

#include "wifi_manager.h"

#include "esp_heap_caps.h"

static const char *TAG = "WIFI STA";
static const char *MANAGER_TAG = "WIFI MANAGER TASK";


typedef enum {
	// Internal events from ESP_IDF
	WIFI_MANAGER_MSG_STA_START = 0,
	WIFI_MANAGER_MSG_CONNECTED,
	WIFI_MANAGER_MSG_GOT_IP,
	WIFI_MANAGER_MSG_DISCONNECTED,
	WIFI_MANAGER_MSG_SCAN_DONE,
	
	// External commands
	WIFI_MANAGER_CMD_CONNECT,
	WIFI_MANAGER_CMD_DISCONNECT,
	WIFI_MANAGER_CMD_SCAN,
	WIFI_MANAGER_CMD_RECONNECT
}wifi_manager_message_id_t;

typedef struct{
	wifi_manager_message_id_t id;
	wifi_err_reason_t disconnect_reason;
}wifi_manager_message_t;


static wifi_state_t wifi_state = WIFI_STATE_INIT;


uint32_t WIFI_RECONECT_DELAY_MS = 2000U;
uint32_t MAX_RECONNEKT_ATTEMPTS = 15U;
uint32_t WIFI_MAX_RECONNECT_DELAY_MS = 30000U;

static uint32_t reconnect_attempts = 1;

static esp_netif_t *wifi_sta_netif = NULL; 
wifi_ap_record_t selected_ap;

bool selected_ap_valid = false;
static bool auto_reconnect_anabled = true; 			

typedef enum{
	WIFI_SCAN_PURPOSE_NONE = 0,
	WIFI_SCAN_PURPOSE_CONNECT,
	WIFI_SCAN_PURPOSE_USER
} wifi_scan_purpose_t;

static wifi_scan_purpose_t wifi_scan_purpose =  WIFI_SCAN_PURPOSE_NONE;

static void wifi_manager_set_state(wifi_state_t new_state);
static bool wifi_manager_send_message(wifi_manager_message_t *message);

static TimerHandle_t wifi_reconnect_timmer = NULL;


static TaskHandle_t wifi_manager_task_handle = NULL;
#define WIFI_MANAGER_TASK_SIZE				4096
#define WIFI_MANAGER_TASK_PRIORITY			5
#define TEST_CORE							1

static QueueHandle_t wifi_manager_queue = NULL;
#define WIFI_MANAGER_QUEUE_LENGTH 			10


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
			
		case WIFI_REASON_TIMEOUT:
			return "TIMEOUT";
		
		case WIFI_REASON_CONNECTION_FAIL:
			return "CONNECTION_FAIL";
		
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


// Сканує AP і в залежності від purpose просто виводить список просканованих AP або вибирає TARGET AP мережу
// і записує її в глобальну змінну selected_ap
static esp_err_t wifi_manager_process_scan_results(wifi_scan_purpose_t purpose)
{
	static const char *TAG = "WIFI SCAN";
	uint16_t ap_count = 0;
	
	esp_err_t err = esp_wifi_scan_get_ap_num(&ap_count);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(err));
		return err;
	}
	ESP_LOGI(TAG, "Scan complited. FOund %d", ap_count);
	
	if(ap_count == 0)
	{
		return ESP_FAIL;
	}
	
	wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
	if(ap_records == NULL)
	{
		ESP_LOGE(TAG, "Failed to allocate memory");
		return ESP_ERR_NO_MEM;
	}
	
	uint16_t record_to_ger = ap_count;
	err = esp_wifi_scan_get_ap_records(&record_to_ger, ap_records);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records Failed: %s", esp_err_to_name(err));
		free(ap_records);
		return err;
	}
	
	// Print all
	for(uint16_t i = 0; i < record_to_ger; i++)
	{
		ESP_LOGI(TAG, 
			"[%u] SSID: %-32s, | RSSI: %" PRId8
			" | CH: %u | BSSID: " MACSTR,
			i,
			(char*)ap_records[i].ssid,
			ap_records[i].rssi,
			ap_records[i].primary,
			MAC2STR(ap_records[i].bssid));
	}	
	
	// Якщо команда на сканування була від user тоді не підключатись на знайденої AP
	if(purpose == WIFI_SCAN_PURPOSE_USER)		
	{
		free(ap_records);
		return ESP_OK;	
	}
	
	// Якщо команда була від Event handle тоді підключитися до потрібної AP
	bool found = false;
	for(uint16_t i = 0; i < record_to_ger; i++)
	{
		if(ap_records[i].ssid[0] == '\0')
		{
			continue;
		}
		if(strcmp((char*)ap_records[i].ssid, WIFI_SSID) != 0)  // Found AP SSID != target AP
		{
			continue;
		}
		
		if(found == false)	
		{
			selected_ap = ap_records[i];
			found = true;
			continue;
		}
		
		// Записати з найкращим сигналом
		if(ap_records[i].rssi > selected_ap.rssi)
		{
			selected_ap = ap_records[i];
		}
	}
	free(ap_records);
	
	if(found == false)
	{
		ESP_LOGW(TAG, "Target AP \"%s\" not found", WIFI_SSID);
		return ESP_FAIL;
	}
	
	selected_ap_valid = true;
	
	ESP_LOGI(TAG, "Selected AP:");
	ESP_LOGI(TAG, " SSID ; %s", (char*)selected_ap.ssid);
	ESP_LOGI(TAG, " RSSI : %d dBm", selected_ap.rssi);
	ESP_LOGI(TAG, " CH : %u", selected_ap.primary);
	ESP_LOGI(TAG, " BSSID : " MACSTR, MAC2STR(selected_ap.bssid));
	
	return ESP_OK;
}

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

wifi_state_t wifi_manager_get_state(void)
{
	return wifi_state;
}

bool wifi_manager_is_online(void)
{
	return wifi_state == WIFI_STATE_ONLINE;
}

bool wifi_manager_scan(void)
{
	wifi_manager_message_t message = {
		.id = WIFI_MANAGER_CMD_SCAN
	};
	return wifi_manager_send_message(&message);
}

// Scan APs without block mode(When scan will done WIFI_EVENT_SCAN_DONE event wil be genereted in wifi_event_handler)
static esp_err_t wifi_manager_start_scan(wifi_scan_purpose_t purpose)   
{
	if(purpose == WIFI_SCAN_PURPOSE_NONE)		//  Запустити скан з реальною причиною
	{
		return ESP_ERR_INVALID_ARG;	
	}
	// wifi_scan_purpose - глобальний поточний стан сканування
	if(wifi_scan_purpose != WIFI_SCAN_PURPOSE_NONE)		// чи не виконується зараз скан
	{
		ESP_LOGW(MANAGER_TAG, "Scan already in progress");
		return ESP_ERR_INVALID_STATE;
	}
	
	wifi_scan_config_t scan_config = {
		.ssid = NULL,
		.bssid = NULL,
		.channel = 0,
		.show_hidden = true,
		.scan_type = WIFI_SCAN_TYPE_ACTIVE,
		.scan_time.active.min = 100,
		.scan_time.active.max = 300
	};
	
	wifi_scan_purpose = purpose;
	
	ESP_LOGI(MANAGER_TAG, "Starting asinchronous WiFi scan");
	esp_err_t err = esp_wifi_scan_start(&scan_config, false);
	if(err != ESP_OK)
	{
		ESP_LOGE(MANAGER_TAG, "esp_wifi_scan_start failed %s", esp_err_to_name(err));
		wifi_scan_purpose = WIFI_SCAN_PURPOSE_NONE;
		return err;
	}
	return ESP_OK;
}

static bool wifi_manager_shedule_reconnect(uint32_t delay_ms)
{
	if(wifi_reconnect_timmer == NULL)
	{
		ESP_LOGE(TAG, "wifi_reconnect_timmer == NULL");
		return false;
	}
	
	TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
	if(delay_ticks == 0)
	{
		delay_ticks = 1;
	}
	
	BaseType_t status = xTimerChangePeriod(wifi_reconnect_timmer, delay_ticks, 0 );
	if(status != pdPASS)
	{
		ESP_LOGE(TAG, "xTimerChangePeriod failed");
		return false;
	}
	return true;
}

static void wifi_reconnect_timer_callback(TimerHandle_t timer)
{
	(void)timer;
	
	wifi_manager_message_t message = {
		.id = WIFI_MANAGER_CMD_RECONNECT
	};
	
	if(wifi_manager_send_message(&message) == false)
	{
		ESP_LOGW(MANAGER_TAG, "Failed send rRECONNECT command from timmer");
	}
}

bool wifi_manager_disconnect(void)
{
	wifi_manager_message_t message = {
		.id = WIFI_MANAGER_CMD_DISCONNECT
	};
	return wifi_manager_send_message(&message);
}

bool wifi_manager_connect(void)
{
	wifi_manager_message_t message = {
		.id = WIFI_MANAGER_CMD_CONNECT
	};
	return wifi_manager_send_message(&message);
}
static bool wifi_manager_start_connect_sequance(void)
{
	// якщо wifi_state не WIFI_STATE_INIT і не WIFI_STATE_DISCONNECTED
	if((wifi_state != WIFI_STATE_INIT) && (wifi_state != WIFI_STATE_DISCONNECTED))
	{
		ESP_LOGW(MANAGER_TAG, "Connect sequence ignored in stste %s",  wifi_state_to_string(wifi_state));	
		return false;
	}
						
	wifi_manager_set_state(WIFI_STATE_SELECTING_AP);
						
	selected_ap_valid = false;
	
	// Scan networks and select target AP	
	esp_err_t err = wifi_manager_start_scan(WIFI_SCAN_PURPOSE_CONNECT);
	if(err != ESP_OK)
	{
		ESP_LOGE(MANAGER_TAG, "Connected to selected AP failed. err: %s", esp_err_to_name(err));
		wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
		return false;
	}
	return true;
}

static bool wifi_manager_send_message(wifi_manager_message_t *message)
{
	if(message == NULL)
	{
		return false;
	}
	
	if(wifi_manager_queue == NULL)
	{
		ESP_LOGE(MANAGER_TAG, "WiFi manager queue is NULL");
		return false;
	}
	
	BaseType_t status = xQueueSend(wifi_manager_queue, message, 0);   	
	if(status != pdPASS)
	{
		ESP_LOGW(MANAGER_TAG, "Failed to send message to wifi manager");
		return false;
	}
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
				
				wifi_manager_message_t message = {
					.id = WIFI_MANAGER_MSG_STA_START
				};
				wifi_manager_send_message(&message);
				break;
			}
			case WIFI_EVENT_STA_CONNECTED:
			{
				ESP_LOGI(TAG, "EVENT: STA_CONNECTED");
				
				wifi_manager_message_t message = {
					.id = WIFI_MANAGER_MSG_CONNECTED
				};
				wifi_manager_send_message(&message);
				
				break;
			}
					
			case WIFI_EVENT_STA_DISCONNECTED:
			{
				ESP_LOGI(TAG, "EVENT: WIFI_EVENT_STA_DISCONNECTED");
				// Get reason of disconnect
				wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
				
				wifi_manager_message_t message = {
					.id = WIFI_MANAGER_MSG_DISCONNECTED,
					.disconnect_reason = WIFI_REASON_UNSPECIFIED
				};
				
				if(event != NULL)
				{
					message.disconnect_reason = event->reason;
					
					ESP_LOGW(TAG, "STA_DISCONNECTED: reason=%d (%s)",
						event->reason,
						wifi_disconnect_reason_to_string(event->reason));
				}
					
				ESP_LOGW(TAG, "EVENT: STA_DISCONNECTED reason :%u" , event->reason);
				
				wifi_manager_send_message(&message);
				break;
			}
			
			case WIFI_EVENT_SCAN_DONE:
				ESP_LOGI(TAG, "EVENT: WIFI_EVENT_SCAN_DONE");
				
				wifi_manager_message_t massege = {
					.id = WIFI_MANAGER_MSG_SCAN_DONE
				};
				wifi_manager_send_message(&massege);
				break;
			
			
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
			
			wifi_manager_message_t message = {
				.id = WIFI_MANAGER_MSG_GOT_IP,
				};
			wifi_manager_send_message(&message);
		}
	}
}



static void wifi_manager_task(void *parameters)
{
	(void)parameters;

	wifi_manager_message_t message;
	
	wifi_err_reason_t last_disconnect_reason = 0;
 
	
	ESP_LOGI(TAG, "Manager task started");
	
	for(;;)
	{ 
		BaseType_t status = xQueueReceive(wifi_manager_queue ,&message, portMAX_DELAY);
		if(status != pdPASS)
		{
			continue;
		} 
		 
		switch(message.id)
		{
			// COMMANDS FROM wifi_event_handler 
			
			case WIFI_MANAGER_MSG_STA_START:
			{
				ESP_LOGI(MANAGER_TAG, "MANAGER: start event");
				
				if((wifi_state == WIFI_STATE_INIT) || (wifi_state == WIFI_STATE_DISCONNECTED))
				{
					auto_reconnect_anabled = true;
					reconnect_attempts = 0;
					
					wifi_manager_start_connect_sequance();
				}
				else
				{
					// selected_ap_valid = false;
					ESP_LOGW(TAG, "STA_Start ignoreg in state %s", wifi_state_to_string(wifi_state));
				}
				break;	
			}
			
			case WIFI_MANAGER_MSG_CONNECTED:
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
				break;
			}

				
			case WIFI_MANAGER_MSG_DISCONNECTED:
			{
				last_disconnect_reason = message.disconnect_reason;
				ESP_LOGW(MANAGER_TAG, "MANAGER: disconnet, reason %u (%s)", last_disconnect_reason, wifi_disconnect_reason_to_string(last_disconnect_reason));
			
				wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
				
				// Cteate delay depent of attempt. 2,4,8,16,30, 30, 30 seconda + random part from 0 to 500 mSec
				uint32_t delay = wifi_get_retry_delay(reconnect_attempts);
				
				if(auto_reconnect_anabled == false)
				{
					ESP_LOGI(MANAGER_TAG, "Auto reconnect disabled. Stay disconnect.");
					break;
				}
				
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
					
					if(wifi_manager_shedule_reconnect(delay) == false)
					{
						ESP_LOGE(MANAGER_TAG, "Failed to shedule reconnect");
					}
				}
				else
				{
					ESP_LOGW(MANAGER_TAG, "Reconnect disabled for reason=%u (%s)", last_disconnect_reason, wifi_disconnect_reason_to_string(last_disconnect_reason));
				}
				break;
			}
			
			case WIFI_MANAGER_MSG_GOT_IP:
			{
				ESP_LOGI(MANAGER_TAG, "MANAGER: GOT_IP event");
			
				// GOT_ID is vilid after wifi connection
				if(wifi_state == WIFI_STATE_CONNECTED) 
				{
					wifi_manager_set_state(WIFI_STATE_ONLINE);
					ESP_LOGI(MANAGER_TAG, "MANAGER: network is ONLINE");
					reconnect_attempts = 1;
				}
				else
				{
					ESP_LOGW(MANAGER_TAG, "GOT_IP ignored in state=%s", wifi_state_to_string(wifi_state));
				}
				break;
			}
			
			case WIFI_MANAGER_MSG_SCAN_DONE:  
			{
				ESP_LOGI(MANAGER_TAG, "MANAGER: WIFI_MANAGER_MSG_SCAN_DONE event");
				
				// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
				
				wifi_scan_purpose_t complected_scan = wifi_scan_purpose;
				wifi_scan_purpose = WIFI_SCAN_PURPOSE_NONE;
				
				if(complected_scan == WIFI_SCAN_PURPOSE_NONE)
				{
					ESP_LOGW(MANAGER_TAG, "Unexpectec SCAN_DONE ignored");
					break;
				}
				
				esp_err_t err = wifi_manager_process_scan_results(complected_scan);
				
				// User requested scan		
				if(complected_scan == WIFI_SCAN_PURPOSE_USER)
				{
					ESP_LOGW(MANAGER_TAG, "Don't connect, only print AP list");
					if(err != ESP_OK)
					{
						ESP_LOGE(MANAGER_TAG, "User scan processing failed");
					}
					break;
				}
				
				// Scan was part of CONNECT sequence
				if(complected_scan == WIFI_SCAN_PURPOSE_CONNECT)
				{		
					if(err != ESP_OK)
					{
						ESP_LOGW(MANAGER_TAG, "Target AP not found");
						
						wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
						break;
					}
					
					if(auto_reconnect_anabled == false)
					{
						ESP_LOGI(MANAGER_TAG, "Connect aequence canselled");
						selected_ap_valid = false;
						wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
						break;
					}
					err = wifi_manager_connect_selected_ap();
					if(err != ESP_OK)
					{
						ESP_LOGE(MANAGER_TAG, "Connect selected AP failed");
						wifi_manager_set_state(WIFI_STATE_DISCONNECTED);
					}
				}
				break;	
			}
			




			// COMMANDS FROM Outside //////////////////////////////////////////////////////////////////////////
			case WIFI_MANAGER_CMD_CONNECT:
			{
				ESP_LOGI(MANAGER_TAG, "WIFI_MANAGER_SMD_CONNECT command received");

				// Підключати тільки тоді коли стан WIFI_STATE_DISCONNECTED
				if(wifi_state != WIFI_STATE_DISCONNECTED)
				{
					ESP_LOGW(MANAGER_TAG, "Connect ignored: in state %s", wifi_state_to_string(wifi_state));
					break;
				}
				
				// Manual connect endbles automatic recowert again
				auto_reconnect_anabled = true;
				reconnect_attempts = 1;
				
				// Cansel pending automatic reconnect
				if(wifi_reconnect_timmer != NULL)
				{
					xTimerStop(wifi_reconnect_timmer, 0);
				}
				wifi_manager_start_connect_sequance();
				break;	
			}
				
				
			case WIFI_MANAGER_CMD_DISCONNECT:
			{
				ESP_LOGI(MANAGER_TAG, "WIFI_MANAGER_SMD_DISCONNECT command received");
			
				auto_reconnect_anabled = false;
					
				// Cansel pending reconnect if one exists
				if(wifi_reconnect_timmer != NULL)
				{
					xTimerStop(wifi_reconnect_timmer, 0);
				}
				
				if(wifi_state == WIFI_STATE_SELECTING_AP)
				{
					ESP_LOGI(MANAGER_TAG, "Disconnection cansel request during AP scan");
					break;
				}
				
				if((wifi_state == WIFI_STATE_CONNECTING) || (wifi_state == WIFI_STATE_CONNECTED) ||(wifi_state == WIFI_STATE_ONLINE))
				{
					esp_err_t err = esp_wifi_disconnect();
					if(err != ESP_OK)
					{
						ESP_LOGE(MANAGER_TAG, "esp_wifi_disconnect failed: %s", esp_err_to_name(err));
					}
				}
				else if(wifi_state == WIFI_STATE_DISCONNECTED)
				{
					ESP_LOGI(MANAGER_TAG, "Already disconnected");
				}
				else
				{
					ESP_LOGW(MANAGER_TAG, "Disconnected ignored in state %s", wifi_state_to_string(wifi_state));
						
				}
				break;
			}
				
			case WIFI_MANAGER_CMD_SCAN:
			{
				ESP_LOGI(MANAGER_TAG, "MANAGER: WIFI_MANAGER_CMD_SCAN event");
				
				if(wifi_scan_purpose != WIFI_SCAN_PURPOSE_NONE)
				{
					ESP_LOGW(MANAGER_TAG, "Scan ignored: scan already in progress");
					break;
				}
				
				// SCAN дозволени тільки коли є стан WIFI_STATE_ONLINE або WIFI_STATE_DISCONNECTED
				if((wifi_state != WIFI_STATE_ONLINE) && (wifi_state != WIFI_STATE_DISCONNECTED))
				{
					ESP_LOGW(MANAGER_TAG, "Scan ignored: in state %s", wifi_state_to_string(wifi_state));
					break;
				}
				esp_err_t err = wifi_manager_start_scan(WIFI_SCAN_PURPOSE_USER);
				if(err != ESP_OK)
				{
					ESP_LOGE(MANAGER_TAG, "Failed to scan");
				}
				break;
			}
			
		
			case WIFI_MANAGER_CMD_RECONNECT:
			{
				ESP_LOGI(MANAGER_TAG, "WIFI_MANAGER_SMD_RECONNECT command received");
				
				if(auto_reconnect_anabled == false)
				{
					ESP_LOGI(MANAGER_TAG, "Autoreconnect ignored: autoreconnect disabled");
					break;
				}
				if(wifi_state != WIFI_STATE_DISCONNECTED)
				{
					ESP_LOGW(MANAGER_TAG, "Reconnect ignored in state %s", wifi_state_to_string(wifi_state));
					break;
				}
				
				if(reconnect_attempts >= MAX_RECONNEKT_ATTEMPTS)
				{
					ESP_LOGW(MANAGER_TAG, "Maximum reconnect attempts reached");
					break;
				}
					
				wifi_manager_start_connection();
				break;	
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

	// Create mamager queue
	wifi_manager_queue = xQueueCreate(WIFI_MANAGER_QUEUE_LENGTH , sizeof(wifi_manager_message_t));
	if(wifi_manager_queue == NULL)
	{
		ESP_LOGE(TAG, "Failed to create queue");
		return ESP_FAIL;
	}
	
	// Create reconnect timer  pdFALSE - mean ONECHOT TIMER
	wifi_reconnect_timmer = xTimerCreate("wifi_reconnect_timmer", pdMS_TO_TICKS(1000),pdFALSE, NULL, wifi_reconnect_timer_callback);
	if(wifi_reconnect_timmer == NULL)
	{
		ESP_LOGE(TAG, "Failed create reconnect timmer");
		return ESP_FAIL;
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


static void print_memory_info(void)
{
	const char *TAG = "MEMORY TEST >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>";
	
	UBaseType_t stack_free_words = uxTaskGetStackHighWaterMark(NULL);
	size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
	size_t min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
	
	ESP_LOGI(TAG,
		"Stask minimum free: %u words, "
		"HEAP free: %u bytes, "
		"Heap minimum free: %u bytes ",
		(unsigned)stack_free_words,
		(unsigned)free_heap,
		(unsigned)min_free_heap);
}

static bool rest_wait_for_state(wifi_state_t expected_state, uint32_t timeout_ms)
{
	const char *TAG = "TEST TASK";
	
	TickType_t start_tick = xTaskGetTickCount();
	TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
	
	while((xTaskGetTickCount() - start_tick) < timeout_ticks)
	{
		if(wifi_manager_get_state() == expected_state)
		{
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}
	
	
	ESP_LOGE(TAG, "Timeout: expected state =%s, current state =%s",
		wifi_state_to_string(expected_state),
		wifi_state_to_string(wifi_manager_get_state()));
	
	return false;
}


static void test_task(void *parameters)
{
	(void)parameters;
	
	const char *TAG = "TEST TASK >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> ";
	
	vTaskDelay(pdMS_TO_TICKS(10000));
	
	ESP_LOGI(TAG, "======================= MATRIX TEST START =======================");
	
	// Wait for initial connection
	if(rest_wait_for_state(WIFI_STATE_ONLINE, 15000) == false)
	{
		ESP_LOGE(TAG, "Failed Initial connected");
		vTaskDelete(NULL);
		return;
	}
	
	///////////////////////////////////////////////////////////////////////////////////
	ESP_LOGI(TAG, "TEST 1: ONLINE STATE + CONNECT -> MUST be ignored ");
	wifi_manager_connect();
	vTaskDelay(pdMS_TO_TICKS(500));
	
	if(wifi_manager_get_state() == WIFI_STATE_ONLINE)
	{
		ESP_LOGI(TAG, "STATUS: TEST 1: Pass");
	}
	else 
	{
		ESP_LOGE(TAG, "STATUS: TEST 1: Fall");
	}
	
	///////////////////////////////////////////////////////////////////////////////////
	ESP_LOGI(TAG, "TEST 2: ONLINE STATE + SCAN -> MUST be ignored ");
	wifi_manager_scan();
	vTaskDelay(pdMS_TO_TICKS(200));
	ESP_LOGI(TAG, "TEST 2A. Made SCAN againe");
	wifi_manager_scan();
	// Delay for finish first scan
	vTaskDelay(pdMS_TO_TICKS(4000));
	if(wifi_manager_get_state() == WIFI_STATE_ONLINE)
	{
		ESP_LOGI(TAG, "STATUS: TEST 2: Pass ");
	}
	else
	{
		ESP_LOGE(TAG, "STATUS: TEST 2: Fail. State %s:", wifi_state_to_string(wifi_manager_get_state()));
		
	}
	
	///////////////////////////////////////////////////////////////////////////////////
	ESP_LOGI(TAG, "TEST 3: ONLINE + DISCONECT -> allowed");
 	wifi_manager_disconnect();
	if(rest_wait_for_state(WIFI_STATE_DISCONNECTED, 3000))
	{
		ESP_LOGI(TAG, "STATUS: TEST 3: Pass");
	}
	else
	{
		ESP_LOGE(TAG, "STATUS: TEST 3: Fail");
		vTaskDelete(NULL);
		return;
	}
	
	///////////////////////////////////////////////////////////////////////////////////
	ESP_LOGI(TAG, "TEST 4: DISCONNECTED + DISCONECT ");
	wifi_manager_disconnect();
	vTaskDelay(pdMS_TO_TICKS(500));
	if(wifi_manager_get_state() == WIFI_STATE_DISCONNECTED)
	{
		ESP_LOGI(TAG, "STATUS: TEST 4: Pass");
	}
	else
	{
		ESP_LOGE(TAG, "STATUS: TEST 4: Fail");
	}
	
	///////////////////////////////////////////////////////////////////////////////////
	ESP_LOGI(TAG, "TEST 5: DISCONNECTED + SCAN -> allowed");
	wifi_manager_scan();
	vTaskDelay(pdMS_TO_TICKS(4000));
	if(wifi_manager_get_state() == WIFI_STATE_DISCONNECTED)
	{
		ESP_LOGI(TAG, "STATUS: TEST 5: Pass");
	}
	else
	{
		ESP_LOGE(TAG, "STATUS: TEST 5: Fail");
	}
	
	///////////////////////////////////////////////////////////////////////////////////
	ESP_LOGI(TAG, "TEST 6: DISCONNECTED + CONNECT -> allowed");
	wifi_manager_connect();
	if(rest_wait_for_state(WIFI_STATE_SELECTING_AP, 1000))
	{
		ESP_LOGI(TAG, "STATUS: TEST 6: Pass. WiFi is now in SELECTING_AP state");
	}
	else
	{
		ESP_LOGE(TAG, "STATUS: TEST 6: Fail");
	}
	
	ESP_LOGI(TAG, "TEST 6A: SELECTING_AP + CONNECT -> ignore");
	wifi_manager_connect();
	
	ESP_LOGI(TAG, "TEST 6B: SELECTING_AP + SCAN -> ignore");
	wifi_manager_scan();
	
	vTaskDelay(pdMS_TO_TICKS(200));
	
	
	///////////////////////////////////////////////////////////////////////////////////
	ESP_LOGI(TAG, "TEST 7: SELECTING_AP + DISCONNECT -> allowed  (Cansel connection)");
	wifi_manager_disconnect();
	if(rest_wait_for_state(WIFI_STATE_DISCONNECTED, 5000))
	{
		ESP_LOGI(TAG, "STATUS: TEST 7: Pass.");
	}
	else
	{
		ESP_LOGE(TAG, "STATUS: TEST 7: Fail");
	}
	
	
	///////////////////////////////////////////////////////////////////////////////////
	ESP_LOGI(TAG, "TEST 8: DISCONNECT + CONNECT -> allowed");
	wifi_manager_connect();
	if(rest_wait_for_state(WIFI_STATE_CONNECTED, 10000))
	{
		ESP_LOGI(TAG, "STATUS: TEST 8: Pass.");
	}
	else
	{
		ESP_LOGE(TAG, "STATUS: TEST 8: Fail");
	}
	
	ESP_LOGI(TAG, "======================= MATRIX TEST FINICH =======================");
	
	
	vTaskDelay(pdMS_TO_TICKS(5000));
	
	
	for(uint32_t i = 1; i <= 100; i++)
	{
		ESP_LOGI(TAG, "------------------- START test: %" PRIu32" --------------------", i);
		
		wifi_manager_disconnect();
		ESP_LOGI(TAG, ">>>>>>>>>>>>>>> Send DISCONNECT command");
		if(rest_wait_for_state(WIFI_STATE_DISCONNECTED, 2000))
		{
			ESP_LOGI(TAG, "STATUS: TEST CONNECT TO AP: Pass.");
		}
		else
		{
			ESP_LOGE(TAG, "STATUS: TEST DISCONNECT TO AP: Fail");
			vTaskDelete(NULL);
			return;
		}
	
		vTaskDelay(pdMS_TO_TICKS(500));
		
	
		ESP_LOGI(TAG, ">>>>>>>>>>>>>>> Send CONNECT command");
		wifi_manager_connect();
		if(rest_wait_for_state(WIFI_STATE_CONNECTED, 15000))
		{
			ESP_LOGI(TAG, "STATUS: TEST CONNECT TO AP: Pass.");
		}
		else
		{
			ESP_LOGE(TAG, "STATUS: TEST CONNECT TO AP: Fail");
			vTaskDelete(NULL);
			return;
		}
		
		vTaskDelay(pdMS_TO_TICKS(500));
		
		print_memory_info();
		
		vTaskDelay(pdMS_TO_TICKS(2000));
		
		ESP_LOGI(TAG, "------------------- FINISH ---------------------");
	}

	ESP_LOGI(TAG, "------------------- DONE ---------------------");
	vTaskDelete(NULL);
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
	
	////////// Test task
	//xTaskCreatePinnedToCore(test_task, "test_task", 2048, NULL, 3, NULL, TEST_CORE);
	

}
