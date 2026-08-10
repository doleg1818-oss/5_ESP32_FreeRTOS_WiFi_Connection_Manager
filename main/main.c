#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "secrets.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi_types_generic.h"
#include "nvs_flash.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"


static const char *TAG = "WIFI STA";

static void wifi_event_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	(void)handler_arg;
	
	if(event_base == WIFI_EVENT)
	{
		switch(event_id)
		{
			case WIFI_EVENT_STA_START:
				ESP_LOGI(TAG, "WIFI_EVENT_STA_START");
				ESP_LOGI(TAG, "Connect to AP...");
				
				esp_err_t status = esp_wifi_connect();
				if(status != ESP_OK)
				{
					ESP_LOGE(TAG, "esp_wifi_connect:%s", esp_err_to_name(status));
				}
			break;
			
			case WIFI_EVENT_STA_CONNECTED:
				ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED");
				ESP_LOGI(TAG, "WiFi link established, waiting for IP...");
			break;
			
			case WIFI_EVENT_STA_DISCONNECTED:
				ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
			break;
			
			default:
			{
				break;
			}
		}
	}
	else if((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP))
	{
		esp_netif_t *sta_netif = (esp_netif_t *)handler_arg;
		ip_event_got_ip_t *event = (ip_event_got_ip_t*)event_data;
		
		ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP");
		
		ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
		ESP_LOGI(TAG, "NETMASK: " IPSTR, IP2STR(&event->ip_info.netmask));
		ESP_LOGI(TAG, "GATEWAY: " IPSTR, IP2STR(&event->ip_info.gw));
		
		// Get SSID, BSSID, Chanell, RSSI
		wifi_ap_record_t ap_info = {0};
		esp_err_t status = esp_wifi_sta_get_ap_info(&ap_info);
		if(status == ESP_OK)
		{
			ESP_LOGI(TAG, "SSID: %s", (char *)ap_info.bssid);
			ESP_LOGI(TAG, "BSSID: %02X:%02X:%02X:%02X:%02X:%02X",
				ap_info.bssid[0],
				ap_info.bssid[1],
				ap_info.bssid[2],
				ap_info.bssid[3],
				ap_info.bssid[4],
				ap_info.bssid[5]);
			ESP_LOGI(TAG, "CHANNEL: %u", ap_info.primary);
			ESP_LOGI(TAG, "RSSI: %d dBm", ap_info.rssi);		
		}
		else{
			ESP_LOGE(TAG, "Failed to get AP info: %s", esp_err_to_name(status));
		}
		
		// Get MAC address
		uint8_t sta_mac[6] = {0};
		esp_err_t mac_status = esp_wifi_get_mac(WIFI_IF_STA, sta_mac);
		if(mac_status == ESP_OK)
		{
			ESP_LOGI(TAG, "STA MAC: %02X:%02X:%02X:%02X:%02X:%02X",
				sta_mac[0],
				sta_mac[1],
				sta_mac[2],
				sta_mac[3],
				sta_mac[4],
				sta_mac[5]);
		} 
		else 
		{
			ESP_LOGE(TAG, "Failet to get MAC adress. err :%s", esp_err_to_name(mac_status));
		}
		
		// Get DNS
		if(sta_netif != NULL)
		{
			esp_netif_dns_info_t dnf_info = {0};
			
			esp_err_t dns_status = esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dnf_info);
			if(dns_status == ESP_OK)
			{
				ESP_LOGI(TAG, "DNS: " IPSTR, IP2STR(&dnf_info.ip.u_addr.ip4));	
			}
			else
			{
				ESP_LOGE(TAG, "Failet to get DNS. err:%s", esp_err_to_name(dns_status));
			}
			
			
		}
		
		
		
		
	}
}

void app_main(void)
{
	ESP_LOGI(TAG, "Aplication started");
	
	esp_err_t status = nvs_flash_init();
	if(status == ESP_ERR_NVS_NO_FREE_PAGES || status == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		status = nvs_flash_init();
	}
	
	ESP_ERROR_CHECK(esp_netif_init());		// TCP/IP networt interface infrastructure
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
	if(sta_netif == NULL)
	{
		ESP_LOGE(TAG, "Failed to create default wi-fi STA netif");
		return;
	}
	ESP_LOGI(TAG, "Network infrastructure created");
	
	// Instal WiFi driver
	wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
	
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, sta_netif, NULL));
	
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));	// Kip runtime WiFi config in RAM
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	
	ESP_LOGI(TAG, "WiFi driver instaled in STA mode");
	
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = WIFI_SSID,
			.password = WIFI_PASSWORD
		}
	};
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	
	
	
	
	
	
	
	
	
	
	
	
	

}
