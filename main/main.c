#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"


static const char *TAG = "WIFI STA";



void app_main(void)
{
	ESP_LOGI(TAG, "Aplication started");
	
	esp_err_t status = nvs_flash_init();
	if(status == ESP_ERR_NVS_NO_FREE_PAGES || status == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		status = nvs_flash_init();
	}
	
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
	if(sta_netif == NULL)
	{
		ESP_LOGE(TAG, "Failed to create default wi-fi STA netif");
		return;
	}
	ESP_LOGI(TAG, "Network infrastructure created");
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	

}
