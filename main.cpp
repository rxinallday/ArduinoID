#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <LittleFS.h>
#include <Ticker.h>

#define DISPLAY_WIDTH 128
#define DISPLAY_HEIGHT 32
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define RESET_BUTTON_PIN 0
#define BATTERY_PIN A0
#define SDA 4
#define SCL 5

String SERVER_URL = "https://letpass.ru/?init";
const char* DEFAULT_SSID = "ESP8266_Setup";
const byte DNS_PORT = 53;
const int WIFI_RECONNECT_INTERVAL = 10000;
const int SERVER_UPDATE_DEFAULT = 600000;
const int WIFI_CONNECTION_TIMEOUT = 20000;

IPAddress apIP(192, 168, 4, 1);

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
    String user;
    String serverUrl;
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
unsigned long lastDisplayUpdate = 0;
bool isAccessPointMode = false;
bool displayEnabled = true;
String lastDisplayLine1 = "";
String lastDisplayLine2 = "";
String lastDisplayLine3 = "";
unsigned long lastWifiScan = 0;
bool firstBoot = true;
bool waitingForCredentialsVerification = false;
String pendingRedirectUrl = "";
unsigned long credentialsVerificationStartTime = 0;
int connectionFailCount = 0;

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
void sendDataToServer(bool isHello = false);
String getWiFiSignalStrength();
String getMacAddress();
void formatFS();
void centerText(String text, int y);
void exitAPMode();
void checkCredentialsVerification();

void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting up...");

    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

    ESP.getFreeHeap();

    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed. Formatting...");
        formatFS();
    }

    setupDisplay();
    if (displayEnabled) {
        updateDisplay("Starting up...", "Please wait...");
    }
    else {
        Serial.println("WARNING: Display initialization failed!");
    }

    loadDeviceData();
    loadWiFiCredentials();

    if (deviceData.boardID.length() == 0) {
        deviceData.boardID = "ESP8266_" + String(ESP.getChipId(), HEX);
        deviceData.token = deviceData.boardID + "_token";
        deviceData.timer = 0;
        deviceData.uptime = SERVER_UPDATE_DEFAULT;
        deviceData.text = "Welcome!";
        deviceData.status = "New device";
        deviceData.user = "";
        deviceData.serverUrl = SERVER_URL;
        saveDeviceData();
        Serial.println("Created new device data with ID: " + deviceData.boardID);
    }
    else {
        if (deviceData.serverUrl.length() > 0) {
            SERVER_URL = deviceData.serverUrl;
            Serial.println("Using saved server URL: " + SERVER_URL);
        }
    }

    if (wifiCreds.ssid.length() > 0) {
        updateDisplay("Connecting to WiFi", wifiCreds.ssid);
        if (connectToWiFi(wifiCreds.ssid, wifiCreds.password)) {
            updateDisplay("Connected to WiFi", wifiCreds.ssid, getWiFiSignalStrength());
            wifiCreds.connected = true;
            saveWiFiCredentials(wifiCreds.ssid, wifiCreds.password);

            updateDisplay("Please wait", "Registering to server...", "WiFi " + wifiCreds.ssid + " connected");

            sendDataToServer(true);
        }
        else {
            updateDisplay("WiFi connection", "failed", "Starting setup...");
            delay(2000);
            startAPMode();
        }
    }
    else {
        startAPMode();
    }

    Serial.println("Setup complete");
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    Serial.println("Device ID: " + deviceData.boardID);
    Serial.println("Server URL: " + SERVER_URL);
    Serial.println("WiFi SSID: " + wifiCreds.ssid);
    Serial.println("WiFi connected: " + String(wifiCreds.connected ? "Yes" : "No"));
}

void loop() {
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        unsigned long pressStart = millis();
        while (digitalRead(RESET_BUTTON_PIN) == LOW) {
            delay(10);
        }
        if (millis() - pressStart > 3000) {
            resetWiFiSettings();
        }
    }

    // Мониторим то чего нет)))))
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

    unsigned long currentMillis = millis();

    if (isAccessPointMode) {
        dnsServer.processNextRequest();
        webServer.handleClient();

        if (waitingForCredentialsVerification) {
            checkCredentialsVerification();
        }

        if (currentMillis - lastWifiScan >= 10000) {
            lastWifiScan = currentMillis;
            WiFi.scanNetworksAsync([](int networksFound) {
                Serial.printf("Scan completed, found %d networks\n", networksFound);
                }, true);
        }
    }
    else if (WiFi.status() == WL_CONNECTED) {
        wifiCreds.connected = true;

        if (currentMillis - lastServerUpdate >= deviceData.uptime) {
            lastServerUpdate = currentMillis;
            updateDisplay("Please wait", "Updating data...", "WiFi " + wifiCreds.ssid + " connected");
            sendDataToServer(false);
        }

        deviceData.timer = millis();

        if (currentMillis - lastDisplayUpdate >= 1000) {
            lastDisplayUpdate = currentMillis;
            updateDisplay(
                deviceData.text,
                "Time: " + String(millis()),
                "Status: " + deviceData.status
            );
        }

        static unsigned long lastSave = 0;
        if (currentMillis - lastSave >= 3600000) {
            lastSave = currentMillis;
            saveDeviceData();
        }
    }
    else if (wifiCreds.ssid.length() > 0) {
        if (currentMillis - lastConnectionAttempt >= WIFI_RECONNECT_INTERVAL) {
            lastConnectionAttempt = currentMillis;
            updateDisplay("Reconnecting...", wifiCreds.ssid, "WiFi disconnected");
            Serial.println("Attempting to reconnect to WiFi: " + wifiCreds.ssid);

            if (connectToWiFi(wifiCreds.ssid, wifiCreds.password)) {
                updateDisplay("Reconnected", wifiCreds.ssid, "WiFi connected");
                wifiCreds.connected = true;

                sendDataToServer(true);
                connectionFailCount = 0;
            }
            else {
                updateDisplay("Reconnect failed", "Will retry...", "WiFi disconnected");
                connectionFailCount++;
                Serial.println("Reconnection failed. Attempt: " + String(connectionFailCount));

                if (connectionFailCount >= 3) {
                    Serial.println("Multiple reconnection failures. Starting AP mode.");
                    connectionFailCount = 0;
                    startAPMode();
                }
            }
        }
    }

    static unsigned long lastDisplayCheck = 0;
    if (millis() - lastDisplayCheck > 60000) {
        lastDisplayCheck = millis();
        if (!displayEnabled) {
            Serial.println("Attempting to reinitialize display...");
            setupDisplay();
            if (displayEnabled) {
                updateDisplay("Display reinitialized", "System running",
                    WiFi.status() == WL_CONNECTED ? "WiFi connected" : "WiFi disconnected");
            }
        }
    }

    delay(50);
}


void checkCredentialsVerification() {
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Successfully connected to WiFi: " + wifiCreds.ssid);
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());

        waitingForCredentialsVerification = false;
        wifiCreds.connected = true;
        saveWiFiCredentials(wifiCreds.ssid, wifiCreds.password);
        updateDisplay("Connected to WiFi", wifiCreds.ssid, getWiFiSignalStrength());

        sendDataToServer(true);

        if (pendingRedirectUrl.length() > 0) {
            Serial.println("Keeping AP for redirect to: " + pendingRedirectUrl);
            static unsigned long redirectStartTime = millis();
            if (millis() - redirectStartTime > 60000) {
                exitAPMode();
            }
        }
        else {
            static unsigned long successTime = millis();
            if (millis() - successTime >= 5000) {
                exitAPMode();
            }
        }
    }
    else if ((millis() - credentialsVerificationStartTime) >= WIFI_CONNECTION_TIMEOUT) {
        Serial.println("Connection attempt timed out");
        waitingForCredentialsVerification = false;
        WiFi.disconnect();
        connectionFailCount++;

        updateDisplay("WiFi Failed", "Please try again", "Check credentials");

        if (connectionFailCount >= 3) {
            Serial.println("Multiple connection failures - check AP functionality");
            WiFi.disconnect();
            WiFi.mode(WIFI_AP_STA);
            WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
            WiFi.softAP(DEFAULT_SSID);
            connectionFailCount = 0;
        }
    }
}

void exitAPMode() {
    if (isAccessPointMode) {
        Serial.println("Exiting AP mode, continuing in station mode only");
        isAccessPointMode = false;
        dnsServer.stop();
        webServer.stop();

        WiFi.mode(WIFI_STA);

        updateDisplay("Connected to WiFi", wifiCreds.ssid, "AP mode disabled");

        pendingRedirectUrl = "";
        connectionFailCount = 0;
    }
}

void setupDisplay() {
    Wire.begin(SDA, SCL);

    lastDisplayLine1 = "";
    lastDisplayLine2 = "";
    lastDisplayLine3 = "";

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
        displayEnabled = false;
        return;
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Initializing...");
    display.display();

    Serial.println(F("SSD1306 initialization successful"));
    displayEnabled = true;
}

void updateDisplay(String line1, String line2, String line3 = "") {
    if (!displayEnabled) return;

    lastDisplayLine1 = line1;
    lastDisplayLine2 = line2;
    lastDisplayLine3 = line3;

    display.clearDisplay();

    if (line1.length() > 21) line1 = line1.substring(0, 18) + "...";
    if (line2.length() > 21) line2 = line2.substring(0, 18) + "...";
    if (line3.length() > 21) line3 = line3.substring(0, 18) + "...";

    centerText(line1, 0);
    centerText(line2, 11);

    if (line3.length() > 0) {
        centerText(line3, 22);
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

    float batteryVoltage = analogRead(BATTERY_PIN) * 3.3 / 1023.0 * 2;
    int batteryLevel = map(batteryVoltage * 100, 320, 420, 0, 100);
    batteryLevel = constrain(batteryLevel, 0, 100);

    display.drawRect(2, 2, 12, 6, SSD1306_WHITE);
    display.drawRect(14, 3, 2, 4, SSD1306_WHITE);
    display.fillRect(2, 2, map(batteryLevel, 0, 100, 0, 12), 6, SSD1306_WHITE);

    display.display();

    delay(10);
}

void centerText(String text, int y) {
    int16_t x1, y1;
    uint16_t w, h;

    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

    display.setCursor((DISPLAY_WIDTH - w) / 2, y);

    display.println(text);
}

void startAPMode() {
    WiFi.disconnect(true);
    delay(500);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(DEFAULT_SSID);

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
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

    updateDisplay(
        "Please connect to WiFi:",
        DEFAULT_SSID,
        "Then visit: setup portal"
    );

    lastWifiScan = millis() - 10000;
    WiFi.scanNetworksAsync([](int networksFound) {
        Serial.printf("Initial scan completed, found %d networks\n", networksFound);
        }, true);
}

void handleRoot() {
    String html = R"html(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <meta charset="UTF-8">
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
    .signal-strength {
      float: right;
      color: #666;
    }
    #refresh-btn {
      margin-bottom: 10px;
      background: #34a853;
    }
    #scanning {
      padding: 15px;
      color: #666;
    }
    .status {
      padding: 10px;
      margin-top: 10px;
      border-radius: 4px;
      display: none;
    }
    .error {
      background-color: #ffebee;
      color: #c62828;
      border: 1px solid #ef9a9a;
    }
    .success {
      background-color: #e8f5e9;
      color: #2e7d32;
      border: 1px solid #a5d6a7;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>ESP8266 WiFi Setup</h1>
    <p>Please select your WiFi network and enter the password to connect the device.</p>
    
    <button id="refresh-btn" onclick="fetchNetworks()">Refresh Networks</button>
    
    <div id="networks">
      <p id="scanning">Scanning for networks...</p>
    </div>
    
    <form id="wifi-form" onsubmit="return submitForm()">
      <div class="form-group">
        <label for="ssid">Network Name (SSID):</label>
        <input type="text" id="ssid" name="ssid" required>
      </div>
      
      <div class="form-group">
        <label for="password">Password:</label>
        <input type="password" id="password" name="password">
      </div>
      
      <div class="form-group">
        <label for="redirect_url">Redirect URL:</label>
        <input type="text" id="redirect_url" name="redirect_url" placeholder="https://zalupa.online">
      </div>
      
      <button type="submit">Connect</button>
    </form>
    
    <div id="status-message" class="status"></div>
  </div>
  
  <script>
    window.onload = function() {
      fetchNetworks();
    };
    
    function fetchNetworks() {
      document.getElementById('scanning').textContent = 'Scanning for networks...';
      
      fetch('/scan')
        .then(response => {
          if (!response.ok) {
            throw new Error('Network scan failed');
          }
          return response.json();
        })
        .then(data => {
          const networksDiv = document.getElementById('networks');
          networksDiv.innerHTML = '';
          
          if (!data || data.length === 0) {
            networksDiv.innerHTML = '<p id="scanning">No networks found. Try refreshing...</p>';
            return;
          }

          data.sort((a, b) => b.rssi - a.rssi);
          
          data.forEach(network => {
            if (network.ssid && network.ssid.length > 0) {  // Only show networks with SSID
              const div = document.createElement('div');
              div.className = 'network';

              let signalBars = '';
              const rssi = network.rssi;
              if (rssi > -55) signalBars = '●●●●';
              else if (rssi > -65) signalBars = '●●●○';
              else if (rssi > -75) signalBars = '●●○○';
              else if (rssi > -85) signalBars = '●○○○';
              else signalBars = '○○○○';
              
              div.innerHTML = network.ssid + '<span class="signal-strength">' + signalBars + ' ' + rssi + ' dBm</span>';
              div.onclick = function() {
                document.getElementById('ssid').value = network.ssid;
                document.getElementById('password').focus();
              };
              networksDiv.appendChild(div);
            }
          });
        })
        .catch(error => {
          document.getElementById('networks').innerHTML = '<p id="scanning">Error scanning networks. Retrying...</p>';
          console.error('Error:', error);
        });
    }
    
    function submitForm() {
      const ssid = document.getElementById('ssid').value;
      if(!ssid) {
        showStatus('Please select a network', 'error');
        return false;
      }
      
      const statusDiv = document.getElementById('status-message');
      statusDiv.className = 'status';
      statusDiv.style.display = 'block';
      statusDiv.textContent = 'Connecting to ' + ssid + '...';
      
      const formData = new FormData(document.getElementById('wifi-form'));
      
      fetch('/connect', {
        method: 'POST',
        body: new URLSearchParams(formData)
      })
      .then(response => response.text())
      .then(data => {
        checkConnectionStatus();
      })
      .catch(error => {
        showStatus('Error connecting: ' + error, 'error');
      });
      
      return false;
    }
    
    let connectionCheckCount = 0;
    
    function checkConnectionStatus() {
      connectionCheckCount = 0;
      showStatus('Attempting to connect...', '');
      
      const statusCheck = setInterval(function() {
        connectionCheckCount++;
        
        fetch('/success')
        .then(response => response.text())
        .then(data => {
          if(data === "connected") {
            clearInterval(statusCheck);
            showStatus('Connection successful!', 'success');

            const redirectUrl = document.getElementById('redirect_url').value;
            if(redirectUrl && redirectUrl.length > 0) {
              showStatus('Redirecting to ' + redirectUrl + ' in 3 seconds...', 'success');
              setTimeout(function() {
                window.location.href = redirectUrl;
              }, 3000);
            }
          } else if(data === "connecting") {
            showStatus('Still connecting... please wait', '');
          } else {
            showStatus('Checking connection status...', '');
          }
        })
        .catch(error => {
          showStatus('Connection may have succeeded. If this page disconnects, the device has connected to your network.', '');

          if (connectionCheckCount > 15) {
            clearInterval(statusCheck);
          }
        });
      }, 1000);

      setTimeout(function() {
        clearInterval(statusCheck);
        showStatus('Connection attempt timed out. Please check your password and try again.', 'error');
      }, 30000);
    }
    
    function showStatus(message, type) {
      const statusDiv = document.getElementById('status-message');
      statusDiv.textContent = message;
      statusDiv.className = 'status ' + type;
      statusDiv.style.display = 'block';
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
        saveWiFiCredentials(ssid, password);

        pendingRedirectUrl = redirectUrl;

        WiFi.disconnect(true);
        delay(500);

        waitingForCredentialsVerification = true;
        credentialsVerificationStartTime = millis();
        lastConnectionAttempt = millis();

        updateDisplay("Connecting to", ssid, "Please wait...");

        Serial.println("Attempting to connect to: " + ssid);

        WiFi.mode(WIFI_AP_STA);
        WiFi.begin(ssid.c_str(), password.c_str());

        webServer.send(200, "text/plain", "Attempting to connect to " + ssid);
    }
    else {
        webServer.send(400, "text/plain", "SSID required");
    }
}

void handleSuccess() {
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Success check: WiFi is connected");
        webServer.send(200, "text/plain", "connected");
    }
    else {
        if (waitingForCredentialsVerification) {
            Serial.println("Success check: Still connecting...");
            webServer.send(200, "text/plain", "connecting");
        }
        else {
            Serial.println("Success check: Not connected");
            webServer.send(200, "text/plain", "not connected");
        }
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
    int n = WiFi.scanComplete();
    String json = "[";

    if (n == -2) {
        WiFi.scanNetworksAsync([](int networksFound) {
            Serial.printf("Scan started, found %d networks\n", networksFound);
            }, true);
        json = "[]";
    }
    else if (n == -1) {
        json = "[]";
    }
    else if (n == 0) {
        json = "[]";
        WiFi.scanDelete();
        WiFi.scanNetworksAsync([](int networksFound) {
            Serial.printf("New scan started, found %d networks\n", networksFound);
            }, true);
    }
    else if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
        }

        WiFi.scanDelete();
        WiFi.scanNetworksAsync([](int networksFound) {
            Serial.printf("New scan after results, found %d networks\n", networksFound);
            }, true);
    }

    json += "]";
    webServer.send(200, "application/json", json);
}

void handleNotFound() {
    if (isAccessPointMode) {
        handleRoot();
    }
    else {
        webServer.send(404, "text/plain", "Not found");
    }
}

bool connectToWiFi(String ssid, String password) {
    Serial.println("Attempting to connect to WiFi: " + ssid);

    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_STA);
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

        wifiCreds.connected = true;
        saveWiFiCredentials(ssid, password);

        return true;
    }
    else {
        Serial.println("\nFailed to connect to WiFi");
        return false;
    }
}

void sendDataToServer(bool isHello) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Cannot send data: WiFi not connected");
        updateDisplay("Server update failed", "WiFi not connected", "Please check connection");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String url = SERVER_URL;

    Serial.print("Sending data to server: ");
    Serial.println(url);

    if (!http.begin(client, url)) {
        Serial.println("Connection to server failed");
        updateDisplay("Server error", "Connection failed", "Will retry later");
        return;
    }

    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(1024);
    doc["boardID"] = deviceData.boardID;
    doc["user"] = deviceData.user;
    doc["text"] = deviceData.text;
    doc["status"] = deviceData.status;
    doc["token"] = deviceData.token;
    doc["uptime"] = deviceData.uptime;
    doc["serverUrl"] = deviceData.serverUrl;
    doc["mac"] = getMacAddress();

    if (isHello) {
        doc["time"] = millis();
        doc["hello"] = "Привет от ESP8266";
        Serial.println("Sending hello message to server");
    }
    else {
        doc["time"] = millis();
        doc["timer"] = deviceData.timer;
    }

    String payload;
    serializeJson(doc, payload);

    Serial.print("Sending: ");
    Serial.println(payload);

    int httpCode = http.POST(payload);

    if (httpCode > 0) {
        Serial.printf("HTTP Response code: %d\n", httpCode);

        if (httpCode == HTTP_CODE_OK) {
            String response = http.getString();
            Serial.println("Server response: " + response);

            DynamicJsonDocument respDoc(1024);
            DeserializationError error = deserializeJson(respDoc, response);

            if (!error) {
                bool dataChanged = false;

                if (respDoc.containsKey("boardID")) {
                    String newboardID = respDoc["boardID"].as<String>();
                    if (newboardID.length() != 0 && deviceData.boardID != newboardID) {
                        deviceData.boardID = newboardID;
                        dataChanged = true;
                        Serial.println("Updated boardID: " + deviceData.boardID);
                    }
                }

                if (respDoc.containsKey("user")) {
                    String newuser = respDoc["user"].as<String>();
                    if (newuser.length() != 0 && deviceData.user != newuser) {
                        deviceData.user = newuser;
                        dataChanged = true;
                        Serial.println("Updated user: " + deviceData.user);
                    }
                }

                if (respDoc.containsKey("text")) {
                    String newText = respDoc["text"].as<String>();
                    if (newText.length() != 0 && deviceData.text != newText) {
                        deviceData.text = newText;
                        dataChanged = true;
                        Serial.println("Updated text: " + deviceData.text);
                    }
                }

                if (respDoc.containsKey("status")) {
                    String newStatus = respDoc["status"].as<String>();
                    if (newStatus.length() != 0 && deviceData.status != newStatus) {
                        deviceData.status = newStatus;
                        dataChanged = true;
                        Serial.println("Updated status: " + deviceData.status);
                    }
                }

                if (respDoc.containsKey("token")) {
                    String newtoken = respDoc["token"].as<String>();
                    if (newtoken.length() != 0 && deviceData.token != newtoken) {
                        deviceData.token = newtoken;
                        dataChanged = true;
                        Serial.println("Updated token: " + deviceData.token);
                    }
                }

                if (respDoc.containsKey("uptime")) {
                    long newUptime = respDoc["uptime"].as<long>();
                    if (newUptime > 0 && deviceData.uptime != newUptime) {
                        deviceData.uptime = newUptime;
                        dataChanged = true;
                        Serial.println("Updated uptime: " + String(deviceData.uptime));
                    }
                }

                if (respDoc.containsKey("serverUrl")) {
                    String newserverUrl = respDoc["serverUrl"].as<String>();
                    if (newserverUrl.length() != 0 && deviceData.serverUrl != newserverUrl) {
                        deviceData.serverUrl = newserverUrl;
                        SERVER_URL = newserverUrl;
                        dataChanged = true;
                        Serial.println("Updated server URL to: " + SERVER_URL);
                    }
                }

                if (dataChanged) {
                    saveDeviceData();
                    Serial.println("Saved updated device data to flash");

                    updateDisplay(
                        "Text: " + deviceData.text,
                        "Time: " + String(millis()),
                        "Status: " + deviceData.status
                    );
                }

                lastServerUpdate = millis();
            }
            else {
                Serial.print("JSON parsing failed: ");
                Serial.println(error.c_str());
                updateDisplay("Server comm error", "Invalid response", "Will retry later");
            }
        }
        else {
            updateDisplay("Server error", "HTTP code: " + String(httpCode), "Will retry later");
        }
    }
    else {
        Serial.printf("HTTP request failed: %s\n", http.errorToString(httpCode).c_str());
        updateDisplay("Server error", http.errorToString(httpCode).c_str(), "Will retry later");
    }

    http.end();
}


String getWiFiSignalStrength() {
    if (WiFi.status() != WL_CONNECTED) {
        return "Not connected";
    }

    int rssi = WiFi.RSSI();
    String bars = "";

    if (rssi > -55) bars = "●●●●";
    else if (rssi > -65) bars = "●●●○";
    else if (rssi > -75) bars = "●●○○";
    else if (rssi > -85) bars = "●○○○";
    else bars = "○○○○";

    return bars + " " + String(rssi) + " dBm";
}

String getMacAddress() {
    byte mac[6];
    WiFi.macAddress(mac);
    char macStr[18] = { 0 };
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

void loadDeviceData() {
    if (!LittleFS.exists("/device.json")) {
        Serial.println("No device data found");
        return;
    }

    File file = LittleFS.open("/device.json", "r");
    if (!file) {
        Serial.println("Failed to open device data file");
        return;
    }

    size_t size = file.size();
    std::unique_ptr<char[]> buf(new char[size]);
    file.readBytes(buf.get(), size);
    file.close();

    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, buf.get());

    if (error) {
        Serial.print("JSON parsing failed: ");
        Serial.println(error.c_str());
        return;
    }

    deviceData.boardID = doc["boardID"].as<String>();
    deviceData.token = doc["token"].as<String>();
    deviceData.timer = doc["timer"];
    deviceData.uptime = doc["uptime"];
    deviceData.text = doc["text"].as<String>();
    deviceData.status = doc["status"].as<String>();
    deviceData.user = doc["user"].as<String>();

    if (doc.containsKey("serverUrl")) {
        deviceData.serverUrl = doc["serverUrl"].as<String>();
    }

    Serial.println("Device data loaded:");
    Serial.println("- Board ID: " + deviceData.boardID);
    Serial.println("- Uptime: " + String(deviceData.uptime));
    Serial.println("- Text: " + deviceData.text);
    Serial.println("- Status: " + deviceData.status);
    Serial.println("- Server URL: " + deviceData.serverUrl);
}

void saveDeviceData() {
    DynamicJsonDocument doc(1024);

    doc["boardID"] = deviceData.boardID;
    doc["token"] = deviceData.token;
    doc["timer"] = deviceData.timer;
    doc["uptime"] = deviceData.uptime;
    doc["text"] = deviceData.text;
    doc["status"] = deviceData.status;
    doc["user"] = deviceData.user;
    doc["serverUrl"] = deviceData.serverUrl;

    File file = LittleFS.open("/device.json", "w");
    if (!file) {
        Serial.println("Failed to open device data file for writing");
        return;
    }

    if (serializeJson(doc, file) == 0) {
        Serial.println("Failed to write to device data file");
    }
    else {
        Serial.println("Device data saved");
    }

    file.close();
}

void loadWiFiCredentials() {
    if (!LittleFS.exists("/wifi.json")) {
        Serial.println("No WiFi credentials found");
        wifiCreds.ssid = "";
        wifiCreds.password = "";
        wifiCreds.connected = false;
        return;
    }

    File file = LittleFS.open("/wifi.json", "r");
    if (!file) {
        Serial.println("Failed to open WiFi credentials file");
        return;
    }

    size_t size = file.size();
    std::unique_ptr<char[]> buf(new char[size]);
    file.readBytes(buf.get(), size);
    file.close();

    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, buf.get());

    if (error) {
        Serial.print("JSON parsing failed: ");
        Serial.println(error.c_str());
        return;
    }

    wifiCreds.ssid = doc["ssid"].as<String>();
    wifiCreds.password = doc["password"].as<String>();

    if (doc.containsKey("connected")) {
        wifiCreds.connected = doc["connected"].as<bool>();
    }
    else {
        wifiCreds.connected = false;
    }

    Serial.println("WiFi credentials loaded: " + wifiCreds.ssid);
    Serial.println("Connection status: " + String(wifiCreds.connected ? "Connected" : "Not connected"));
}

void saveWiFiCredentials(String ssid, String password) {
    DynamicJsonDocument doc(512);

    doc["ssid"] = ssid;
    doc["password"] = password;
    doc["connected"] = wifiCreds.connected;

    File file = LittleFS.open("/wifi.json", "w");
    if (!file) {
        Serial.println("Failed to open WiFi credentials file for writing");
        return;
    }

    if (serializeJson(doc, file) == 0) {
        Serial.println("Failed to write to WiFi credentials file");
    }
    else {
        Serial.println("WiFi credentials saved: " + ssid);
    }

    file.close();

    wifiCreds.ssid = ssid;
    wifiCreds.password = password;
}

void resetWiFiSettings() {
    updateDisplay("WiFi Reset", "Removing WiFi settings", "Please wait...");

    if (LittleFS.exists("/wifi.json")) {
        LittleFS.remove("/wifi.json");
        Serial.println("WiFi credentials removed");
    }

    wifiCreds.ssid = "";
    wifiCreds.password = "";
    wifiCreds.connected = false;

    WiFi.disconnect(true);
    delay(1000);

    updateDisplay("WiFi Reset Complete", "Starting setup mode", "Please reconnect");
    delay(2000);
    startAPMode();
}

void formatFS() {
    Serial.println("Formatting file system");
    LittleFS.format();
    if (!LittleFS.begin()) {
        Serial.println("File system format failed");
    }
    else {
        Serial.println("File system formatted");
    }
}