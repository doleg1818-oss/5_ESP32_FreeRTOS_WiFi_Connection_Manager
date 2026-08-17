/*
 * wifi_manager.h
 *
 *  Created on: Aug 15, 2026
 *      Author: Olegd
 */

#ifndef MAIN_WIFI_MANAGER_H_
#define MAIN_WIFI_MANAGER_H_

#include "stdbool.h"

typedef enum{
	WIFI_STATE_INIT = 0,
	WIFI_STATE_CONNECTING,
	WIFI_STATE_SELECTING_AP,
	WIFI_STATE_CONNECTED,
	WIFI_STATE_ONLINE,
	WIFI_STATE_DISCONNECTED	
}wifi_state_t;

// Rrquest WiFi connection
bool wifi_manager_connect(void);	
// Rrquest WiFi disconnection
bool wifi_manager_disconnect(void);
// Rrquest WiFi cto scan
bool wifi_manager_scan(void);

wifi_state_t wifi_manager_get_state(void);
bool wifi_manager_is_online(void);



#endif /* MAIN_WIFI_MANAGER_H_ */
