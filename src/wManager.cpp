#define ESP_DRD_USE_SPIFFS true

// Include Libraries
//#include ".h"

#include <WiFi.h>

#include <WiFiManager.h>

#include "wManager.h"
#include "monitor.h"
#include "drivers/displays/display.h"
#include "drivers/storage/SDCard.h"
#include "drivers/storage/nvMemory.h"
#include "drivers/storage/storage.h"
#include "mining.h"
#include "timeconst.h"

#include <ArduinoJson.h>
#include <esp_flash.h>


// Flag for saving data
bool shouldSaveConfig = false;

// Variables to hold data from custom textboxes
TSettings Settings;

// Define WiFiManager Object
WiFiManager wm;
extern monitor_data mMonitor;

nvMemory nvMem;

extern SDCard SDCrd;

bool parseBSSID(const String& bssidString, uint8_t bssid[6])
{
    String value = bssidString;
    value.trim();

    unsigned int bytes[6];

    if (sscanf(value.c_str(),
               "%02x:%02x:%02x:%02x:%02x:%02x",
               &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6)
    {
        return false;
    }

    for (int i = 0; i < 6; i++)
    {
        if (bytes[i] > 0xFF)
            return false;

        bssid[i] = static_cast<uint8_t>(bytes[i]);
    }

    return true;
}

bool saveWifiBSSIDConfig(const char* bssid, int channel, bool lock, bool fallback)
{
    if (bssid == nullptr)
        return false;

    String bssidString = String(bssid);
    bssidString.trim();
    bssidString.toUpperCase();

    uint8_t parsedBSSID[6];

    if (!parseBSSID(bssidString, parsedBSSID))
    {
        Serial.println("WiFi config: invalid BSSID");
        return false;
    }

    if (channel < 1 || channel > 14)
    {
        Serial.println("WiFi config: invalid channel");
        return false;
    }

    Settings.WifiBSSID = bssidString;
    Settings.WifiChannel = channel;
    Settings.WifiBSSIDLock = lock;
    Settings.WifiFallback = fallback;

    Serial.println("WiFi config: saving BSSID settings");
    Serial.print("BSSID: ");
    Serial.println(Settings.WifiBSSID);
    Serial.print("Channel: ");
    Serial.println(Settings.WifiChannel);
    Serial.print("Lock: ");
    Serial.println(Settings.WifiBSSIDLock ? "ON" : "OFF");
    Serial.print("Fallback: ");
    Serial.println(Settings.WifiFallback ? "ON" : "OFF");

    return nvMem.saveConfig(&Settings);
}

bool savePoolConfig(const char* host,
                    int port,
                    const char* wallet,
                    const char* password,
                    bool changePassword)
{
    if (host == nullptr || wallet == nullptr)
        return false;

    String hostString = String(host);
    hostString.trim();

    String walletString = String(wallet);
    walletString.trim();

    if (hostString.length() == 0 || hostString.length() > 80)
    {
        Serial.println("Pool config: invalid host");
        return false;
    }

    if (port < 1 || port > 65535)
    {
        Serial.println("Pool config: invalid port");
        return false;
    }

    if (walletString.length() == 0 || walletString.length() >= sizeof(Settings.BtcWallet))
    {
        Serial.println("Pool config: invalid wallet/worker");
        return false;
    }

    if (changePassword)
    {
        if (password == nullptr ||
            strlen(password) >= sizeof(Settings.PoolPassword))
        {
            Serial.println("Pool config: invalid password");
            return false;
        }
    }

    Settings.PoolAddress = hostString;
    Settings.PoolPort = port;

    strncpy(Settings.BtcWallet,
            walletString.c_str(),
            sizeof(Settings.BtcWallet) - 1);

    Settings.BtcWallet[sizeof(Settings.BtcWallet) - 1] = '\0';

    if (changePassword)
    {
        strncpy(Settings.PoolPassword,
                password,
                sizeof(Settings.PoolPassword) - 1);

        Settings.PoolPassword[sizeof(Settings.PoolPassword) - 1] = '\0';
    }

    Serial.println("Pool config: saving settings");
    Serial.print("Host: ");
    Serial.println(Settings.PoolAddress);
    Serial.print("Port: ");
    Serial.println(Settings.PoolPort);
    Serial.print("Wallet/worker: ");
    Serial.println(Settings.BtcWallet);
    Serial.print("Password changed: ");
    Serial.println(changePassword ? "YES" : "NO");

    return nvMem.saveConfig(&Settings);
}

bool connectToSavedSSIDAnyBSSID()
{
    String savedSSID = wm.getWiFiSSID(true);
    String savedPassword = wm.getWiFiPass(true);

    if (savedSSID.length() == 0)
    {
        Serial.println("WiFi fallback: no saved SSID");
        return false;
    }

    Serial.println("WiFi fallback: connecting without BSSID lock");
    Serial.print("SSID: ");
    Serial.println(savedSSID);

    // Remove the currently active BSSID-specific STA configuration
    // without erasing the saved SSID/password.
    WiFi.disconnect(false, false);
    delay(250);

    // Scan all channels and prefer the strongest matching AP.
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

    // channel = 0 and BSSID = nullptr intentionally remove AP pinning.
    WiFi.begin(
        savedSSID.c_str(),
        savedPassword.c_str(),
        0,
        nullptr,
        true
    );

    unsigned long startTime = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime < 15000)
    {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("WiFi fallback: connected");
        Serial.print("BSSID: ");
        Serial.println(WiFi.BSSIDstr());
        Serial.print("Channel: ");
        Serial.println(WiFi.channel());
        Serial.print("RSSI: ");
        Serial.println(WiFi.RSSI());

        return true;
    }

    Serial.println("WiFi fallback: connection failed");
    return false;
}
bool connectToLockedBSSID()
{
    if (!Settings.WifiBSSIDLock)
        return false;

    if (Settings.WifiBSSID.length() == 0)
        return false;

    if (Settings.WifiChannel < 1 || Settings.WifiChannel > 14)
    {
        Serial.println("BSSID lock: invalid WiFi channel");
        return false;
    }

    uint8_t bssid[6];

    if (!parseBSSID(Settings.WifiBSSID, bssid))
    {
        Serial.println("BSSID lock: invalid BSSID");
        return false;
    }

    String savedSSID = wm.getWiFiSSID(true);
    String savedPassword = wm.getWiFiPass(true);

    if (savedSSID.length() == 0)
    {
        Serial.println("BSSID lock: no saved WiFi SSID");
        return false;
    }

    Serial.println("BSSID lock: connecting to selected AP");
    Serial.print("SSID: ");
    Serial.println(savedSSID);

    Serial.print("BSSID: ");
    Serial.println(Settings.WifiBSSID);

    Serial.print("Channel: ");
    Serial.println(Settings.WifiChannel);

    WiFi.begin(
        savedSSID.c_str(),
        savedPassword.c_str(),
        Settings.WifiChannel,
        bssid,
        true
    );

    unsigned long startTime = millis();

    while (WiFi.status() != WL_CONNECTED &&
           millis() - startTime < 15000)
    {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("BSSID lock: connected");
        return true;
    }

    Serial.println("BSSID lock: connection failed");
    return false;
}

String readCustomAPName() {
    Serial.println("DEBUG: Attempting to read custom AP name from flash at 0x3F0000...");
    
    // Leer directamente desde flash
    const size_t DATA_SIZE = 128;
    uint8_t buffer[DATA_SIZE];
    memset(buffer, 0, DATA_SIZE); // Clear buffer
    
    // Leer desde 0x3F0000
    esp_err_t result = esp_flash_read(NULL, buffer, 0x3F0000, DATA_SIZE);
    if (result != ESP_OK) {
        Serial.printf("DEBUG: Flash read error: %s\n", esp_err_to_name(result));
        return "";
    }
    
    Serial.println("DEBUG: Successfully read from flash");
    String data = String((char*)buffer);
    
    // Debug: show raw data read
    Serial.printf("DEBUG: Raw flash data: '%s'\n", data.c_str());
    
    if (data.startsWith("WEBFLASHER_CONFIG:")) {
        Serial.println("DEBUG: Found WEBFLASHER_CONFIG marker");
        String jsonPart = data.substring(18); // Después del marcador "WEBFLASHER_CONFIG:"
        
        Serial.printf("DEBUG: JSON part: '%s'\n", jsonPart.c_str());
        
        DynamicJsonDocument doc(256);
        DeserializationError error = deserializeJson(doc, jsonPart);
        
        if (error == DeserializationError::Ok) {
            Serial.println("DEBUG: JSON parsed successfully");
            
            if (doc.containsKey("apname")) {
                String customAP = doc["apname"].as<String>();
                customAP.trim();
                
                if (customAP.length() > 0 && customAP.length() < 32) {
                    Serial.printf("✅ Custom AP name from webflasher: %s\n", customAP.c_str());
                    return customAP;
                } else {
                    Serial.printf("DEBUG: AP name invalid length: %d\n", customAP.length());
                }
            } else {
                Serial.println("DEBUG: 'apname' key not found in JSON");
            }
        } else {
            Serial.printf("DEBUG: JSON parse error: %s\n", error.c_str());
        }
    } else {
        Serial.println("DEBUG: WEBFLASHER_CONFIG marker not found - no custom config");
    }
    
    Serial.println("DEBUG: Using default AP name");
    return "";
}

void saveConfigCallback()
// Callback notifying us of the need to save configuration
{
    Serial.println("Should save config");
    shouldSaveConfig = true;    
    //wm.setConfigPortalBlocking(false);
}

/* void saveParamsCallback()
// Callback notifying us of the need to save configuration
{
    Serial.println("Should save config");
    shouldSaveConfig = true;
    nvMem.saveConfig(&Settings);
} */

void configModeCallback(WiFiManager* myWiFiManager)
// Called when config mode launched
{
    Serial.println("Entered Configuration Mode");
    drawSetupScreen();
    Serial.print("Config SSID: ");
    Serial.println(myWiFiManager->getConfigPortalSSID());

    Serial.print("Config IP Address: ");
    Serial.println(WiFi.softAPIP());
}

void reset_configuration()
{
    Serial.println("Erasing Config, restarting");
    nvMem.deleteConfig();
    resetStat();
    wm.resetSettings();
    ESP.restart();
}

void reset_wifi_configuration()
{
    Serial.println("Erasing WiFi configuration, restarting");

    // Clear our BSSID-specific settings.
    Settings.WifiBSSIDLock = false;
    Settings.WifiBSSID = "";
    Settings.WifiChannel = 0;
    Settings.WifiFallback = true;

    // Keep pool/wallet/etc., only update the Wi-Fi related extras.
    nvMem.saveConfig(&Settings);

    // Remove WiFiManager's stored SSID/password.
    wm.resetSettings();

    delay(250);
    ESP.restart();
}

void init_WifiManager()
{
#ifdef MONITOR_SPEED
    Serial.begin(MONITOR_SPEED);
#else
    Serial.begin(115200);
#endif //MONITOR_SPEED
#if ARDUINO_USB_CDC_ON_BOOT
    Serial.setTxTimeoutMs(0);
#endif
    //Serial.setTxTimeoutMs(10);
    
    // Check for custom AP name from flasher config, otherwise use default
    String customAPName = readCustomAPName();
    const char* apName = customAPName.length() > 0 ? customAPName.c_str() : DEFAULT_SSID;

    //Init pin 15 to eneble 5V external power (LilyGo bug)
#ifdef PIN_ENABLE5V
    pinMode(PIN_ENABLE5V, OUTPUT);
    digitalWrite(PIN_ENABLE5V, HIGH);
#endif

    // Change to true when testing to force configuration every time we run
    bool forceConfig = false;

    // True only when the device has no saved configuration yet.
    bool freshConfig = false;

#if defined(PIN_BUTTON_2)
    // Check if button2 is pressed to enter configMode with actual configuration
    if (!digitalRead(PIN_BUTTON_2)) {
        Serial.println(F("Button pressed to force start config mode"));
        forceConfig = true;
        wm.setBreakAfterConfig(true); //Set to detect config edition and save
    }
#endif
    // Explicitly set WiFi mode
    WiFi.mode(WIFI_STA);

    if (!nvMem.loadConfig(&Settings))
    {
        //No config file on internal flash.
        if (SDCrd.loadConfigFile(&Settings))
        {
            //Config file on SD card.
            SDCrd.SD2nvMemory(&nvMem, &Settings); // reboot on success.          
        }
        else
        {
            // No config file anywhere: first-time setup.
            forceConfig = true;
            freshConfig = true;
        }
    };
    
    // Free the memory from SDCard class 
    SDCrd.terminate();
    
    // Reset settings (only for development)
    //wm.resetSettings();

    //Set dark theme
    //wm.setClass("invert"); // dark theme

    // Set config save notify callback
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setSaveParamsCallback(saveConfigCallback);

    // Set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wm.setAPCallback(configModeCallback);    

    //Advanced settings
    wm.setConfigPortalBlocking(false); //Hacemos que el portal no bloquee el firmware
    wm.setConnectTimeout(40); // how long to try to connect for before continuing
    wm.setConfigPortalTimeout(180); // auto close configportal after n seconds
    // wm.setCaptivePortalEnable(false); // disable captive portal redirection
    // wm.setAPClientCheck(true); // avoid timeout if client connected to softap
    //wm.setTimeout(120);
    //wm.setConfigPortalTimeout(120); //seconds

    // Custom elements

    // Text box (String) - 80 characters maximum
    WiFiManagerParameter pool_text_box("Poolurl", "Pool url", freshConfig ? "" : Settings.PoolAddress.c_str(), 80);

    // Need to convert numerical input to string to display the default value.
    char convertedValue[6];

    if (freshConfig)
        convertedValue[0] = '\0';
    else
        sprintf(convertedValue, "%d", Settings.PoolPort);

    // Text box (Number) - 7 characters maximum
    WiFiManagerParameter port_text_box_num("Poolport", "Pool port", convertedValue, 7);

    // Text box (String) - 80 characters maximum
    //WiFiManagerParameter password_text_box("Poolpassword", "Pool password (Optional)", Settings.PoolPassword, 80);

    // Text box (String) - 80 characters maximum
    WiFiManagerParameter addr_text_box("btcAddress", "Your BTC address", freshConfig ? "" : Settings.BtcWallet, 80);

  // Text box (Number) - 2 characters maximum
  char charZone[6];
  sprintf(charZone, "%d", Settings.Timezone);
  WiFiManagerParameter time_text_box_num("TimeZone", "TimeZone fromUTC (-12/+12)", charZone, 3);

  WiFiManagerParameter features_html("<hr><br><label style=\"font-weight: bold;margin-bottom: 25px;display: inline-block;\">Features</label>");

  char checkboxParams[24] = "type=\"checkbox\"";
  if (Settings.saveStats)
  {
    strcat(checkboxParams, " checked");
  }
  WiFiManagerParameter save_stats_to_nvs("SaveStatsToNVS", "Save mining statistics to flash memory.", "T", 2, checkboxParams, WFM_LABEL_AFTER);
  // Text box (String) - 80 characters maximum
  WiFiManagerParameter password_text_box("Poolpassword - Optional", "Pool password", freshConfig ? "" : Settings.PoolPassword, 80);
  // WM_AP_SUPPORT_INTEGRATION_BEGIN
  WiFiManagerParameter support_referral_html(R"WMHTML(
<div id="wmSupportApNote"
     style="display:none;
            margin:12px 0 6px 0;
            text-align:center;
            font-size:12px;
            opacity:0.65;">
  <strong>Optional support</strong><br>
  After saving you will be redirected to the pool affiliate page.<br>
  If you check your miner there, you support Web Manager development
  through the pool affiliate program.
</div>

<script>
(function()
{
  function normalizeHost(value)
  {
    return String(value || '')
      .trim()
      .toLowerCase()
      .replace(/^stratum\+tcp:\/\//, '')
      .replace(/^https?:\/\//, '')
      .split('/')[0]
      .split(':')[0]
      .replace(/\.$/, '');
  }

  function supportUrl(value)
  {
    const host = normalizeHost(value);

    const matches = (base) =>
      host === base || host.endsWith('.' + base);

    if (matches('xec.hmpool.io'))
      return 'https://xec.hmpool.io?ref=HM-39uakb';

    if (matches('digi.hmpool.io'))
      return 'https://digi.hmpool.io?ref=HM-qg3swe';

    if (matches('btc.hmpool.io'))
      return 'https://hmpool.io?ref=HM-dk6cuv';

    if (matches('bch.hmpool.io'))
      return 'https://bch.hmpool.io?ref=HM-r6zdkz';

    return '';
  }

  function install()
  {
    const pool = document.getElementById('Poolurl');

    if (!pool || !pool.form)
      return;

    const form = pool.form;
    const note = document.getElementById('wmSupportApNote');

    const initialPool = normalizeHost(pool.value);

    function currentSupportUrl()
    {
      const currentPool = normalizeHost(pool.value);

      if (currentPool === initialPool)
        return '';

      return supportUrl(currentPool);
    }

    function updateNote()
    {
      if (!note)
        return;

      note.style.display =
        currentSupportUrl() ? 'block' : 'none';
    }

    pool.addEventListener('input', updateNote);
    pool.addEventListener('change', updateNote);

    form.addEventListener('submit', function(event)
    {
      const url = currentSupportUrl();

      /*
       * Non-HMPool or unchanged pool:
       * keep normal WiFiManager behaviour.
       */
      if (!url)
        return;

      event.preventDefault();

      const formData = new FormData(form);
      const encoded = new URLSearchParams();

      formData.forEach(function(value, key)
      {
        encoded.append(key, value);
      });

      let action = form.getAttribute('action') || window.location.pathname;
      const method = (form.method || 'POST').toUpperCase();

      action = new URL(action, window.location.href).toString();

      document.body.innerHTML =
        '<div style="' +
          'font-family:sans-serif;' +
          'max-width:520px;' +
          'margin:60px auto;' +
          'padding:24px;' +
          'text-align:center;' +
        '">' +
          '<h2>Settings saved</h2>' +
          '<p>Support Web Manager development</p>' +
          '<p>' +
            'If you check your miner on the pool page that opens, ' +
            'you support Web Manager development through the pool affiliate program.' +
          '</p>' +
          '<p><strong>This is optional.</strong></p>' +
          '<p style="font-size:13px;opacity:.65">' +
            'Waiting for Internet connection...' +
          '</p>' +
          '<p style="font-size:13px;opacity:.65">' +
            'If the pool page does not open automatically, ' +
            'refresh this page once your Internet connection is back.' +
          '</p>' +
        '</div>';

      const options = {
        method: method,
        cache: 'no-store'
      };

      if (method === 'GET')
      {
        const separator = action.includes('?') ? '&' : '?';
        action += separator + encoded.toString();
      }
      else
      {
        options.headers = {
          'Content-Type':
            'application/x-www-form-urlencoded;charset=UTF-8'
        };

        options.body = encoded.toString();
      }

      fetch(action, options).catch(function()
      {
        /*
         * Expected when the ESP leaves AP mode or restarts.
         */
      });

      setTimeout(function()
      {
        window.location.href = url;
      }, 12000);
    });

    updateNote();
  }

  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', install);
  else
    install();
})();
</script>
)WMHTML");
  // WM_AP_SUPPORT_INTEGRATION_END

  // Add all defined parameters
  wm.addParameter(&pool_text_box);
  wm.addParameter(&port_text_box_num);
  wm.addParameter(&password_text_box);
  wm.addParameter(&support_referral_html);
  wm.addParameter(&addr_text_box);
  wm.addParameter(&time_text_box_num);
  wm.addParameter(&features_html);
  wm.addParameter(&save_stats_to_nvs);
  #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
  char checkboxParams2[24] = "type=\"checkbox\"";
  if (Settings.invertColors)
  {
    strcat(checkboxParams2, " checked");
  }
  WiFiManagerParameter invertColors("inverColors", "Invert Display Colors (if the colors looks weird)", "T", 2, checkboxParams2, WFM_LABEL_AFTER);
  wm.addParameter(&invertColors);
  #endif
  #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
    char brightnessConvValue[2];
    sprintf(brightnessConvValue, "%d", Settings.Brightness);
    // Text box (Number) - 3 characters maximum
    WiFiManagerParameter brightness_text_box_num("Brightness", "Screen backlight Duty Cycle (0-255)", brightnessConvValue, 3);
    wm.addParameter(&brightness_text_box_num);
  #endif

    Serial.println("AllDone: ");
    if (forceConfig)    
    {
        // Run if we need a configuration
        //No configuramos timeout al modulo
        wm.setConfigPortalBlocking(true); //Hacemos que el portal SI bloquee el firmware
        drawSetupScreen();
        mMonitor.NerdStatus = NM_Connecting;
        wm.startConfigPortal(apName, DEFAULT_WIFIPW);

        if (shouldSaveConfig)
        {
            //Could be break forced after edditing, so save new config
            Serial.println("failed to connect and hit timeout");
            Settings.PoolAddress = pool_text_box.getValue();
            Settings.PoolPort = atoi(port_text_box_num.getValue());
            strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
            strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet));
            Settings.Timezone = atoi(time_text_box_num.getValue());
            //Serial.println(save_stats_to_nvs.getValue());
            Settings.saveStats = (strncmp(save_stats_to_nvs.getValue(), "T", 1) == 0);
            #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
            #endif
            #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.Brightness = atoi(brightness_text_box_num.getValue());
            #endif
            nvMem.saveConfig(&Settings);
            delay(3*SECOND_MS);
            //reset and try again, or maybe put it to deep sleep
            ESP.restart();            
        };
    }
    else
    {
        //Tratamos de conectar con la configuración inicial ya almacenada
        mMonitor.NerdStatus = NM_Connecting;
        // disable captive portal redirection
        wm.setCaptivePortalEnable(true); 
        wm.setConfigPortalBlocking(true);
        wm.setEnableConfigPortal(true);
        // if (!wm.autoConnect(Settings.WifiSSID.c_str(), Settings.WifiPW.c_str()))
        bool wifiConnected = false;

if (Settings.WifiBSSIDLock)
{
    wifiConnected = connectToLockedBSSID();

    if (!wifiConnected)
    {
        WiFi.disconnect();
        delay(250);

        if (Settings.WifiFallback)
        {
            Serial.println("BSSID lock: selected AP unavailable");
            Serial.println("BSSID lock: fallback to normal WiFiManager connection");

            wifiConnected = connectToSavedSSIDAnyBSSID();

            if (!wifiConnected)
            {
                Serial.println("WiFi fallback failed, starting WiFiManager recovery");
                wifiConnected = wm.autoConnect(apName, DEFAULT_WIFIPW);
            }
        }
        else
        {
            Serial.println("BSSID lock: selected AP unavailable");
            Serial.println("BSSID lock: fallback disabled");
            Serial.println("Starting configuration AP for recovery");

            wifiConnected = wm.startConfigPortal(apName, DEFAULT_WIFIPW);

            if (wifiConnected)
            {
                // A felhasználó kézzel új Wi-Fi kapcsolatot állított be.
                // Kapcsoljuk ki a régi BSSID lockot, különben
                // a következő újraindításkor ismét a régi AP-t keresné.
                Settings.WifiBSSIDLock = false;
                Settings.WifiBSSID = "";
                Settings.WifiChannel = 0;
                Settings.WifiFallback = true;

                nvMem.saveConfig(&Settings);

                Serial.println("BSSID lock cleared after manual WiFi recovery");
            }
        }
    }
}
else
{
    // Always connect with BSSID unset so identical-SSID APs
    // can be selected freely.
    wifiConnected = connectToSavedSSIDAnyBSSID();

    if (!wifiConnected)
    {
        wifiConnected = wm.autoConnect(apName, DEFAULT_WIFIPW);
    }
}

if (!wifiConnected)
        {
            Serial.println("Failed to connect to configured WIFI, and hit timeout");
            if (shouldSaveConfig) {
                // Save new config            
                Settings.PoolAddress = pool_text_box.getValue();
                Settings.PoolPort = atoi(port_text_box_num.getValue());
                strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
                strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet));
                Settings.Timezone = atoi(time_text_box_num.getValue());
                // Serial.println(save_stats_to_nvs.getValue());
                Settings.saveStats = (strncmp(save_stats_to_nvs.getValue(), "T", 1) == 0);
                #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
                #endif
                #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
                Settings.Brightness = atoi(brightness_text_box_num.getValue());
                #endif
                nvMem.saveConfig(&Settings);
                vTaskDelay(2000 / portTICK_PERIOD_MS);      
            }        
            ESP.restart();                            
        } 
    }
    
    //Conectado a la red Wifi
    if (WiFi.status() == WL_CONNECTED) {
        //tft.pushImage(0, 0, MinerWidth, MinerHeight, MinerScreen);
        Serial.println("");
        Serial.println("WiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());


        // Lets deal with the user config values

        // Copy the string value
        Settings.PoolAddress = pool_text_box.getValue();
        //strncpy(Settings.PoolAddress, pool_text_box.getValue(), sizeof(Settings.PoolAddress));
        Serial.print("PoolString: ");
        Serial.println(Settings.PoolAddress);

        //Convert the number value
        Settings.PoolPort = atoi(port_text_box_num.getValue());
        Serial.print("portNumber: ");
        Serial.println(Settings.PoolPort);

        // Copy the string value
        strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
        Serial.print("poolPassword: ");
        Serial.println(Settings.PoolPassword);

        // Copy the string value
        strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet));
        Serial.print("btcString: ");
        Serial.println(Settings.BtcWallet);

        //Convert the number value
        Settings.Timezone = atoi(time_text_box_num.getValue());
        Serial.print("TimeZone fromUTC: ");
        Serial.println(Settings.Timezone);

        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
        Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
        Serial.print("Invert Colors: ");
        Serial.println(Settings.invertColors);        
        #endif

        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
        Settings.Brightness = atoi(brightness_text_box_num.getValue());
        Serial.print("Brightness: ");
        Serial.println(Settings.Brightness);
        #endif

    }

    // Lets deal with the user config values

    // Copy the string value
    Settings.PoolAddress = pool_text_box.getValue();
    //strncpy(Settings.PoolAddress, pool_text_box.getValue(), sizeof(Settings.PoolAddress));
    Serial.print("PoolString: ");
    Serial.println(Settings.PoolAddress);

    //Convert the number value
    Settings.PoolPort = atoi(port_text_box_num.getValue());
    Serial.print("portNumber: ");
    Serial.println(Settings.PoolPort);

    // Copy the string value
    strncpy(Settings.PoolPassword, password_text_box.getValue(), sizeof(Settings.PoolPassword));
    Serial.print("poolPassword: ");
    Serial.println(Settings.PoolPassword);

    // Copy the string value
    strncpy(Settings.BtcWallet, addr_text_box.getValue(), sizeof(Settings.BtcWallet));
    Serial.print("btcString: ");
    Serial.println(Settings.BtcWallet);

    //Convert the number value
    Settings.Timezone = atoi(time_text_box_num.getValue());
    Serial.print("TimeZone fromUTC: ");
    Serial.println(Settings.Timezone);

    #ifdef ESP32_2432S028R
    Settings.invertColors = (strncmp(invertColors.getValue(), "T", 1) == 0);
    Serial.print("Invert Colors: ");
    Serial.println(Settings.invertColors);
    #endif

    // Save the custom parameters to FS
    if (shouldSaveConfig)
    {
        nvMem.saveConfig(&Settings);
        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
         if (Settings.invertColors) ESP.restart();                
        #endif
        #if defined(ESP32_2432S028R) || defined(ESP32_2432S028_2USB)
        if (Settings.Brightness != 250) ESP.restart();
        #endif
    }
}

//----------------- MAIN PROCESS WIFI MANAGER --------------
int oldStatus = 0;

void wifiManagerProcess() {

    wm.process(); // avoid delays() in loop when non-blocking and other long running code

    int newStatus = WiFi.status();
    if (newStatus != oldStatus) {
        if (newStatus == WL_CONNECTED) {
            Serial.println("CONNECTED - Current ip: " + WiFi.localIP().toString());
        } else {
            Serial.print("[Error] - current status: ");
            Serial.println(newStatus);
        }
        oldStatus = newStatus;
    }
}
