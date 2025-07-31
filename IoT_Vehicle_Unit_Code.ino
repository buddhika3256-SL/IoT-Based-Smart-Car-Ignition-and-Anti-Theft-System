// --- Required Libraries ---
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPSPlus.h>

#include <Adafruit_GFX.h>          // --- OLED ---
#include <Adafruit_SSD1306.h>

// --- WiFi Credentials ---
const char* ssid = "Dialog 4G 399";
const char* password = "7CFd00D3";

// --- MQTT Broker Info ---
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

// --- MQTT Topics ---
const char* mqtt_topic_relay_control = "esp32/relay/control";
const char* mqtt_topic_incomingdata_state_status = "esp32/incomingData/state/status";
const char* mqtt_topic_sensors1 = "esp32/incomingData/state/sensors1";
const char* mqtt_topic_sensors2 = "esp32/incomingData/state/sensors2";
const char* mqtt_topic_sensors3 = "esp32/incomingData/state/sensors3";

// --- Device ID ---
#define DEVICE_ID "esp32_data"

// --- Pin Definitions ---
const int pin_DL = 33;
const int pin_AC = 32;
const int pin_Siren = 25;
const int pin_ACC = 27;
const int pin_IGN = 14;
const int pin_Crank = 13;
const int pin_CenterLockBtn = 4;  // changed from 21 to 4 as per request
const int pin_RPM = 19;
const int pin_Speed = 18;
const int pin_Fuel = 39;
const int pin_Temp = 36;
const int pin_Vibration = 5;
const int pin_Motion = 23;
const int pin_DHT = 26;

// --- Pulse Counting Variables ---
volatile unsigned long rpmCount = 0;
volatile unsigned long speedCount = 0;

// --- Vibration Detection ---
volatile bool vibrationDetected = false;
volatile unsigned long lastVibrationTime = 0;

// --- Timing ---
unsigned long lastReport = 0;
const unsigned long reportInterval = 1000;
unsigned long crankStartTime = 0;
bool crankTriggered = false;
bool crankCompleted = true;
bool ignOn = false;
bool accOn = false;
bool allowCrank = true;
unsigned long accOffTime = 0;

// --- Constants for Calculations ---
const float pulsesPerRev = 1.0;
const float pulsesPerWheelRev = 4.0;
const float wheel_circumference_m = 2.0;

// --- DHT11 Sensor ---
#define DHTTYPE DHT11
DHT dht(pin_DHT, DHTTYPE);

// --- MPU6050 ---
Adafruit_MPU6050 mpu;

// --- GPS ---
HardwareSerial gpsSerial(2);
TinyGPSPlus gps;

WiFiClient espClient;
PubSubClient client(espClient);

// --- OLED Setup ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- ISR Functions ---
void IRAM_ATTR rpmISR() {
  rpmCount++;
}
void IRAM_ATTR speedISR() {
  speedCount++;
}
void IRAM_ATTR vibrationISR() {
  unsigned long currentTime = millis();
  if (currentTime - lastVibrationTime > 50) {
    vibrationDetected = true;
    lastVibrationTime = currentTime;
  }
}

// --- MQTT Callback ---
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) return;
  JsonObject cmd = doc["command"];

  if (cmd.containsKey("DL")) digitalWrite(pin_DL, !(cmd["DL"] == "ON"));
  if (cmd.containsKey("AC")) digitalWrite(pin_AC, !(cmd["AC"] == "ON"));
  if (cmd.containsKey("Siren")) digitalWrite(pin_Siren, !(cmd["Siren"] == "ON"));

  if (cmd.containsKey("ACC")) {
    if (cmd["ACC"] == "ON") {
      digitalWrite(pin_ACC, LOW);
      accOn = true;
      allowCrank = true;
    } else {
      digitalWrite(pin_IGN, HIGH);
      ignOn = false;
      delay(1000);
      digitalWrite(pin_ACC, HIGH);
      accOn = false;
      allowCrank = false;
      accOffTime = millis();
    }
  }

  if (cmd.containsKey("Crank") && cmd["Crank"] == "ON" && accOn && allowCrank) {
    digitalWrite(pin_IGN, LOW);
    ignOn = true;
    crankStartTime = millis();
    crankTriggered = true;
    crankCompleted = false;
  }
}

// --- MQTT Reconnection ---
void reconnectMQTT() {
  while (!client.connected()) {
    String clientId = DEVICE_ID + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      client.subscribe(mqtt_topic_relay_control);
    } else {
      delay(3000);
    }
  }
}

// --- OLED Pages Setup ---
const int NUM_PAGES = 7;
int currentPage = 0;
unsigned long pageStartTime = 0;

// Page durations in milliseconds:
const unsigned long pageDurations[NUM_PAGES] = {
  4000,  // Page 1: RPM, Speed
  3000,  // Page 2: Fuel, Temp
  3000,  // Page 3: AC state, DHT Temp/Humidity
  3000,  // Page 4: Motion, Vibration, Gyro
  2000,  // Page 5: GPS
  2000,  // Page 6: Siren, Vibration
  2000   // Page 7: About page
};

// Page names:
const char* pageNames[NUM_PAGES] = {
  "RPM & Speed",
  "Fuel & Temp",
  "AC & DHT",
  "Motion & Gyro",
  "GPS",
  "Siren & Vib",
  "About"
};

void drawPageHeader(int page) {
  display.fillRect(0, 0, SCREEN_WIDTH, 15, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(2, 4);
  display.setTextSize(1);
  display.print("Page ");
  display.print(page + 1);
  display.print(": ");
  display.print(pageNames[page]);
  display.setTextColor(SSD1306_WHITE);
}

// --- Setup ---
void setup() {
  Serial.begin(115200);
  gpsSerial.begin(9600, SERIAL_8N1, 16, 17);

  pinMode(pin_DL, OUTPUT); pinMode(pin_AC, OUTPUT);
  pinMode(pin_Siren, OUTPUT); pinMode(pin_ACC, OUTPUT);
  pinMode(pin_IGN, OUTPUT); pinMode(pin_Crank, OUTPUT);
  pinMode(pin_CenterLockBtn, INPUT_PULLUP);
  pinMode(pin_RPM, INPUT_PULLUP); pinMode(pin_Speed, INPUT_PULLUP);
  pinMode(pin_Vibration, INPUT_PULLUP);
  pinMode(pin_Motion, INPUT_PULLUP);

  digitalWrite(pin_DL, HIGH);
  digitalWrite(pin_AC, HIGH);
  digitalWrite(pin_Siren, HIGH);
  digitalWrite(pin_ACC, HIGH);
  digitalWrite(pin_IGN, HIGH);
  digitalWrite(pin_Crank, HIGH);

  attachInterrupt(digitalPinToInterrupt(pin_RPM), rpmISR, RISING);
  attachInterrupt(digitalPinToInterrupt(pin_Speed), speedISR, RISING);
  attachInterrupt(digitalPinToInterrupt(pin_Vibration), vibrationISR, FALLING);

  dht.begin();
  if (!mpu.begin()) Serial.println("MPU6050 not found!");

  // OLED init
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not found");
    while (true);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("System Booting...");
  display.display();
  delay(1000);

  // WiFi connect with OLED feedback
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("ESP32 WiFi");
  display.println("Connecting...");
  display.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    // (Optional) Could add some animation here
  }

  // Once connected
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("ESP32 WiFi");
  display.println("Connected!");
  display.display();
  Serial.println("\nWiFi connected");
  delay(1000);

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  pageStartTime = millis();  // start page timer
}

// --- Main Loop ---
void loop() {
  if (!client.connected()) reconnectMQTT();
  client.loop();

  while (gpsSerial.available()) gps.encode(gpsSerial.read());

  unsigned long now = millis();
  if (crankTriggered && ignOn && !crankCompleted) {
    if (now - crankStartTime >= 2000 && digitalRead(pin_Crank) == HIGH) {
      digitalWrite(pin_Crank, LOW);
    }

    if ((now - crankStartTime >= 5000) || ((rpmCount * 60.0) / pulsesPerRev > 900)) {
      digitalWrite(pin_Crank, HIGH);
      crankCompleted = true;
      allowCrank = false;
    }
  }

  if (now - lastReport >= reportInterval) {
    lastReport = now;

    noInterrupts();
    unsigned long rpmPulses = rpmCount;
    unsigned long speedPulses = speedCount;
    rpmCount = 0; speedCount = 0;
    interrupts();

    float rpmVal = 0;
    float speedVal = 0;

    if (millis() - accOffTime > 1000) {
      rpmVal = (rpmPulses * 60.0) / pulsesPerRev;
      speedVal = (speedPulses * wheel_circumference_m * 3.6) / pulsesPerWheelRev;
    }

    int temp = map(analogRead(pin_Temp), 0, 4095, 0, 100);
    int fuel = map(analogRead(pin_Fuel), 0, 4095, 0, 100);

    float dhtTemp = dht.readTemperature();
    float dhtHum = dht.readHumidity();

    sensors_event_t a, g, tempEvent;
    mpu.getEvent(&a, &g, &tempEvent);

    bool centerLock = digitalRead(pin_CenterLockBtn) == LOW;
    bool motion = digitalRead(pin_Motion) == LOW;
    bool vibration = false;

    if (vibrationDetected) {
      vibration = true;
      vibrationDetected = false;
    }

    // --- Status JSON ---
    StaticJsonDocument<512> statusDoc;
    JsonObject stat = statusDoc.createNestedObject("incomingData");
    stat["device_id"] = DEVICE_ID;
    stat["status_DL"] = digitalRead(pin_DL) ? "OFF" : "ON";
    stat["status_ac"] = digitalRead(pin_AC) ? "OFF" : "ON";
    stat["status_siren"] = digitalRead(pin_Siren) ? "OFF" : "ON";
    stat["status_acc"] = digitalRead(pin_ACC) ? "OFF" : "ON";
    stat["status_IGN"] = ignOn ? "ON" : "OFF";
    stat["status_Crank"] = digitalRead(pin_Crank) ? "OFF" : "ON";
    char statusBuf[512];
    serializeJson(statusDoc, statusBuf);
    client.publish(mqtt_topic_incomingdata_state_status, statusBuf);

    // --- Sensors1 JSON ---
    StaticJsonDocument<512> sensorsDoc1;
    JsonObject sens1 = sensorsDoc1.createNestedObject("incomingData");
    sens1["device_id"] = DEVICE_ID;
    sens1["state_cen-lock"] = centerLock ? "LOCKED" : "UNLOCKED";
    sens1["state_temp"] = temp;
    sens1["state_fuel"] = fuel;
    sens1["state_dht_temp"] = isnan(dhtTemp) ? -1 : dhtTemp;
    sens1["state_dht_hum"] = isnan(dhtHum) ? -1 : dhtHum;
    sens1["state_vibration"] = vibration ? "DETECT" : "NONE";
    sens1["state_motion"] = motion ? "STILL" : "MOTION";
    char sensorsBuf1[512];
    serializeJson(sensorsDoc1, sensorsBuf1);
    client.publish(mqtt_topic_sensors1, sensorsBuf1);

    // --- Sensors2 JSON ---
    StaticJsonDocument<512> sensorsDoc2;
    JsonObject sens2 = sensorsDoc2.createNestedObject("incomingData");
    sens2["device_id"] = DEVICE_ID;
    sens2["state_rpm"] = rpmVal;
    sens2["state_speed"] = speedVal;
    sens2["state_acc_x"] = a.acceleration.x;
    sens2["state_acc_y"] = a.acceleration.y;
    sens2["state_acc_z"] = a.acceleration.z;
    sens2["IGN"] = ignOn ? "ON" : "OFF";
    char sensorsBuf2[512];
    serializeJson(sensorsDoc2, sensorsBuf2);
    client.publish(mqtt_topic_sensors2, sensorsBuf2);

    // --- Sensors3 JSON for GPS ---
    StaticJsonDocument<256> sensorsDoc3;
    JsonObject sens3 = sensorsDoc3.createNestedObject("incomingData");
    sens3["device_id"] = DEVICE_ID;

    if (gps.location.isValid()) {
      sens3["state_lat"] = gps.location.lat();
      sens3["state_lng"] = gps.location.lng();
      sens3["state_speed_gps"] = gps.speed.kmph();
    } else {
      sens3["state_lat"] = "Searching...";
      sens3["state_lng"] = "Searching...";
      sens3["state_speed_gps"] = "- ";
    }

    char sensorsBuf3[256];
    serializeJson(sensorsDoc3, sensorsBuf3);
    client.publish(mqtt_topic_sensors3, sensorsBuf3);

    // --- OLED Display Update ---

    // Handle auto page switching
    if (now - pageStartTime >= pageDurations[currentPage]) {
      currentPage = (currentPage + 1) % NUM_PAGES;
      pageStartTime = now;
    }

    // Draw page header (yellow background)
    drawPageHeader(currentPage);

    // Draw bottom part with blue background
    display.fillRect(0, 15, SCREEN_WIDTH, SCREEN_HEIGHT - 15, SSD1306_BLACK); // Clear bottom area
    display.setTextColor(SSD1306_WHITE);

    switch (currentPage) {
      case 0: // Page 1: RPM & Speed
        display.setCursor(0, 20);
        display.setTextSize(1);
        display.print("RPM  : ");
        display.setTextSize(2);
        display.printf("%.0f\n", rpmVal);

        display.setTextSize(1);
        display.print("Speed: ");
        display.setTextSize(2);
        display.printf("%d", (int)speedVal);  // Speed value large
        display.setTextSize(1);
        display.print("km/h");                 // "km/h" small
        break;

      case 1: // Page 2: Fuel & Temp
        display.setCursor(0, 20);
        display.setTextSize(1);
        display.print("Fuel: ");
        display.setTextSize(2);
        display.printf("%d%%\n", fuel);

        display.setTextSize(1);
        display.print("Temp: ");
        display.setTextSize(2);
        display.printf("%d C", temp);
        break;

      case 2: // Page 3: AC & DHT
        display.setCursor(0, 20);
        display.setTextSize(1);
        display.printf("AC State: %s\n", digitalRead(pin_AC) ? "OFF" : "ON");
        display.printf("DHT Temp: %.1f C\n", isnan(dhtTemp) ? -1 : dhtTemp);
        display.printf("Humidity: %.1f%%", isnan(dhtHum) ? -1 : dhtHum);
        break;

      case 3: // Page 4: Motion, Vibration, Gyro
        display.setCursor(0, 20);
        display.setTextSize(1);
        display.printf("Motion: %s\n", motion ? "Still" : "Moving");
        display.printf("Vibration: %s\n", vibration ? "Yes" : "No");
        display.printf("Gyro X: %.2f\n", a.acceleration.x);
        display.printf("Gyro Y: %.2f\n", a.acceleration.y);
        display.printf("Gyro Z: %.2f", a.acceleration.z);
        break;

      case 4: // Page 5: GPS
        display.setCursor(0, 20);
        display.setTextSize(1);
        if (gps.location.isValid()) {
          display.printf("Lat: %.5f\n", gps.location.lat());
          display.printf("Lng: %.5f\n", gps.location.lng());
          display.printf("Speed GPS: %.1f km/h", gps.speed.kmph());
        } else {
          display.println("GPS: No Fix");
        }
        break;

      case 5: // Page 6: Siren, Vibration
        display.setCursor(0, 20);
        display.setTextSize(1);
        display.print("Siren: ");
        display.setTextSize(2);
        display.printf("%s\n", digitalRead(pin_Siren) ? "OFF" : "ON");

        display.setTextSize(1);
        display.print("Vib: ");
        display.setTextSize(2);
        display.printf("%s", vibration ? "YES" : "NO");
        break;

      case 6: // Page 7: About Page
        display.setCursor(0, 20);
        display.setTextSize(1);
        display.println("ESP32 Vehicle\nTelemetry System");
        display.println("By Your Name");
        display.println("2025");
        break;
    }

    display.display();
  }
}
