#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266HTTPClient.h>
#include <Ticker.h>

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 32
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define EEPROM_SIZE 512
#define DEFAULT_SSID "ESP8266_Setup"
#define RESET_BUTTON_PIN 0
#define BATTERY_PIN A0

const char* SERVER_URL = "https://letpass.ru/?init";
const byte DNS_PORT = 53;
IPAddress apIP(1, 1, 1, 1);

Adafruit_SSD1306 display(DISPLAY_WIDTH, DISPLAY_HEIGHT, &Wire, OLED_RESET);
ESP8266WebServer webServer(80);
DNSServer dnsServer;
Ticker wifiTicker;

struct DeviceData {
    String boardID;
    String token;
    unsigned long timer;
    unsigned long uptime;
    String text;
    String status;
};

struct WiFiCredentials {
    String ssid;
    String password;
    bool connected;
};

DeviceData deviceData;
WiFiCredentials wifiCreds;
unsigned long lastConnectionAttempt = 0;
unsigned long lastServerUpdate = 0;
bool isAccessPointMode = false;
bool displayEnabled = true;

void setupDisplay();
void updateDisplay(String line1, String line2, String line3 = "");
void handleRoot();
void handleConnect();
void handleSuccess();
void handleRedirect();
void handleScan();
void handleNotFound();
void startAPMode();
void loadDeviceData();
void saveDeviceData();
void loadWiFiCredentials();
void saveWiFiCredentials(String ssid, String password);
void resetWiFiSettings();
bool connectToWiFi(String ssid, String password);
void sendDataToServer();
void sendHelloToServer();
String getWiFiSignalStrength();
String getMacAddress();
void wifiOFF();
void wifiON();

void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting up...");

    EEPROM.begin(EEPROM_SIZE);

    setupDisplay();
    updateDisplay("Starting up...", "Please wait...");

    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

    loadDeviceData();
    loadWiFiCredentials();

    if (deviceData.boardID.length() == 0) {
        deviceData.boardID = "ESP8266_" + String(ESP.getChipId(), HEX);
        deviceData.token = deviceData.boardID + "_token";
        deviceData.timer = 0;
        deviceData.uptime = 600000;
        deviceData.text = "Welcome!";
        deviceData.status = "New device";
        saveDeviceData();
    }

    if (wifiCreds.ssid.length() > 0) {
        updateDisplay("Connecting to WiFi", wifiCreds.ssid);
        if (connectToWiFi(wifiCreds.ssid, wifiCreds.password)) {
            updateDisplay("Connected to WiFi", wifiCreds.ssid, getWiFiSignalStrength());
            wifiCreds.connected = true;

            // Send initial hello message
            sendHelloToServer();
            sendDataToServer();
        }
        else {
            updateDisplay("WiFi connection", "failed", "Starting setup...");
            startAPMode();
        }
    }
    else {
        startAPMode();
    }
}

void loop() {
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        delay(50);
        if (digitalRead(RESET_BUTTON_PIN) == LOW) {
            unsigned long pressStartTime = millis();
            while (digitalRead(RESET_BUTTON_PIN) == LOW && millis() - pressStartTime < 3000) {
                delay(100);
            }

            if (millis() - pressStartTime >= 3000) {
                resetWiFiSettings();
            }
        }
    }

    float batteryVoltage = analogRead(BATTERY_PIN) * 3.3 / 1023.0 * 2;
    static bool isDataSaved = false;

    if (batteryVoltage < 3.1 && !isDataSaved) {
        saveDeviceData();
        updateDisplay("Low Battery!", "Saving data...", String(batteryVoltage, 2) + "V");
        delay(2000);
        isDataSaved = true;
    }
    else if (batteryVoltage >= 3.1) {
        isDataSaved = false;
    }

    if (isAccessPointMode) {
        dnsServer.processNextRequest();
        webServer.handleClient();
    }
    else if (wifiCreds.connected) {
        unsigned long currentMillis = millis();
        if (currentMillis - lastServerUpdate >= deviceData.uptime) {
            lastServerUpdate = currentMillis;
            sendDataToServer();
        }

        deviceData.timer += (currentMillis - lastServerUpdate);

        if (displayEnabled) {
            updateDisplay(
                deviceData.text,
                "Timer: " + String(deviceData.timer / 1000) + "s",
                deviceData.status
            );
        }

        static unsigned long lastSave = 0;
        if (currentMillis - lastSave >= 3600000) {
            lastSave = currentMillis;
            saveDeviceData();
        }

        if (WiFi.status() != WL_CONNECTED) {
            if (currentMillis - lastConnectionAttempt >= 30000) {
                lastConnectionAttempt = currentMillis;
                updateDisplay("Reconnecting...", wifiCreds.ssid);
                if (connectToWiFi(wifiCreds.ssid, wifiCreds.password)) {
                    updateDisplay("Reconnected", wifiCreds.ssid, getWiFiSignalStrength());
                }
                else {
                    updateDisplay("Reconnect failed", "Will retry...");
                }
            }
        }
    }

    delay(50);
}

void setupDisplay() {
    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        displayEnabled = false;
        return;
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.display();
    displayEnabled = true;
}

void updateDisplay(String line1, String line2, String line3) {
    if (!displayEnabled) return;

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(line1);
    display.setCursor(0, 10);
    display.println(line2);

    if (line3.length() > 0) {
        display.setCursor(0, 20);
        display.println(line3);
    }

    if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        int bars = 0;

        if (rssi > -55) bars = 4;
        else if (rssi > -65) bars = 3;
        else if (rssi > -75) bars = 2;
        else if (rssi > -85) bars = 1;

        for (int i = 0; i < bars; i++) {
            display.fillRect(DISPLAY_WIDTH - 18 + i * 4, 2 + (4 - i) * 2, 3, i * 2 + 2, SSD1306_WHITE);
        }
    }

    display.display();
}

void wifiOFF() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi turned OFF");
}

void wifiON() {
    WiFi.mode(WIFI_STA);
    Serial.println("WiFi turned ON");
}

void startAPMode() {
    wifiON();
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(DEFAULT_SSID);

    dnsServer.start(DNS_PORT, "*", apIP);

    webServer.on("/", handleRoot);
    webServer.on("/connect", HTTP_POST, handleConnect);
    webServer.on("/success", handleSuccess);
    webServer.on("/redirect", handleRedirect);
    webServer.on("/scan", handleScan);
    webServer.onNotFound(handleNotFound);
    webServer.begin();

    isAccessPointMode = true;
    Serial.println("AP Mode started");
    Serial.print("AP SSID: ");
    Serial.println(DEFAULT_SSID);

    updateDisplay("WiFi Setup Mode", "Connect: " + String(DEFAULT_SSID), "Open: 1.1.1.1");
}

void handleRoot() {
    String html = R"html(
  <!DOCTYPE html>
  <html>
  <head>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP8266 WiFi Setup</title>
    <style>
      body {
        font-family: Arial, sans-serif;
        margin: 0;
        padding: 20px;
        background: #f5f5f5;
        text-align: center;
      }
      .container {
        max-width: 400px;
        margin: 0 auto;
        background: white;
        padding: 20px;
        border-radius: 10px;
        box-shadow: 0 2px 10px rgba(0,0,0,0.1);
      }
      h1 {
        color: #333;
      }
      .form-group {
        margin-bottom: 15px;
        text-align: left;
      }
      label {
        display: block;
        margin-bottom: 5px;
        font-weight: bold;
      }
      input {
        width: 100%;
        padding: 8px;
        box-sizing: border-box;
        border: 1px solid #ddd;
        border-radius: 4px;
      }
      button {
        background: #4285f4;
        color: white;
        border: none;
        padding: 10px 15px;
        border-radius: 4px;
        cursor: pointer;
        font-weight: bold;
      }
      button:hover {
        opacity: 0.9;
      }
      #networks {
        max-height: 200px;
        overflow-y: auto;
        margin-bottom: 15px;
        border: 1px solid #ddd;
        border-radius: 4px;
      }
      .network {
        padding: 8px;
        border-bottom: 1px solid #ddd;
        cursor: pointer;
      }
      .network:hover {
        background: rgba(0,0,0,0.05);
      }
    </style>
  </head>
  <body>
    <div class="container">
      <h1>ESP8266 WiFi Setup</h1>
      <p>Please select your WiFi network and enter the password to connect the device.</p>
      
      <div id="networks">
        <p id="scanning">Scanning for networks...</p>
      </div>
      
      <form action="/connect" method="POST">
        <div class="form-group">
          <label for="ssid">Network Name (SSID):</label>
          <input type="text" id="ssid" name="ssid" required>
        </div>
        
        <div class="form-group">
          <label for="password">Password:</label>
          <input type="password" id="password" name="password">
        </div>
        
        <div class="form-group">
          <label for="redirect_url">Redirect URL (optional):</label>
          <input type="text" id="redirect_url" name="redirect_url" placeholder="https://example.com">
        </div>
        
        <button type="submit">Connect</button>
      </form>
    </div>
    
    <script>
      // Scan for networks when page loads
      window.onload = function() {
        fetchNetworks();
      };
      
      function fetchNetworks() {
        fetch('/scan')
          .then(response => response.json())
          .then(data => {
            const networksDiv = document.getElementById('networks');
            networksDiv.innerHTML = '';
            
            if (data.length === 0) {
              networksDiv.innerHTML = '<p>No networks found</p>';
              return;
            }
            
            data.forEach(network => {
              const div = document.createElement('div');
              div.className = 'network';
              div.textContent = network.ssid + ' (' + network.rssi + ' dBm)';
              div.onclick = function() {
                document.getElementById('ssid').value = network.ssid;
              };
              networksDiv.appendChild(div);
            });
          })
          .catch(error => {
            document.getElementById('networks').innerHTML = '<p>Error scanning networks</p>';
            console.error('Error:', error);
          });
      }
    </script>
  </body>
  </html>
  )html";

    webServer.send(200, "text/html", html);
}

void handleConnect() {
    String ssid = webServer.arg("ssid");
    String password = webServer.arg("password");
    String redirectUrl = webServer.arg("redirect_url");

    if (ssid.length() > 0) {
        String html = R"html(
    <!DOCTYPE html>
    <html>
    <head>
      <meta name="viewport" content="width=device-width, initial-scale=1.0">
      <title>ESP8266 WiFi Setup</title>
      <style>
        body {
          font-family: Arial, sans-serif;
          margin: 0;
          padding: 20px;
          background: #f5f5f5;
          text-align: center;
        }
        .container {
          max-width: 400px;
          margin: 0 auto;
          background: white;
          padding: 20px;
          border-radius: 10px;
          box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }
        h1 {
          color: #333;
        }
        .loader {
          border: 5px solid #f3f3f3;
          border-top: 5px solid #3498db;
          border-radius: 50%;
          width: 50px;
          height: 50px;
          animation: spin 2s linear infinite;
          margin: 20px auto;
        }
        @keyframes spin {
          0% { transform: rotate(0deg); }
          100% { transform: rotate(360deg); }
        }
      </style>
      <script>
        // Check connection status periodically
        var checkStatusInterval = setInterval(function() {
          fetch('/success')
            .then(response => {
              if (response.ok) {
                clearInterval(checkStatusInterval);
                document.getElementById('connection-status').innerHTML = '<h1>Connection Successful!</h1><p>The device is now connected to the internet.</p>';
                document.getElementById('loader').style.display = 'none';
                
                // Redirect if URL was provided
                var redirectUrl = ')html" + redirectUrl + R"html(';
                if (redirectUrl && redirectUrl.length > 0) {
                  setTimeout(function() {
                    window.location.href = redirectUrl;
                  }, 3000);
                  document.getElementById('redirect-msg').innerHTML = 'Redirecting to ' + redirectUrl + ' in 3 seconds...';
                }
              }
            })
            .catch(error => {
              // Connection attempt still in progress
            });
        }, 2000);
      </script>
    </head>
    <body>
      <div class="container" id="connection-status">
        <h1>Connecting...</h1>
        <div class="loader" id="loader"></div>
        <p>Attempting to connect to <strong>)html" + ssid + R"html(</strong></p>
        <p>The device will restart if connection is successful.</p>
        <p>If connection fails, the device will return to setup mode.</p>
      </div>
      <p id="redirect-msg"></p>
    </body>
    </html>
    )html";

        webServer.send(200, "text/html", html);

        // Process the connection in a non-blocking way if possible
        saveWiFiCredentials(ssid, password);
        updateDisplay("Connecting to", ssid, "Please wait...");

        if (connectToWiFi(ssid, password)) {
            wifiCreds.connected = true;
            // Don't stop AP mode yet - let the user see the success page

            updateDisplay("Connected to", ssid, getWiFiSignalStrength());

            // Send initial hello message
            sendHelloToServer();
            sendDataToServer();

            // Keep the AP active for a while so the user can see success
            delay(5000);

            // Now we can stop AP mode
            isAccessPointMode = false;
            dnsServer.stop();
            webServer.stop();
            WiFi.mode(WIFI_STA);
        }
        else {
            updateDisplay("Connection Failed", "Restarting setup", "");
            delay(3000);
            ESP.restart();
        }
    }
    else {
        webServer.sendHeader("Location", "/", true);
        webServer.send(302, "text/plain", "");
    }
}

void handleSuccess() {
    if (WiFi.status() == WL_CONNECTED) {
        webServer.send(200, "text/plain", "connected");
    }
    else {
        webServer.send(503, "text/plain", "not connected");
    }
}

void handleRedirect() {
    String redirectUrl = webServer.arg("url");
    if (redirectUrl.length() > 0) {
        webServer.sendHeader("Location", redirectUrl, true);
        webServer.send(302, "text/plain", "");
    }
    else {
        webServer.send(400, "text/plain", "No URL provided");
    }
}

void handleScan() {
    String json = "[";
    int n = WiFi.scanNetworks();

    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }

    json += "]";
    webServer.send(200, "application/json", json);
}

void handleNotFound() {
    webServer.sendHeader("Location", "http://1.1.1.1/", true);
    webServer.send(302, "text/plain", "");
}

bool connectToWiFi(String ssid, String password) {
    wifiON();

    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), password.c_str());

    int timeout = 20;
    while (WiFi.status() != WL_CONNECTED && timeout > 0) {
        delay(1000);
        Serial.print(".");
        timeout--;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nConnected to WiFi!");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        return true;
    }
    else {
        Serial.println("\nFailed to connect to WiFi");
        return false;
    }
}

String getMacAddress() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[18] = { 0 };
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

void sendHelloToServer() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    WiFiClient client;
    HTTPClient http;

    Serial.println("Sending hello to server...");
    updateDisplay(deviceData.text, "Sending hello...", deviceData.status);

    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> jsonDoc;
    jsonDoc["id"] = deviceData.boardID;
    jsonDoc["token"] = deviceData.token;
    jsonDoc["mac"] = getMacAddress();
    jsonDoc["hello"] = "Hello from ESP8266";
    jsonDoc["time"] = millis();

    String jsonString;
    serializeJson(jsonDoc, jsonString);

    int httpResponseCode = http.POST(jsonString);
    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("Hello response: " + response);
    }
    else {
        Serial.println("Hello HTTP error: " + http.errorToString(httpResponseCode));
    }

    http.end();
}

void sendDataToServer() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    WiFiClient client;
    HTTPClient http;

    Serial.println("Sending data to server...");
    updateDisplay(deviceData.text, "Sending data...", deviceData.status);

    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> jsonDoc;
    jsonDoc["id"] = deviceData.boardID;
    jsonDoc["token"] = deviceData.token;
    jsonDoc["mac"] = getMacAddress();
    jsonDoc["timer"] = deviceData.timer;
    jsonDoc["time"] = millis();

    String jsonString;
    serializeJson(jsonDoc, jsonString);

    int retry = 0;
    int httpResponseCode = 0;

    while (retry < 3 && httpResponseCode <= 0) {
        httpResponseCode = http.POST(jsonString);
        if (httpResponseCode <= 0) {
            Serial.println("HTTP error: " + http.errorToString(httpResponseCode));
            retry++;
            delay(1000);
        }
    }

    if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.println("Server response: " + response);

        StaticJsonDocument<512> responseDoc;
        DeserializationError error = deserializeJson(responseDoc, response);

        if (!error) {
            if (responseDoc.containsKey("text")) {
                deviceData.text = responseDoc["text"].as<String>();
            }

            if (responseDoc.containsKey("status")) {
                deviceData.status = responseDoc["status"].as<String>();
            }

            if (responseDoc.containsKey("uptime")) {
                deviceData.uptime = responseDoc["uptime"].as<unsigned long>();
            }

            // Only save device data once
            saveDeviceData();
        }
    }

    http.end();
}

void resetWiFiSettings() {
    Serial.println("Resetting WiFi settings...");
    updateDisplay("Resetting WiFi", "Please wait...");

    for (int i = 100; i < 250; i++) {
        EEPROM.write(i, 0);
    }
    EEPROM.commit();

    delay(1000);
    updateDisplay("WiFi Reset", "Rebooting...");
    delay(1000);

    ESP.restart();
}

void loadDeviceData() {
    String dataString = "";
    for (int i = 0; i < 100; i++) {
        char c = EEPROM.read(i);
        if (c == 0) break;
        dataString += c;
    }

    if (dataString.length() > 0) {
        StaticJsonDocument<512> jsonDoc;
        DeserializationError error = deserializeJson(jsonDoc, dataString);

        if (!error) {
            deviceData.boardID = jsonDoc["boardID"].as<String>();
            deviceData.token = jsonDoc["token"].as<String>();
            deviceData.timer = jsonDoc["timer"].as<unsigned long>();
            deviceData.uptime = jsonDoc["uptime"].as<unsigned long>();
            deviceData.text = jsonDoc["text"].as<String>();
            deviceData.status = jsonDoc["status"].as<String>();
        }
    }
}

void saveDeviceData() {
    // To prevent double saving, add a static timestamp
    static unsigned long lastSaveTime = 0;
    unsigned long currentTime = millis();

    // Only save if at least 1 second has passed since the last save
    if (currentTime - lastSaveTime < 1000) {
        return;
    }
    lastSaveTime = currentTime;

    StaticJsonDocument<512> jsonDoc;
    jsonDoc["boardID"] = deviceData.boardID;
    jsonDoc["token"] = deviceData.token;
    jsonDoc["timer"] = deviceData.timer;
    jsonDoc["uptime"] = deviceData.uptime;
    jsonDoc["text"] = deviceData.text;
    jsonDoc["status"] = deviceData.status;

    String jsonString;
    serializeJson(jsonDoc, jsonString);

    for (int i = 0; i < 100; i++) {
        EEPROM.write(i, 0);
    }

    for (int i = 0; i < jsonString.length() && i < 100; i++) {
        EEPROM.write(i, jsonString[i]);
    }

    EEPROM.commit();
    Serial.println("Device data saved");
}

void loadWiFiCredentials() {
    String dataString = "";
    for (int i = 100; i < 250; i++) {
        char c = EEPROM.read(i);
        if (c == 0) break;
        dataString += c;
    }

    if (dataString.length() > 0) {
        int separatorIndex = dataString.indexOf('|');
        if (separatorIndex > 0) {
            wifiCreds.ssid = dataString.substring(0, separatorIndex);
            wifiCreds.password = dataString.substring(separatorIndex + 1);
            wifiCreds.connected = false;
        }
    }
}

void saveWiFiCredentials(String ssid, String password) {
    String data = ssid + "|" + password;

    for (int i = 100; i < 250; i++) {
        EEPROM.write(i, 0);
    }

    for (int i = 0; i < data.length() && i < 150; i++) {
        EEPROM.write(i + 100, data[i]);
    }

    EEPROM.commit();
    Serial.println("WiFi credentials saved");

    wifiCreds.ssid = ssid;
    wifiCreds.password = password;
}

String getWiFiSignalStrength() {
    if (WiFi.status() != WL_CONNECTED) {
        return "No WiFi";
    }

    int rssi = WiFi.RSSI();
    String signalStrength = "";

    if (rssi > -50) {
        signalStrength = "Excellent";
    }
    else if (rssi > -60) {
        signalStrength = "Good";
    }
    else if (rssi > -70) {
        signalStrength = "Fair";
    }
    else {
        signalStrength = "Weak";
    }

    return signalStrength + " (" + String(rssi) + " dBm)";
}