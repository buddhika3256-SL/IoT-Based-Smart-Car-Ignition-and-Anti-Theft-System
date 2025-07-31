#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- WiFi Credentials ---
const char* ssid = "Dialog 4G 399";
const char* password = "7CFd00D3";

// --- MQTT Broker ---
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

// --- MQTT Topics ---
const char* mqtt_topic_relay_control = "esp32/relay/control";
const char* mqtt_topic_sensors1 = "esp32/incomingData/state/sensors1";
const char* mqtt_topic_sensors2 = "esp32/incomingData/state/sensors2";
const char* mqtt_topic_sensors3 = "esp32/incomingData/state/sensors3";
const char* mqtt_topic_sensors4 = "esp32/incomingData/state/sensors4";
const char* mqtt_topic_status = "esp32/incomingData/state/status";

// --- Keypad Pins ---
const int keyPins[4] = {18, 19, 13, 12}; // ACC, CRK, AC, DL
bool relayStates[4] = {false, false, false, false};
bool lastKeyStates[4] = {true, true, true, true};

// --- Relay Labels ---
const int NUM_RELAYS = 5;
const char* relayLabels[NUM_RELAYS] = {"ACC", "CRK", "AC", "DL", "IGN"};
bool relayStatesFromJson[NUM_RELAYS] = {false, false, false, false, false};

// --- OLED Display ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- MQTT Client ---
WiFiClient espClient;
PubSubClient client(espClient);

// --- Sensor Data Variables ---
float rpmVal = 0;
float speedVal = 0;
int tempVal = -1;
int fuelVal = -1;
bool motionVal = false;
bool vibrationVal = false;
bool newSearch_on = false;
float gpsLat = 0.0;
float gpsLng = 0.0;
float gpsSpeed = 0.0;
bool gpsValid = false;
bool accRelay = false;
bool ignRelay = false;
bool crankRelay = false;
int searchToneState = 0;
// --- Display Timing ---
unsigned long lastDisplayChange = 0;
const unsigned long pageDuration = 4000;
int currentPage = 0;
const int totalPages = 4;

// --- Relay Status Page Display ---
bool showRelayStatusPage = false;
unsigned long relayStatusPageStart = 0;
const unsigned long relayStatusPageDuration = 5000;

// --- Buzzer Settings ---
#define BUZZER_PIN 26
#define TONE1 2500
#define TONE2 2400
#define TONE_DURATION 200

// --- LED Pins ---
#define SEARCH_LED_PIN 25   // Change as per your wiring
#define FOUND_LED_PIN 33    // LED flashes when FOUND

// --- Buzzer AlertTone_3 State Machine ---
int alertToneState = 0;
unsigned long lastToneChange = 0;
bool buzzerActive = false;

// --- LED Blink Timers ---
unsigned long lastSearchLEDToggle = 0;
unsigned long lastFoundLEDToggle = 0;
bool searchLEDState = false;
bool foundLEDState = false;

// --- Motion/Vibration alert repeat control ---
bool motionActive = false;
bool vibrationActive = false;
unsigned long motionToneTimer = 0;
unsigned long vibrationToneTimer = 0;
const unsigned long alertToneInterval = 1000;  // ms between repeated alerts

/*void handleAlertTone3() {
  unsigned long currentMillis = millis();
  const int shortTone = TONE_DURATION - 150;

  if (!buzzerActive || !newSearch_on) {
    noTone(BUZZER_PIN);
    alertToneState = 0;
    return;
  }

  switch (alertToneState) {
    case 0:
      tone(BUZZER_PIN, TONE2);
      lastToneChange = currentMillis;
      alertToneState = 1;
      break;
    case 1:
      if (currentMillis - lastToneChange > shortTone) {
        tone(BUZZER_PIN, TONE1);
        lastToneChange = currentMillis;
        alertToneState = 2;
      }
      break;
    case 2:
      if (currentMillis - lastToneChange > shortTone) {
        noTone(BUZZER_PIN);
        lastToneChange = currentMillis;
        alertToneState = 3;
      }
      break;
    case 3:
      if (currentMillis - lastToneChange > 50) {
        tone(BUZZER_PIN, TONE2);
        lastToneChange = currentMillis;
        alertToneState = 4;
      }
      break;
    case 4:
      if (currentMillis - lastToneChange > shortTone) {
        tone(BUZZER_PIN, TONE1);
        lastToneChange = currentMillis;
        alertToneState = 5;
      }
      break;
    case 5:
      if (currentMillis - lastToneChange > shortTone) {
        noTone(BUZZER_PIN);
        alertToneState = 0;
      }
      break;
  }
  }*/

void handleSearchTone() {
  unsigned long currentMillis = millis();
  const int lowToneDuration = 50;
  const int highToneDuration = 100;

  if (!buzzerActive || !newSearch_on) {
    noTone(BUZZER_PIN);
    searchToneState = 0;
    return;
  }

  switch (searchToneState) {
    case 0:
      tone(BUZZER_PIN, 2500); // Low pulsing tone
      lastToneChange = currentMillis;
      searchToneState = 1;
      break;

    case 1:
      if (currentMillis - lastToneChange > lowToneDuration) {
        noTone(BUZZER_PIN);
        lastToneChange = currentMillis;
        searchToneState = 2;
      }
      break;

    case 2:
      if (currentMillis - lastToneChange > 100) {
        tone(BUZZER_PIN, 3600); // Higher pitch, slightly longer
        lastToneChange = currentMillis;
        searchToneState = 3;
      }
      break;

    case 3:
      if (currentMillis - lastToneChange > highToneDuration) {
        noTone(BUZZER_PIN);
        lastToneChange = currentMillis;
        searchToneState = 4;
      }
      break;

    case 4:
      if (currentMillis - lastToneChange > 150) {
        searchToneState = 0;  // Repeat cycle
      }
      break;
  }
}


// --- Connect WiFi ---
void connectWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    display.setCursor(10, 45);
    display.println("WiFi Searching...");
    display.display();
    digitalWrite(SEARCH_LED_PIN, HIGH);
    delay(50);
    digitalWrite(SEARCH_LED_PIN, LOW);
    delay(1000);
    Serial.println("WiFi Searching..!");
  }
}

// --- MQTT Reconnect ---
void reconnectMQTT() {
  while (!client.connected()) {
    String clientId = "esp32_remote_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      client.subscribe(mqtt_topic_sensors1);
      client.subscribe(mqtt_topic_sensors2);
      client.subscribe(mqtt_topic_sensors3);
      client.subscribe(mqtt_topic_sensors4);
      client.subscribe(mqtt_topic_status);
    } else {
      delay(100);
    }
    digitalWrite(SEARCH_LED_PIN, HIGH);
    delay(50);
    digitalWrite(SEARCH_LED_PIN, LOW);
    delay(50);
    Serial.println("MQTT connecting..!");
  }
}

// --- Send Command for Individual Relay ---
void sendRelayCommand(int index) {
  StaticJsonDocument<128> doc;
  doc["device_id"] = "esp32_data";
  JsonObject cmd = doc.createNestedObject("command");

  switch (index) {
    case 0: cmd["ACC"] = relayStates[0] ? "ON" : "OFF"; break;
    case 1: cmd["Crank"] = relayStates[1] ? "ON" : "OFF"; break;
    case 2: cmd["AC"] = relayStates[2] ? "ON" : "OFF"; break;
    case 3: cmd["DL"] = relayStates[3] ? "ON" : "OFF"; break;
    default: return;
  }

  char buffer[128];
  serializeJson(doc, buffer);
  client.publish(mqtt_topic_relay_control, buffer);
}

// --- Parse Relay Status JSON ---
void parseRelayStatus(JsonObject obj) {
  bool oldStates[NUM_RELAYS];
  for (int i = 0; i < NUM_RELAYS; i++) oldStates[i] = relayStatesFromJson[i];

  relayStatesFromJson[0] = (strcmp(obj["status_acc"] | "", "ON") == 0);
  relayStatesFromJson[1] = (strcmp(obj["status_Crank"] | "", "ON") == 0);
  relayStatesFromJson[2] = (strcmp(obj["status_ac"] | "", "ON") == 0);
  relayStatesFromJson[3] = (strcmp(obj["status_DL"] | "", "ON") == 0);
  relayStatesFromJson[4] = (strcmp(obj["status_IGN"] | "", "ON") == 0);

  for (int i = 0; i < NUM_RELAYS; i++) {
    if (relayStatesFromJson[i] != oldStates[i]) {
      showRelayStatusPage = true;
      relayStatusPageStart = millis();
      break;
    }
  }
}

// --- Play Alert Tones 1 & 2 ---
void playAlertTone_1() {
  tone(BUZZER_PIN, TONE1, TONE_DURATION);
  delay(TONE_DURATION - 100);
  tone(BUZZER_PIN, TONE2, TONE_DURATION);
  delay(TONE_DURATION);
  noTone(BUZZER_PIN);
}

void playAlertTone_2() {
  tone(BUZZER_PIN, TONE2, TONE_DURATION - 150);
  delay(TONE_DURATION);
  tone(BUZZER_PIN, TONE1, TONE_DURATION - 150);
  delay(TONE_DURATION);
  noTone(BUZZER_PIN);
}

// --- MQTT Callback ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) return;

  String t = String(topic);
  JsonObject obj = doc["incomingData"];

  if (t == mqtt_topic_status) {
    parseRelayStatus(obj);
  } else if (t == mqtt_topic_sensors1) {
    int tempNew = obj["state_temp"] | -1;
    int fuelNew = obj["state_fuel"] | -1;
    bool newMotion = (obj["state_motion"] == "MOTION");
    bool newVibration = (obj["state_vibration"] == "DETECT");

    // Save previous states for buzzer logic in loop
    motionVal = newMotion;
    vibrationVal = newVibration;

    tempVal = tempNew;
    fuelVal = fuelNew;

  } else if (t == mqtt_topic_sensors2) {
    rpmVal = obj["state_rpm"] | 0.0;
    speedVal = obj["state_speed"] | 0.0;
    accRelay = (obj["IGN"] == "ON");

  } else if (t == mqtt_topic_sensors3) {
    if (obj["state_lat"].is<float>()) {
      gpsLat = obj["state_lat"];
      gpsLng = obj["state_lng"];
      gpsSpeed = obj["state_speed_gps"] | 0.0;
      gpsValid = true;
    } else {
      gpsValid = false;
    }

  } else if (t == mqtt_topic_sensors4) {
    newSearch_on = (obj["state_search"] == "FOUND");
  }
}

// --- UI Drawing ---
void drawSquare(int x, int y, bool filled) {
  if (filled) display.fillRect(x, y, 10, 10, SSD1306_WHITE);
  else display.drawRect(x, y, 10, 10, SSD1306_WHITE);
}

void drawHeaderWithSpeed() {
  display.fillRect(0, 0, SCREEN_WIDTH, 14, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.printf("RPM:%4.0f  %.1fkmh^-1", rpmVal, speedVal);
  display.setTextColor(SSD1306_WHITE);
}

void drawButtonStates() {
  int startX = 5;
  int startY = 20;
  int spacing = 24;

  for (int i = 0; i < 4; i++) {
    bool btnPressed = !digitalRead(keyPins[i]);
    drawSquare(startX + i * spacing, startY, btnPressed);
    drawSquare(startX + i * spacing, startY + 15, relayStatesFromJson[i]);
    display.setCursor(startX + i * spacing - 1, startY + 28);
    display.setTextSize(1);
    display.print(relayLabels[i]);
  }

  int ignX = startX + 4 * spacing + 8;
  drawSquare(ignX, startY + 15, relayStatesFromJson[4]);
  display.setCursor(ignX - 1, startY + 28);
  display.setTextSize(1);
  display.print("IGN");
}

void drawSensorPage() {
  display.setTextSize(1);
  if (currentPage == 1) {
    display.setCursor(0, 18); display.printf("Temp: %d C", tempVal);
    display.setCursor(80, 18); display.printf("Fuel: %d%%", fuelVal);
    display.setCursor(0, 30); display.printf("%s", motionVal ? "MOTION" : "STILL");
    display.setCursor(80, 30); display.printf("Vib: %s", vibrationVal ? "YES" : "NO");
  } else if (currentPage == 2) {
    display.setCursor(0, 18); display.printf("ACC: %s", relayStatesFromJson[0] ? "ON" : "OFF");
    display.setCursor(64, 18); display.printf("IGN: %s", relayStatesFromJson[4] ? "ON" : "OFF");
    display.setCursor(0, 30); display.printf("CRK: %s", relayStatesFromJson[1] ? "ON" : "OFF");
  } else if (currentPage == 3) {
    if (gpsValid) {
      display.setCursor(0, 18); display.printf("Lat: %.4f", gpsLat);
      display.setCursor(0, 30); display.printf("Lng: %.4f", gpsLng);
      display.setCursor(0, 42); display.printf("GPS: %.1f km/h", gpsSpeed);
    } else {
      display.setCursor(0, 30); display.print("GPS: Searching...");
    }
  }

  display.setCursor(100, 56);
  display.printf("%d/%d", currentPage + 1, totalPages);
}

// --- Draw "Follow me" message and icon ---
void drawFollowMe() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(15, 25);
  display.print("FIND ME!");

  // Simple arrow icon below text
  display.fillTriangle(60, 45, 68, 45, 64, 55, SSD1306_WHITE);
  display.drawLine(64, 55, 64, 60, SSD1306_WHITE);
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 4; i++) pinMode(keyPins[i], INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(SEARCH_LED_PIN, OUTPUT);
  pinMode(FOUND_LED_PIN, OUTPUT);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED failed to start");
    while (true);  // Halt if display fails
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 25);
  display.println("Remote Starting...");
  display.display();
  delay(3000);  // Wait 3 seconds to allow user to read it
  connectWiFi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
}

// --- Main Loop ---
void loop() {
  if (!client.connected()) reconnectMQTT();
  client.loop();

  unsigned long now = millis();

  // Priority 1: FOUND tone (newSearch_on)
  if (newSearch_on) {
    // Disable motion/vibration alerts during FOUND
    motionActive = false;
    vibrationActive = false;

    buzzerActive = true;
    handleSearchTone();

    // --- LED Blink Handling ---
    if (now - lastSearchLEDToggle > 100) {
      searchLEDState = !searchLEDState;
      digitalWrite(SEARCH_LED_PIN, searchLEDState ? HIGH : LOW);
      lastSearchLEDToggle = now;
    }
    digitalWrite(FOUND_LED_PIN, LOW);

  } else {
    buzzerActive = false;
    noTone(BUZZER_PIN);  // Stop FOUND tone

    // --- LED Blink Handling ---
    // Searching LED off
    digitalWrite(SEARCH_LED_PIN, LOW);
    // FOUND LED blinks faster
    if (now - lastFoundLEDToggle > 1000) {
      foundLEDState = !foundLEDState;
      digitalWrite(FOUND_LED_PIN, foundLEDState ? HIGH : LOW);
      lastFoundLEDToggle = now;
    }

    // Motion alert - play repeatedly
    if (motionVal) {
      if (!motionActive || now - motionToneTimer > alertToneInterval) {
        playAlertTone_2();
        motionActive = true;
        motionToneTimer = now;
      }
    } else {
      motionActive = false;
    }

    // Vibration alert - play repeatedly
    if (vibrationVal) {
      if (!vibrationActive || now - vibrationToneTimer > alertToneInterval) {
        playAlertTone_1();
        vibrationActive = true;
        vibrationToneTimer = now;
      }
    } else {
      vibrationActive = false;
    }
  }

  // --- Handle key presses ---
  for (int i = 0; i < 4; i++) {
    bool currState = !digitalRead(keyPins[i]);
    if (currState && lastKeyStates[i] == false) {
      relayStates[i] = !relayStates[i];
      sendRelayCommand(i);
      showRelayStatusPage = true;
      relayStatusPageStart = now;
      currentPage = 0;
    }
    lastKeyStates[i] = currState;
  }

  display.clearDisplay();

  if (newSearch_on) {
    // Show "Follow me" and icon, no page cycling during FOUND
    drawFollowMe();
  }
  else if (showRelayStatusPage) {
    if (now - relayStatusPageStart < relayStatusPageDuration) {
      drawHeaderWithSpeed();
      drawButtonStates();
    } else {
      showRelayStatusPage = false;
      lastDisplayChange = now;
    }
  } else {
    // Normal page cycling every pageDuration ms
    if (now - lastDisplayChange > pageDuration) {
      currentPage = (currentPage + 1) % totalPages;
      lastDisplayChange = now;
    }

    drawHeaderWithSpeed();

    if (currentPage == 0) {
      drawButtonStates();
    } else {
      drawSensorPage();
    }
  }

  display.display();
}
