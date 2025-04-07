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

// Default server URL - will be updated from server response if available
String SERVER_URL = "https://letpass.ru/?init";
const char* DEFAULT_SSID = "ESP8266_Setup";
const byte DNS_PORT = 53;
const int WIFI_RECONNECT_INTERVAL = 10000; // 10 seconds
const int SERVER_UPDATE_DEFAULT = 600000;  // 10 minutes
const int WIFI_CONNECTION_TIMEOUT = 20000; // 20 seconds wait for connection

IPAddress apIP(192, 168, 4, 1); // Standard AP IP that works better with captive portals

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

// Function declarations
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

void setup() {
    Serial.begin(115200);
    Serial.println("\nStarting up...");

    // Initialize LittleFS
    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed. Formatting...");
        formatFS();
    }

    // Setup display first so we can show status
    setupDisplay();
    updateDisplay("Starting up...", "Please wait...");

    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

    // Load saved data
    loadDeviceData();
    loadWiFiCredentials();

    // Set default values if no data exists
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
    }
    else {
        // Use saved server URL if available
        if (deviceData.serverUrl.length() > 0) {
            SERVER_URL = deviceData.serverUrl;
        }
    }

    // Try to connect to saved WiFi or start AP mode
    if (wifiCreds.ssid.length() > 0) {
        updateDisplay("Connecting to WiFi", wifiCreds.ssid);
        if (connectToWiFi(wifiCreds.ssid, wifiCreds.password)) {
            updateDisplay("Connected to WiFi", wifiCreds.ssid, getWiFiSignalStrength());
            wifiCreds.connected = true;

            // Show wait message during server communication
            updateDisplay("Please wait", "Registering to server...", "WiFi " + wifiCreds.ssid + " connected");

            // Send initial hello message
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
}

void loop() {
    // Check for reset button press
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        delay(50);  // Debounce
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

    // Check battery voltage
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

        // Handle credential verification if needed
// Handle credential verification if needed
        if (waitingForCredentialsVerification) {
            if (WiFi.status() == WL_CONNECTED) {
                waitingForCredentialsVerification = false;
                updateDisplay("Connected to WiFi", wifiCreds.ssid, getWiFiSignalStrength());
                wifiCreds.connected = true;

                // Send initial hello message
                sendDataToServer(true);

                // Keep the AP active for a short while so the user can see success
                delay(3000);

                // Handle redirect if specified
                if (pendingRedirectUrl.length() > 0) {
                    // Keep AP running but let the client know we're connected
                }
                else {
                    // Stop AP mode
                    isAccessPointMode = false;
                    dnsServer.stop();
                    webServer.stop();
                    WiFi.mode(WIFI_STA);
                }
            }
            else if (currentMillis - lastConnectionAttempt >= WIFI_CONNECTION_TIMEOUT) {
                // Connection timed out, go back to AP mode
                waitingForCredentialsVerification = false;
                WiFi.disconnect();

                updateDisplay("WiFi Failed", "Please try again", "Check credentials");

                // Keep AP running
                WiFi.mode(WIFI_AP_STA);
                WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
                WiFi.softAP(DEFAULT_SSID);
            }
        }

        // Periodically scan for networks to keep the list updated
        if (currentMillis - lastWifiScan >= 10000) {  // Every 10 seconds
            lastWifiScan = currentMillis;
            WiFi.scanNetworks(true); // Non-blocking scan
        }
    }
    else if (wifiCreds.connected) {
        // Check if it's time to update server data
        if (currentMillis - lastServerUpdate >= deviceData.uptime) {
            lastServerUpdate = currentMillis;
            updateDisplay("Please wait", "Updating data...", "WiFi " + wifiCreds.ssid + " connected");
            sendDataToServer(false);
        }

        // Update timer
        deviceData.timer = millis(); // Use actual device uptime

        // Update display every second to avoid flickering
        if (currentMillis - lastDisplayUpdate >= 1000) {
            lastDisplayUpdate = currentMillis;
            updateDisplay(
                deviceData.text,
                "Timer: " + String(deviceData.timer / 1000) + "s",
                deviceData.status
            );
        }

        // Save data periodically (every hour)
        static unsigned long lastSave = 0;
        if (currentMillis - lastSave >= 3600000) {
            lastSave = currentMillis;
            saveDeviceData();
        }

        // Check WiFi connection and reconnect if needed
        if (WiFi.status() != WL_CONNECTED) {
            if (currentMillis - lastConnectionAttempt >= WIFI_RECONNECT_INTERVAL) {
                lastConnectionAttempt = currentMillis;
                updateDisplay("Reconnecting...", wifiCreds.ssid, "WiFi disconnected");

                if (connectToWiFi(wifiCreds.ssid, wifiCreds.password)) {
                    updateDisplay("Reconnected", wifiCreds.ssid, "WiFi connected");

                    // Send data to server after reconnection
                    sendDataToServer(false);
                }
                else {
                    updateDisplay("Reconnect failed", "Will retry...", "WiFi disconnected");

                    // After several failed attempts, switch to AP mode
                    static int failedAttempts = 0;
                    failedAttempts++;

                    if (failedAttempts >= 3) {  // After 3 failed attempts (30 seconds)
                        failedAttempts = 0;
                        startAPMode();
                    }
                }
            }
        }
    }

    delay(50); // Small delay to prevent watchdog reset
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

    // Only update if content changed to prevent flickering
    if (line1 == lastDisplayLine1 && line2 == lastDisplayLine2 && line3 == lastDisplayLine3) {
        return;
    }

    lastDisplayLine1 = line1;
    lastDisplayLine2 = line2;
    lastDisplayLine3 = line3;

    display.clearDisplay();

    // Center text for first line
    centerText(line1, 0);

    // Center text for second line
    centerText(line2, 10);

    if (line3.length() > 0) {
        // Center text for third line
        centerText(line3, 20);
    }

    // Draw WiFi signal strength indicator
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

void centerText(String text, int y) {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((DISPLAY_WIDTH - w) / 2, y);
    display.println(text);
}

void startAPMode() {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP_STA); // Both AP and STA mode for scanning
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(DEFAULT_SSID);

    // Setup DNS server to redirect all domains to the captive portal
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

    // Force a WiFi scan when starting AP mode
    lastWifiScan = millis() - 10000;  // Ensure scan happens immediately
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
      display: none; /* Hidden by default */
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
    
    <form id="wifi-form" onsubmit="return validateForm()">
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
    
    <div id="status-message" class="status"></div>
  </div>
  
  <script>
    // Scan for networks when page loads and then every 5 seconds
    window.onload = function() {
      fetchNetworks();
      setInterval(fetchNetworks, 5000);
    };
    
    function fetchNetworks() {
      document.getElementById('scanning').textContent = 'Scanning for networks...';
      
      fetch('/scan')
        .then(response => response.json())
        .then(data => {
          const networksDiv = document.getElementById('networks');
          networksDiv.innerHTML = '';
          
          if (data.length === 0) {
            networksDiv.innerHTML = '<p id="scanning">No networks found. Refreshing...</p>';
            return;
          }
          
          // Sort networks by signal strength
          data.sort((a, b) => b.rssi - a.rssi);
          
          data.forEach(network => {
            const div = document.createElement('div');
            div.className = 'network';
            
            // Calculate signal bars
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
          });
        })
        .catch(error => {
          document.getElementById('networks').innerHTML = '<p id="scanning">Error scanning networks. Retrying...</p>';
          console.error('Error:', error);
        });
    }
    
    function validateForm() {
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
        // Check connection status periodically
        checkConnectionStatus();
      })
      .catch(error => {
        showStatus('Error connecting: ' + error, 'error');
      });
      
      return false; // Prevent form submission
    }
    
    function checkConnectionStatus() {
      const statusCheck = setInterval(function() {
        fetch('/success')
        .then(response => {
          if(response.ok) {
            clearInterval(statusCheck);
            showStatus('Connection successful!', 'success');
            
            // Check for redirect URL
            const redirectUrl = document.getElementById('redirect_url').value;
            if(redirectUrl && redirectUrl.length > 0) {
              showStatus('Redirecting to ' + redirectUrl + ' in 3 seconds...', 'success');
              setTimeout(function() {
                window.location.href = redirectUrl;
              }, 3000);
            }
          }
        })
        .catch(error => {
          // Still trying...
        });
      }, 1000);
      
      // Set a timeout in case connection never succeeds
      setTimeout(function() {
        clearInterval(statusCheck);
        showStatus('Connection attempt timed out. Please check your password and try again.', 'error');
      }, 20000);
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
        // Save credentials first
        saveWiFiCredentials(ssid, password);

        // Save redirect URL for later use
        pendingRedirectUrl = redirectUrl;

        // Set waiting flag to indicate we're verifying credentials
        waitingForCredentialsVerification = true;
        lastConnectionAttempt = millis();

        // Update display
        updateDisplay("Connecting to", ssid, "Please wait...");

        // Attempt connection in background
        WiFi.disconnect();
        WiFi.mode(WIFI_AP_STA); // Keep AP running while testing STA connection
        WiFi.begin(ssid.c_str(), password.c_str());

        // Send a simple acknowledgement
        webServer.send(200, "text/plain", "Attempting to connect to " + ssid);
    }
    else {
        webServer.send(400, "text/plain", "SSID required");
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
    int n = WiFi.scanComplete();

    if (n == -2) {
        // Scan not triggered
        WiFi.scanNetworks(true);
        json = "[]";
    }
    else if (n == -1) {
        // Scan in progress
        json = "[]";
    }
    else {
        for (int i = 0; i < n; i++) {
            if (i > 0) json += ",";
            json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
        }

        // Delete scan results
        WiFi.scanDelete();

        // Start new scan
        WiFi.scanNetworks(true);
    }

    json += "]";
    webServer.send(200, "application/json", json);
}

void handleNotFound() {
    // Redirect all requests to the setup page when in AP mode
    if (isAccessPointMode) {
        // Serve the same HTML as handleRoot for any URL request
        handleRoot();
    }
    else {
        webServer.send(404, "text/plain", "Not found");
    }
}

bool connectToWiFi(String ssid, String password) {
    WiFi.disconnect();
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

void sendDataToServer(bool isHello) {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();  // Disable certificate verification
    HTTPClient http;

    Serial.println(isHello ? "Sending hello to server..." : "Sending data to server...");

    // Use the current SERVER_URL value
    http.begin(client, SERVER_URL);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<256> jsonDoc;
    jsonDoc["id"] = deviceData.boardID;
    jsonDoc["token"] = deviceData.token;
    jsonDoc["mac"] = getMacAddress();
    jsonDoc["timer"] = deviceData.timer;
    jsonDoc["time"] = millis();

    if (isHello) {
        jsonDoc["hello"] = "Hello from ESP8266";
    }

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
            bool dataChanged = false;

            if (responseDoc.containsKey("text") && responseDoc["text"].as<String>() != deviceData.text) {
                deviceData.text = responseDoc["text"].as<String>();
                dataChanged = true;
            }

            if (responseDoc.containsKey("status") && responseDoc["status"].as<String>() != deviceData.status) {
                deviceData.status = responseDoc["status"].as<String>();
                dataChanged = true;
            }

            if (responseDoc.containsKey("uptime")) {
                unsigned long newUptime = responseDoc["uptime"].as<unsigned long>();
                if (newUptime != deviceData.uptime) {
                    deviceData.uptime = newUptime;
                    dataChanged = true;
                }
            }

            if (responseDoc.containsKey("user")) {
                String newUser = responseDoc["user"].as<String>();
                if (newUser != deviceData.user) {
                    deviceData.user = newUser;
                    dataChanged = true;
                }
            }

            // Check for server URL updates - important fix to make sure the URL gets updated
            if (responseDoc.containsKey("server_url")) {
                String newServerUrl = responseDoc["server_url"].as<String>();
                if (newServerUrl != deviceData.serverUrl && newServerUrl.length() > 0) {
                    deviceData.serverUrl = newServerUrl;
                    SERVER_URL = newServerUrl;  // Make sure to update the global variable
                    Serial.println("Server URL updated to: " + SERVER_URL);
                    dataChanged = true;
                }
            }

            // Only save if data changed
            if (dataChanged) {
                saveDeviceData();
            }
        }

        // Update last server update time
        lastServerUpdate = millis();
    }

    http.end();
}

void resetWiFiSettings() {
    Serial.println("Resetting WiFi settings...");
    updateDisplay("Resetting WiFi", "Please wait...");

    // Remove wifi credentials file
    if (LittleFS.exists("/wifi.json")) {
        LittleFS.remove("/wifi.json");
    }

    // Clear the WiFi credentials in memory
    wifiCreds.ssid = "";
    wifiCreds.password = "";
    wifiCreds.connected = false;

    // Disconnect from current WiFi
    WiFi.disconnect(true);
    delay(1000);

    // Provide feedback
    updateDisplay("WiFi Reset", "Complete", "Restarting...");
    delay(2000);

    // Restart in AP mode
    ESP.restart();
}

void loadDeviceData() {
    if (LittleFS.exists("/device.json")) {
        File file = LittleFS.open("/device.json", "r");
        if (file) {
            StaticJsonDocument<512> doc;
            DeserializationError error = deserializeJson(doc, file);
            file.close();

            if (!error) {
                deviceData.boardID = doc["boardID"].as<String>();
                deviceData.token = doc["token"].as<String>();
                deviceData.timer = doc["timer"].as<unsigned long>();
                deviceData.uptime = doc["uptime"].as<unsigned long>();
                deviceData.text = doc["text"].as<String>();
                deviceData.status = doc["status"].as<String>();
                deviceData.user = doc["user"].as<String>();

                // Load server URL if it exists
                if (doc.containsKey("serverUrl")) {
                    deviceData.serverUrl = doc["serverUrl"].as<String>();
                }
                else {
                    deviceData.serverUrl = SERVER_URL;
                }

                Serial.println("Device data loaded");
            }
            else {
                Serial.println("Failed to parse device data");
            }
        }
    }
    else {
        Serial.println("No device data found");
    }
}

void saveDeviceData() {
    File file = LittleFS.open("/device.json", "w");
    if (file) {
        StaticJsonDocument<512> doc;
        doc["boardID"] = deviceData.boardID;
        doc["token"] = deviceData.token;
        doc["timer"] = deviceData.timer;
        doc["uptime"] = deviceData.uptime;
        doc["text"] = deviceData.text;
        doc["status"] = deviceData.status;
        doc["user"] = deviceData.user;
        doc["serverUrl"] = deviceData.serverUrl;

        if (serializeJson(doc, file)) {
            Serial.println("Device data saved");
        }
        else {
            Serial.println("Failed to write device data");
        }
        file.close();
    }
    else {
        Serial.println("Failed to open device data file for writing");
    }
}

void loadWiFiCredentials() {
    if (LittleFS.exists("/wifi.json")) {
        File file = LittleFS.open("/wifi.json", "r");
        if (file) {
            StaticJsonDocument<256> doc;
            DeserializationError error = deserializeJson(doc, file);
            file.close();

            if (!error) {
                wifiCreds.ssid = doc["ssid"].as<String>();
                wifiCreds.password = doc["password"].as<String>();
                Serial.println("WiFi credentials loaded");
            }
            else {
                Serial.println("Failed to parse WiFi credentials");
            }
        }
    }
    else {
        Serial.println("No WiFi credentials found");
    }
}

void saveWiFiCredentials(String ssid, String password) {
    File file = LittleFS.open("/wifi.json", "w");
    if (file) {
        StaticJsonDocument<256> doc;
        doc["ssid"] = ssid;
        doc["password"] = password;

        if (serializeJson(doc, file)) {
            Serial.println("WiFi credentials saved");
            wifiCreds.ssid = ssid;
            wifiCreds.password = password;
        }
        else {
            Serial.println("Failed to write WiFi credentials");
        }
        file.close();
    }
    else {
        Serial.println("Failed to open WiFi credentials file for writing");
    }
}

String getWiFiSignalStrength() {
    if (WiFi.status() == WL_CONNECTED) {
        int rssi = WiFi.RSSI();
        if (rssi > -55) return "Excellent";
        else if (rssi > -65) return "Good";
        else if (rssi > -75) return "Fair";
        else if (rssi > -85) return "Weak";
        else return "Poor";
    }
    return "Disconnected";
}

void formatFS() {
    Serial.println("Formatting filesystem...");
    LittleFS.format();
    if (LittleFS.begin()) {
        Serial.println("Filesystem formatted successfully");
    }
    else {
        Serial.println("Filesystem format failed");
    }
}