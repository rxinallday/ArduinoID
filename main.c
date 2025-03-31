#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <time.h>

#define EEPROM_SIZE 512
#define DEVICE_DATA_START 0
#define WIFI_DATA_START (DEVICE_DATA_START + sizeof(DeviceData))

#define SERVER_URL "http://example.com/api/device"
#define HTTP_TIMEOUT 10000

#define WEB_SERVER_PORT 80

typedef struct {
    time_t init_timestamp;
    char user_id[64];
    char display_text[128];
    char status[32];
    char token[64];
    unsigned long timer;
    int uptime;
    bool initialized;
} DeviceData;

typedef struct {
    char ssid[64];
    char password[64];
} WifiData;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
DeviceData device_data;
WifiData wifi_data;
char mac_address[18];
unsigned long last_update_time = 0;
unsigned long last_save_time = 0;
unsigned long last_timer_update = 0;
ESP8266WebServer webServer(WEB_SERVER_PORT);
bool ap_mode = false;

const char* html_start = R"=====(<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>USB Identifier Device</title><style>body { font-family: Arial, sans-serif; margin: 0; padding: 20px; max-width: 600px; margin: 0 auto; } h1 { color: #333; } .btn { background-color: #4CAF50; border: none; color: white; padding: 10px 20px; text-align: center; text-decoration: none; display: inline-block; font-size: 16px; margin: 10px 2px; cursor: pointer; border-radius: 4px; } .info { background-color: #f9f9f9; border-left: 6px solid #2196F3; padding: 10px; margin: 10px 0; }</style></head><body><h1>USB Identifier Device</h1><div class="info"><p>MAC Address: )=====";

const char* html_mid = R"=====(</p></div><h2>Initialize Device</h2><p>Click the button below to initialize the device:</p><a href="/init" class="btn">Initialize Device</a><h2>Configure Wi-Fi</h2><form action="/configure" method="post"><p><label for="ssid">Wi-Fi SSID:</label><br><input type="text" id="ssid" name="ssid" required></p><p><label for="password">Wi-Fi Password:</label><br><input type="password" id="password" name="password" required></p><input type="submit" value="Configure Wi-Fi" class="btn"></form></body></html>)=====";

void save_device_data() {
    for (size_t i = 0; i < sizeof(DeviceData); i++) {
        EEPROM.write(DEVICE_DATA_START + i, ((uint8_t*)&device_data)[i]);
    }
    EEPROM.commit();
}

bool load_device_data() {
    for (size_t i = 0; i < sizeof(DeviceData); i++) {
        ((uint8_t*)&device_data)[i] = EEPROM.read(DEVICE_DATA_START + i);
    }
    return device_data.initialized;
}

void save_wifi_data() {
    for (size_t i = 0; i < sizeof(WifiData); i++) {
        EEPROM.write(WIFI_DATA_START + i, ((uint8_t*)&wifi_data)[i]);
    }
    EEPROM.commit();
}

bool load_wifi_data() {
    for (size_t i = 0; i < sizeof(WifiData); i++) {
        ((uint8_t*)&wifi_data)[i] = EEPROM.read(WIFI_DATA_START + i);
    }
    return (strlen(wifi_data.ssid) > 0);
}

void get_mac_address() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

String scan_wifi_networks() {
    Serial.println("Scanning Wi-Fi networks...");
    int n = WiFi.scanNetworks();
    DynamicJsonDocument doc(2048);
    JsonArray array = doc.to<JsonArray>();

    for (int i = 0; i < n; ++i) {
        array.add(WiFi.SSID(i));
    }

    String result;
    serializeJson(array, result);
    return result;
}

bool check_wifi_connection() {
    return WiFi.status() == WL_CONNECTED;
}

void update_display() {
    char timer_str[13];
    snprintf(timer_str, sizeof(timer_str), "%012lu", device_data.timer);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    u8g2.drawStr(0, 10, device_data.display_text);
    u8g2.drawStr(0, 25, timer_str);
    u8g2.drawStr(0, 40, device_data.status);

    if (check_wifi_connection()) {
        u8g2.drawStr(0, 55, "WiFi: Connected");
    }
    else if (ap_mode) {
        u8g2.drawStr(0, 55, "WiFi: AP Mode");
    }
    else {
        u8g2.drawStr(0, 55, "WiFi: Disconnected");
    }

    u8g2.sendBuffer();
}

bool connect_to_wifi() {
    if (strlen(wifi_data.ssid) == 0) {
        return false;
    }

    WiFi.begin(wifi_data.ssid, wifi_data.password);

    unsigned long start_time = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        unsigned long current_time = millis();
        device_data.timer += (current_time - last_timer_update);
        last_timer_update = current_time;

        if (current_time - start_time > 20000) {
            return false;
        }
    }

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    return true;
}

void setup_ap_mode() {
    String ap_name = "USB-ID-" + String(mac_address).substring(9);

    WiFi.softAP(ap_name.c_str(), "12345678");

    ap_mode = true;

    strcpy(device_data.status, "AP Mode");
    update_display();
}

bool send_initial_request() {
    if (!check_wifi_connection()) {
        return false;
    }

    WiFiClient client;
    HTTPClient http;

    DynamicJsonDocument doc(512);
    doc["start"] = "hello";
    doc["mac"] = mac_address;

    String json_data;
    serializeJson(doc, json_data);

    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT);

    int http_code = http.POST(json_data);

    if (http_code > 0) {
        if (http_code == HTTP_CODE_OK) {
            String payload = http.getString();

            DynamicJsonDocument response_doc(512);
            DeserializationError error = deserializeJson(response_doc, payload);

            if (error) {
                http.end();
                return false;
            }

            device_data.init_timestamp = response_doc["time"].as<time_t>();
            strlcpy(device_data.user_id, response_doc["id"] | "unknown", sizeof(device_data.user_id));
            strlcpy(device_data.display_text, response_doc["text"] | "Hello", sizeof(device_data.display_text));
            strlcpy(device_data.status, response_doc["status"] | "OK", sizeof(device_data.status));
            strlcpy(device_data.token, response_doc["token"] | "", sizeof(device_data.token));
            device_data.timer = 0;
            device_data.uptime = 60;
            device_data.initialized = true;

            save_device_data();

            http.end();
            return true;
        }
    }
    http.end();
    return false;
}

bool send_periodic_request() {
    if (!check_wifi_connection()) {
        return false;
    }

    WiFiClient client;
    HTTPClient http;

    DynamicJsonDocument doc(4096);
    doc["time"] = device_data.init_timestamp;
    doc["id"] = device_data.user_id;
    doc["token"] = device_data.token;

    String networks = scan_wifi_networks();
    JsonArray wifi_array = doc.createNestedArray("wifi");
    DynamicJsonDocument networks_doc(2048);
    deserializeJson(networks_doc, networks);
    for (JsonVariant v : networks_doc.as<JsonArray>()) {
        wifi_array.add(v.as<String>());
    }

    doc["connected"] = check_wifi_connection() ? 1 : 0;

    String json_data;
    serializeJson(doc, json_data);

    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT);

    int http_code = http.POST(json_data);

    if (http_code > 0) {
        if (http_code == HTTP_CODE_OK) {
            String payload = http.getString();

            DynamicJsonDocument response_doc(512);
            DeserializationError error = deserializeJson(response_doc, payload);

            if (error) {
                http.end();
                return false;
            }

            device_data.timer = 0;
            device_data.uptime = 60;
            device_data.initialized = true;

            http.end();
            return true;
        }
    }

    http.end();
    return false;
}

void setup() {
    Serial.begin(115200);
    EEPROM.begin(EEPROM_SIZE);

    if (!load_device_data()) {
        setup_ap_mode();
        return;
    }

    get_mac_address();
    load_wifi_data();

    u8g2.begin();

    WiFi.mode(WIFI_STA);
    if (!connect_to_wifi()) {
        setup_ap_mode();
    }
    else {
        send_initial_request();
    }

    webServer.on("/", HTTP_GET, []() {
        String html = html_start + String(mac_address) + html_mid;
        webServer.send(200, "text/html", html);
        });

    webServer.on("/init", HTTP_GET, []() {
        device_data.initialized = true;
        save_device_data();
        webServer.sendHeader("Location", "/");
        webServer.send(303);
        });

    webServer.on("/configure", HTTP_POST, []() {
        String ssid = webServer.arg("ssid");
        String password = webServer.arg("password");

        strlcpy(wifi_data.ssid, ssid.c_str(), sizeof(wifi_data.ssid));
        strlcpy(wifi_data.password, password.c_str(), sizeof(wifi_data.password));

        save_wifi_data();
        connect_to_wifi();

        webServer.sendHeader("Location", "/");
        webServer.send(303);
        });

    webServer.begin();
}

void loop() {
    webServer.handleClient();

    unsigned long current_time = millis();
    if (current_time - last_update_time >= 1000) {
        last_update_time = current_time;
        device_data.uptime--;
        update_display();
    }

    if (current_time - last_save_time >= 60000) {
        last_save_time = current_time;
        save_device_data();
    }

    if (current_time - device_data.timer >= 10000) {
        device_data.timer += 10;
    }
}
