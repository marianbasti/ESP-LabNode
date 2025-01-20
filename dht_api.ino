#include <WiFi.h>
#include <WebServer.h>
#include <dht.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <EEPROM.h>

// Pin definitions
#define DHTPIN 4
#define DHTTYPE DHT22
#define RELAYPIN 3
#define LED_PIN 2  // Built-in LED for status

// Add after pin definitions
#define HOSTNAME_MAX_LENGTH 32
#define EEPROM_HOSTNAME_ADDR 0

struct Timer {
  bool enabled = false;
  unsigned long onDuration = 0;  // milliseconds
  unsigned long offDuration = 0; // milliseconds
  unsigned long lastToggle = 0;
  bool currentState = false;
};

// Create objects
WebServer server(80);
DHT dht(DHTPIN, DHTTYPE);
WiFiManager wifiManager;

// Add after global objects
Timer relayTimer;

// Add new global variables
char deviceHostname[HOSTNAME_MAX_LENGTH] = "esp32";

// Function to load hostname from EEPROM
void loadHostname() {
  EEPROM.begin(HOSTNAME_MAX_LENGTH);
  if (EEPROM.read(0) != 255) {  // Check if EEPROM has been written before
    for (int i = 0; i < HOSTNAME_MAX_LENGTH; i++) {
      deviceHostname[i] = EEPROM.read(EEPROM_HOSTNAME_ADDR + i);
      if (deviceHostname[i] == '\0') break;
    }
  }
  EEPROM.end();
}

// Function to save hostname to EEPROM
void saveHostname() {
  EEPROM.begin(HOSTNAME_MAX_LENGTH);
  for (int i = 0; i < HOSTNAME_MAX_LENGTH; i++) {
    EEPROM.write(EEPROM_HOSTNAME_ADDR + i, deviceHostname[i]);
    if (deviceHostname[i] == '\0') break;
  }
  EEPROM.commit();
  EEPROM.end();
}

// Function to find next available hostname number
void setupUniqueHostname() {
  for (int i = 1; i <= 255; i++) {
    char testHostname[HOSTNAME_MAX_LENGTH];
    snprintf(testHostname, HOSTNAME_MAX_LENGTH, "%s-%d", deviceHostname, i);
    if (!WiFi.hostByName(testHostname, IPAddress())) {
      snprintf(deviceHostname, HOSTNAME_MAX_LENGTH, "%s-%d", deviceHostname, i);
      break;
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialize pins
  pinMode(RELAYPIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(RELAYPIN, LOW);
  digitalWrite(LED_PIN, HIGH);  // LED on during setup
  
  dht.begin();
  
  // WiFiManager configuration
  wifiManager.setConfigPortalTimeout(180); // 3 minute timeout
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  
  loadHostname();
  setupUniqueHostname();
  
  // Add hostname configuration
  WiFi.setHostname(deviceHostname);
  
  // Try to connect to WiFi or create AP
  if(!wifiManager.autoConnect("ESP32_Setup", "password123")) {
    Serial.println("Failed to connect and hit timeout");
    ESP.restart();
  }
  
  // Connected to WiFi
  Serial.println("Connected to WiFi");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  // Setup API endpoints
  server.on("/api/sensor", HTTP_GET, handleSensorData);
  server.on("/api/relay", HTTP_POST, handleRelay);
  
  // Add new endpoint
  server.on("/api/timer", HTTP_ANY, handleTimer);
  server.on("/api/hostname", HTTP_ANY, handleHostname);
  
  server.begin();
  digitalWrite(LED_PIN, LOW);  // LED off when ready
}

void loop() {
  // Add timer control logic before WiFi check
  if (relayTimer.enabled) {
    unsigned long currentTime = millis();
    unsigned long elapsed = currentTime - relayTimer.lastToggle;
    
    if (relayTimer.currentState && elapsed >= relayTimer.onDuration) {
      digitalWrite(RELAYPIN, LOW);
      relayTimer.currentState = false;
      relayTimer.lastToggle = currentTime;
    } 
    else if (!relayTimer.currentState && elapsed >= relayTimer.offDuration) {
      digitalWrite(RELAYPIN, HIGH);
      relayTimer.currentState = true;
      relayTimer.lastToggle = currentTime;
    }
  }

  // Indicate WiFi status with LED
  if(WiFi.status() != WL_CONNECTED) {
    digitalWrite(LED_PIN, millis() % 1000 < 500);  // Blink LED when disconnected
    WiFi.reconnect();
    delay(1000);
  }
  
  server.handleClient();
}

void configModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
  // Fast LED blink in config mode
  digitalWrite(LED_PIN, millis() % 500 < 250);
}

void saveConfigCallback() {
  Serial.println("Configuration saved");
  digitalWrite(LED_PIN, HIGH);
  delay(1000);
  digitalWrite(LED_PIN, LOW);
}

// Add new timer configuration endpoint handler before handleRelay()
void handleTimer() {
  StaticJsonDocument<200> doc;

  if (server.method() == HTTP_GET) {
    doc["enabled"] = relayTimer.enabled;
    doc["onDuration"] = relayTimer.onDuration / 1000; // Convert to seconds
    doc["offDuration"] = relayTimer.offDuration / 1000;
    doc["currentState"] = relayTimer.currentState;
  }
  else if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"No data provided\"}");
      return;
    }

    String content = server.arg("plain");
    StaticJsonDocument<200> input;
    DeserializationError error = deserializeJson(input, content);

    if (error) {
      server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }

    if (input.containsKey("enabled")) {
      relayTimer.enabled = input["enabled"].as<bool>();
      if (!relayTimer.enabled) {
        digitalWrite(RELAYPIN, LOW);
        relayTimer.currentState = false;
      }
    }
    if (input.containsKey("onDuration")) {
      relayTimer.onDuration = input["onDuration"].as<unsigned long>() * 1000; // Convert to milliseconds
    }
    if (input.containsKey("offDuration")) {
      relayTimer.offDuration = input["offDuration"].as<unsigned long>() * 1000;
    }
    
    relayTimer.lastToggle = millis();
    doc["status"] = "success";
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// Modify handleRelay() to disable timer when manually controlling
void handleRelay() {
  // Disable timer when manually controlling
  relayTimer.enabled = false;
  
  if (!server.hasArg("state")) {
    server.send(400, "application/json", "{\"error\":\"Missing state parameter\"}");
    return;
  }
  
  String state = server.arg("state");
  bool relayState;
  
  if (state == "on") {
    relayState = true;
  } else if (state == "off") {
    relayState = false;
  } else {
    server.send(400, "application/json", "{\"error\":\"Invalid state parameter. Use 'on' or 'off'\"}");
    return;
  }
  
  digitalWrite(RELAYPIN, relayState);
  
  StaticJsonDocument<100> doc;
  doc["relay"] = relayState ? "on" : "off";
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// Modify handleSensorData() to include timer status
void handleSensorData() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  
  if (isnan(temperature) || isnan(humidity)) {
    server.send(500, "application/json", "{\"error\":\"Failed to read sensor data\"}");
    return;
  }
  
  StaticJsonDocument<300> doc;
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["timer"]["enabled"] = relayTimer.enabled;
  doc["timer"]["currentState"] = relayTimer.currentState;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

// Add new hostname configuration endpoint handler
void handleHostname() {
  StaticJsonDocument<200> doc;

  if (server.method() == HTTP_GET) {
    doc["hostname"] = deviceHostname;
  }
  else if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"No data provided\"}");
      return;
    }

    String content = server.arg("plain");
    StaticJsonDocument<200> input;
    DeserializationError error = deserializeJson(input, content);

    if (error || !input.containsKey("hostname")) {
      server.send(400, "application/json", "{\"error\":\"Invalid JSON or missing hostname\"}");
      return;
    }

    const char* newHostname = input["hostname"];
    if (strlen(newHostname) > HOSTNAME_MAX_LENGTH - 4) {  // Leave room for -255
      server.send(400, "application/json", "{\"error\":\"Hostname too long\"}");
      return;
    }

    strncpy(deviceHostname, newHostname, HOSTNAME_MAX_LENGTH);
    saveHostname();
    setupUniqueHostname();
    WiFi.setHostname(deviceHostname);
    
    doc["status"] = "success";
    doc["hostname"] = deviceHostname;
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}
