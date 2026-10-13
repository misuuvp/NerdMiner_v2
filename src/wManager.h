#ifndef _WMANAGER_H
#define _WMANAGER_H

void init_WifiManager();
void wifiManagerProcess();
void reset_configuration();
void reset_wifi_configuration();
bool saveWifiBSSIDConfig(const char* bssid, int channel, bool lock, bool fallback);
bool savePoolConfig(const char* host, int port, const char* wallet, const char* password, bool changePassword);
#endif // _WMANAGER_H
