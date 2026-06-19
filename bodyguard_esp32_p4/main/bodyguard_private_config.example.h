#pragma once

/*
 * Copy this file to bodyguard_private_config.h before building.
 * Keep bodyguard_private_config.h out of Git because it contains local
 * WiFi credentials, cloud endpoints, and device secrets.
 */

#define BODYGUARD_WIFI_SSID                    "YOUR_WIFI_SSID"
#define BODYGUARD_WIFI_PASSWORD                "YOUR_WIFI_PASSWORD"
#define BODYGUARD_CLOUD_HOST                   "http://YOUR_SERVER_IP:5000"

/* Optional Tuya identity. Leave blank if Tuya integration is not used. */
#define BODYGUARD_TUYA_PID                     ""
#define BODYGUARD_TUYA_UUID                    ""
#define BODYGUARD_TUYA_AUTHKEY                 ""

