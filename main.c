#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>

#define EEPROM_SIZE 512
#define DEVICE_DATA_START 0
#define WIFI_DATA_START (DEVICE_DATA_START + sizeof(DeviceData))

#define SERVER_URL "http://192.168.1.100/api/device"
#define HTTP_TIMEOUT 10000
#define WEB_SERVER_PORT 80
#define AP_SSID "USB-Device"
#define AP_PASSWORD "12345678"

U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

typedef struct {
    time_t init_timestamp;
    char user_id[32];
    char display_text[64];
    char status[16];
    char token[48];
    unsigned long timer;
    uint16_t uptime;
    uint8_t initialized;
} DeviceData;

typedef struct {
    char ssid[32];
    char password[32];
} WifiData;

DeviceData device_data;
WifiData wifi_data;
char mac_address[18];
unsigned long last_update_time = 0;
unsigned long last_save_time = 0;
unsigned long last_timer_update = 0;
ESP8266WebServer webServer(WEB_SERVER_PORT);
bool ap_mode = false;

const char html_start[] PROGMEM = R"=====(<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>USB ID</title><style>body{font-family:Arial,sans-serif;margin:0;padding:10px;max-width:600px;margin:0 auto}h1{color:#333}.btn{background-color:#4CAF50;border:none;color:white;padding:8px 16px;text-align:center;text-decoration:none;display:inline-block;font-size:14px;margin:8px 2px;cursor:pointer;border-radius:4px}.info{background-color:#f9f9f9;border-left:6px solid #2196F3;padding:8px;margin:8px 0}</style></head><body><h1>USB Identifier</h1><div class="info"><p>MAC: )=====";

const char html_mid[] PROGMEM = R"=====(</p></div><h2>Setup Device</h2><a href="/init" class="btn">Initialize</a><h2>WiFi Config</h2><form action="/configure" method="post"><p><label for="ssid">SSID:</label><br><input type="text" id="ssid" name="ssid" required></p><p><label for="password">Password:</label><br><input type="password" id="password" name="password" required></p><input type="submit" value="Save" class="btn"></form><h2>Server Settings</h2><form action="/login" method="post"><p><label for="login">Login:</label><br><input type="text" id="login" name="login" required></p><p><label for="password">Password:</label><br><input type="password" id="password" name="password" required></p><input type="submit" value="Connect" class="btn"></form></body></html>)=====";

void save_device_data();
bool load_device_data();
void save_wifi_data();
bool load_wifi_data();
void get_mac_address();
String scan_wifi_networks();
bool check_wifi_connection();
void update_display();
bool connect_to_wifi();
void setup_ap_mode();
void send_server_request();
void handle_root();
void handle_login();
void handle_init();
void handle_configure();

void save_device_data() {
    EEPROM.put(DEVICE_DATA_START, device_data);
    EEPROM.commit();
}

bool load_device_data() {
    EEPROM.get(DEVICE_DATA_START, device_data);
    return device_data.initialized;
}

void save_wifi_data() {
    EEPROM.put(WIFI_DATA_START, wifi_data);
    EEPROM.commit();
}

bool load_wifi_data() {
    EEPROM.get(WIFI_DATA_START, wifi_data);
    return wifi_data.ssid[0] != '\0';
}

void get_mac_address() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

String scan_wifi_networks() {
    int n = WiFi.scanNetworks();
    StaticJsonDocument<512> doc;
    JsonArray array = doc.to<JsonArray>();

    for (int i = 0; i < min(n, 10); ++i) {
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
    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(0, 8, device_data.display_text);
    u8g2.drawStr(0, 16, timer_str);
    u8g2.drawStr(0, 24, device_data.status);

    if (check_wifi_connection()) {
        u8g2.drawStr(0, 32, "WiFi: Connected");
    }
    else if (ap_mode) {
        u8g2.drawStr(0, 32, "WiFi: AP Mode");
    }
    else {
        u8g2.drawStr(0, 32, "WiFi: Disconnected");
    }

    u8g2.sendBuffer();
}

bool connect_to_wifi() {
    if (wifi_data.ssid[0] == '\0') {
        return false;
    }

    WiFi.begin(wifi_data.ssid, wifi_data.password);

    unsigned long start_time = millis();
    last_timer_update = start_time;

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        unsigned long current_time = millis();
        device_data.timer += (current_time - last_timer_update);
        last_timer_update = current_time;
        update_display();

        if (current_time - start_time > 15000) {
            return false;
        }
    }

    return true;
}

void setup_ap_mode() {
    String ap_name = String("USB-") + String(mac_address).substring(12);
    WiFi.softAP(ap_name.c_str(), AP_PASSWORD);
    ap_mode = true;
    Serial.println(F("AP Mode Active: ") + ap_name);
    Serial.println(F("Password: ") + String(AP_PASSWORD));

    strlcpy(device_data.display_text, ap_name.c_str(), sizeof(device_data.display_text));
    strlcpy(device_data.status, "Connect to configure", sizeof(device_data.status));
    update_display();
}

void send_server_request() {
    if (!check_wifi_connection() && !connect_to_wifi()) {
        strlcpy(device_data.status, "Wifi Error", sizeof(device_data.status));
        update_display();
        return;
    }

    WiFiClient client;
    HTTPClient http;

    StaticJsonDocument<384> doc;

    if (!device_data.initialized) {
        doc["start"] = "hello";
        doc["mac"] = mac_address;
    }
    else {
        doc["time"] = device_data.init_timestamp;
        doc["id"] = device_data.user_id;
        doc["token"] = device_data.token;
        doc["connected"] = check_wifi_connection() ? 1 : 0;

        JsonArray wifiArray = doc.createNestedArray("wifi");
        int n = WiFi.scanNetworks();
        for (int i = 0; i < min(n, 5); ++i) {
            wifiArray.add(WiFi.SSID(i));
        }
    }

    String json_data;
    serializeJson(doc, json_data);

    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT);

    int http_code = http.POST(json_data);

    if (http_code > 0 && http_code == HTTP_CODE_OK) {
        String payload = http.getString();
        StaticJsonDocument<384> response_doc;
        DeserializationError error = deserializeJson(response_doc, payload);

        if (!error) {
            if (response_doc.containsKey("time")) {
                device_data.init_timestamp = response_doc["time"].as<time_t>();
            }

            if (response_doc.containsKey("id")) {
                strlcpy(device_data.user_id, response_doc["id"] | "user", sizeof(device_data.user_id));
            }

            if (response_doc.containsKey("text")) {
                strlcpy(device_data.display_text, response_doc["text"] | "Hello", sizeof(device_data.display_text));
            }

            if (response_doc.containsKey("status")) {
                strlcpy(device_data.status, response_doc["status"] | "OK", sizeof(device_data.status));
            }

            if (response_doc.containsKey("token")) {
                strlcpy(device_data.token, response_doc["token"] | "", sizeof(device_data.token));
            }

            if (response_doc.containsKey("timer")) {
                unsigned long new_timer = response_doc["timer"].as<unsigned long>();
                device_data.timer = new_timer;
                last_timer_update = millis();
            }

            if (response_doc.containsKey("uptime")) {
                device_data.uptime = response_doc["uptime"].as<uint16_t>();
                if (device_data.uptime > 3600) device_data.uptime = 3600;
                if (device_data.uptime < 5) device_data.uptime = 5;
            }

            if (response_doc.containsKey("wifi") && response_doc.containsKey("password")) {
                String new_ssid = response_doc["wifi"].as<String>();
                String new_pass = response_doc["password"].as<String>();

                if (new_ssid.length() > 0 &&
                    (strcmp(wifi_data.ssid, new_ssid.c_str()) != 0 ||
                        strcmp(wifi_data.password, new_pass.c_str()) != 0)) {

                    strlcpy(wifi_data.ssid, new_ssid.c_str(), sizeof(wifi_data.ssid));
                    strlcpy(wifi_data.password, new_pass.c_str(), sizeof(wifi_data.password));
                    save_wifi_data();

                    WiFi.disconnect();
                    delay(500);
                    connect_to_wifi();
                }
            }

            if (!device_data.initialized) {
                device_data.initialized = 1;
            }

            save_device_data();
        }
        else {
            strlcpy(device_data.status, "JSON Error", sizeof(device_data.status));
        }
    }
    else {
        strlcpy(device_data.status, "HTTP Error", sizeof(device_data.status));
        Serial.printf("HTTP Error: %d\n", http_code);
    }

    http.end();
    update_display();
}

void handle_root() {
    String html = FPSTR(html_start) + String(mac_address) + FPSTR(html_mid);
    webServer.send(200, "text/html", html);
}

void handle_login() {
    String login = webServer.arg("login");
    String password = webServer.arg("password");

    WiFiClient client;
    HTTPClient http;

    StaticJsonDocument<256> doc;
    doc["start"] = "hello";
    doc["mac"] = mac_address;
    doc["login"] = login;
    doc["password"] = password;

    String json_data;
    serializeJson(doc, json_data);

    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT);

    int http_code = http.POST(json_data);

    if (http_code > 0 && http_code == HTTP_CODE_OK) {
        String payload = http.getString();
        StaticJsonDocument<384> response_doc;
        DeserializationError error = deserializeJson(response_doc, payload);

        if (!error) {
            device_data.init_timestamp = response_doc["time"].as<time_t>();
            strlcpy(device_data.user_id, response_doc["id"] | "unknown", sizeof(device_data.user_id));
            strlcpy(device_data.display_text, response_doc["text"] | "Hello", sizeof(device_data.display_text));
            strlcpy(device_data.status, response_doc["status"] | "OK", sizeof(device_data.status));
            strlcpy(device_data.token, response_doc["token"] | "", sizeof(device_data.token));
            device_data.timer = 0;
            device_data.uptime = 60;
            device_data.initialized = 1;

            save_device_data();
            http.end();

            webServer.sendHeader("Location", "/");
            webServer.send(303);
        }
        else {
            http.end();
            webServer.send(400, "text/plain", "Invalid JSON Response");
        }
    }
    else {
        http.end();
        webServer.send(400, "text/plain", "Connection Error");
    }
}

void handle_init() {
    device_data.initialized = 1;
    device_data.timer = 0;
    device_data.uptime = 60;
    save_device_data();
    webServer.send(200, "text/plain", "Initialization Complete");
}

void handle_configure() {
    wifi_data.ssid[0] = '\0';
    wifi_data.password[0] = '\0';
    save_wifi_data();

    webServer.send(200, "text/plain", "WiFi Configuration Reset");
}

void setup() {
    Serial.begin(115200);
    EEPROM.begin(EEPROM_SIZE);
    Wire.begin(D2, D1);
    u8g2.begin();

    get_mac_address();
    if (!load_device_data()) {
        setup_ap_mode();
    }

    webServer.on("/", HTTP_GET, handle_root);
    webServer.on("/login", HTTP_POST, handle_login);
    webServer.on("/init", HTTP_GET, handle_init);
    webServer.on("/configure", HTTP_POST, handle_configure);

    webServer.begin();
}

void loop() {
    webServer.handleClient();
    if (millis() - last_update_time > 1000) {
        last_update_time = millis();
        send_server_request();
    }
}
