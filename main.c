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

#define SERVER_URL "http://192.168.1.100/api/device"
#define HTTP_TIMEOUT 10000
#define WEB_SERVER_PORT 80

#define AP_SSID "USB-Device"
#define AP_PASSWORD "12345678"

typedef struct {
    char ssid[64];
    char password[64];
} WifiData;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
WifiData wifi_data;
ESP8266WebServer webServer(WEB_SERVER_PORT);
bool ap_mode = false;

void save_wifi_data() {
    EEPROM.put(WIFI_DATA_START, wifi_data);
    EEPROM.commit();
}

bool load_wifi_data() {
    EEPROM.get(WIFI_DATA_START, wifi_data);
    return (strlen(wifi_data.ssid) > 0);
}

void setup_ap_mode() {
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    ap_mode = true;
    Serial.println("AP Mode Active");
}

void handle_root() {
    String html = "<!DOCTYPE html><html><head><title>Login</title></head><body>"
        "<h1>Device Authorization</h1>"
        "<form action='/login' method='post'>"
        "Login: <input type='text' name='login'><br>"
        "Password: <input type='password' name='password'><br>"
        "<input type='submit' value='Authorize'>"
        "</form></body></html>";
    webServer.send(200, "text/html", html);
}

void handle_login() {
    String login = webServer.arg("login");
    String password = webServer.arg("password");

    WiFiClient client;
    HTTPClient http;
    DynamicJsonDocument doc(512);
    doc["login"] = login;
    doc["password"] = password;
    String json_data;
    serializeJson(doc, json_data);

    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT);
    int http_code = http.POST(json_data);

    if (http_code == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument response_doc(512);
        deserializeJson(response_doc, payload);

        strlcpy(wifi_data.ssid, response_doc["ssid"], sizeof(wifi_data.ssid));
        strlcpy(wifi_data.password, response_doc["password"], sizeof(wifi_data.password));
        save_wifi_data();

        http.end();
        webServer.send(200, "text/plain", "Success! Rebooting...");
        delay(2000);
        ESP.restart();
    }
    else {
        http.end();
        webServer.send(403, "text/plain", "Authorization Failed");
        Serial.printf("HTTP Error: %d\n", http_code);
    }
}

void setup() {
    Serial.begin(115200);
    EEPROM.begin(EEPROM_SIZE);

    if (!load_wifi_data()) {
        setup_ap_mode();
    }
    else {
        WiFi.begin(wifi_data.ssid, wifi_data.password);

        unsigned long startTime = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
            delay(500);
        }

        if (WiFi.status() != WL_CONNECTED) {
            setup_ap_mode();
        }
    }

    webServer.on("/", HTTP_GET, handle_root);
    webServer.on("/login", HTTP_POST, handle_login);
    webServer.begin();
}

void loop() {
    webServer.handleClient();
}
