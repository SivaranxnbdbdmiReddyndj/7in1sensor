
#include <WiFi.h>
#include <esp_now.h>
#include <DHT.h>
#include <math.h>

// ================================================
// BABY MONITORING DEVICE (BMD)
// ESP32 + HC-SR04 + Sound Sensor + DHT11
// ESP-NOW transmitter to Parent Monitoring Device
// ================================================

// HC-SR04
#define TRIG_PIN 5
#define ECHO_PIN 18  // Use voltage divider

// Sound sensor analogue output
#define SOUND_PIN 34

// DHT11
#define DHT_PIN 4
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// Optional local RGB LED and buzzer
#define RED_PIN   25
#define GREEN_PIN 26
#define BLUE_PIN  27
#define BUZZER_PIN 23

// false = common cathode
// true = common anode
const bool COMMON_ANODE = false;

// ================================================
// CONFIGURATION
// ================================================

const float MIN_DISTANCE_CM = 1.0;
const float MAX_DISTANCE_CM = 20.0;

const int SOUND_THRESHOLD = 10;
const float TEMP_THRESHOLD_C = 40.0;

const unsigned long SEND_INTERVAL_MS = 500;
const unsigned long DHT_INTERVAL_MS = 2000;
const unsigned long SOUND_HOLD_MS = 500;

const int REQUIRED_DISTANCE_READINGS = 3;

// Must match the Parent Device receiver!
const uint32_t PACKET_MAGIC = 0xB2026001;

// Broadcast address: no parent MAC address needed
uint8_t parentAddress[] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

// ================================================
// WIRELESS PACKET
// Must match the parent's BabyStatus structure.
// ================================================

struct BabyStatus {
  uint32_t magic;
  uint32_t sequence;
  uint8_t presenceWarning;
  uint8_t cryWarning;
  uint8_t temperatureWarning;
};

BabyStatus statusPacket;

uint32_t packetSequence = 0;

bool espNowReady = false;
bool packetSent = false;

// ================================================
// SENSOR VARIABLES
// ================================================

float distanceCM = NAN;
float temperatureC = NAN;
float humidity = NAN;

bool echoReceived = false;
bool babyPresent = false;
bool presenceWarning = true;
bool cryWarning = false;
bool temperatureWarning = false;
bool temperatureValid = false;

int presenceCounter = 0;
int absenceCounter = 0;

unsigned long lastDHTRead = 0;
unsigned long lastSoundTime = 0;
unsigned long lastSendTime = 0;

bool presenceInitialised = false;

// ================================================
// RGB LED CONTROL
// ================================================

void setRGB(bool r, bool g, bool b) {
  if (COMMON_ANODE) {
    digitalWrite(RED_PIN, !r);
    digitalWrite(GREEN_PIN, !g);
    digitalWrite(BLUE_PIN, !b);
  } else {
    digitalWrite(RED_PIN, r);
    digitalWrite(GREEN_PIN, g);
    digitalWrite(BLUE_PIN, b);
  }
}

// ================================================
// ULTRASONIC SENSOR
// Object in 1-20 cm = presence detected.
// Repeated out-of-range/no-echo readings indicate
// presence not confirmed.
// ================================================

void updatePresence() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(3);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration =
      pulseIn(ECHO_PIN, HIGH, 30000);

  echoReceived = (duration != 0);

  if (echoReceived) {
    distanceCM = duration * 0.0343f / 2.0f;
  } else {
    distanceCM = NAN;
  }

  bool inRange =
      echoReceived &&
      distanceCM >= MIN_DISTANCE_CM &&
      distanceCM <= MAX_DISTANCE_CM;

  if (inRange) {
    presenceCounter++;
    absenceCounter = 0;
  } else {
    absenceCounter++;
    presenceCounter = 0;
  }

  if (presenceCounter >= REQUIRED_DISTANCE_READINGS) {
    babyPresent = true;
    presenceInitialised = true;
  }

  if (absenceCounter >= REQUIRED_DISTANCE_READINGS) {
    babyPresent = false;
    presenceInitialised = true;
  }

  // Initially, do not claim that presence is confirmed.
  presenceWarning = !babyPresent;
}

// ================================================
// SOUND SENSOR
// Analogue sound event detection, not cry recognition.
// ================================================

int readSoundLevel() {
  int minimum = 4095;
  int maximum = 0;

  unsigned long start = millis();

  while (millis() - start < 20) {
    int value = analogRead(SOUND_PIN);

    if (value < minimum) minimum = value;
    if (value > maximum) maximum = value;
  }

  return maximum - minimum;
}

void updateSound() {
  int soundLevel = readSoundLevel();

  if (soundLevel >= SOUND_THRESHOLD) {
    lastSoundTime = millis();
  }

  cryWarning =
      (millis() - lastSoundTime <= SOUND_HOLD_MS);

  Serial.print("Sound level: ");
  Serial.println(soundLevel);
}

// ================================================
// DHT11 TEMPERATURE
// ================================================

void updateTemperature() {
  if (millis() - lastDHTRead < DHT_INTERVAL_MS) {
    return;
  }

  lastDHTRead = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && !isnan(h)) {
    temperatureC = t;
    humidity = h;
    temperatureValid = true;

    temperatureWarning =
        (temperatureC > TEMP_THRESHOLD_C);
  } else {
    temperatureValid = false;

    // Treat a failed temperature reading as a warning.
    temperatureWarning = true;
  }
}

// ================================================
// ESP-NOW TRANSMISSION
// ================================================

void sendBabyStatus() {
  statusPacket.magic = PACKET_MAGIC;
  statusPacket.sequence = ++packetSequence;

  statusPacket.presenceWarning =
      presenceWarning ? 1 : 0;

  statusPacket.cryWarning =
      cryWarning ? 1 : 0;

  statusPacket.temperatureWarning =
      temperatureWarning ? 1 : 0;

  if (!espNowReady) {
    Serial.println("ESP-NOW not ready!");
    return;
  }

  esp_err_t result = esp_now_send(
      parentAddress,
      (uint8_t *)&statusPacket,
      sizeof(statusPacket)
  );

  packetSent = (result == ESP_OK);

  Serial.print("ESP-NOW queue result: ");
  Serial.println(packetSent ? "OK" : "FAILED");
}

// ================================================
// LOCAL BMD ALERTS
// ================================================

void updateLocalAlerts() {
  int warningCount = 0;

  if (presenceWarning) warningCount++;
  if (cryWarning) warningCount++;
  if (temperatureWarning) warningCount++;

  if (warningCount == 0) {
    setRGB(false, false, false);
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

  if (warningCount >= 2) {
    setRGB(true, false, false);  // Solid red
  } else if (cryWarning) {
    setRGB(true, false, false);  // Red
  } else if (temperatureWarning) {
    setRGB(true, true, false);   // Yellow
  } else {
    setRGB(false, false, true);  // Blue
  }

  digitalWrite(BUZZER_PIN, HIGH);
}

// ================================================
// SETUP
// ================================================

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(SOUND_PIN, INPUT);

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  analogReadResolution(12);
  dht.begin();

  digitalWrite(TRIG_PIN, LOW);
  setRGB(false, false, false);
  digitalWrite(BUZZER_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.println();
  Serial.println("BABY MONITORING DEVICE");
  Serial.print("BMD MAC address: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialisation failed!");
    return;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, parentAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer!");
    return;
  }

  espNowReady = true;

  lastDHTRead = millis() - DHT_INTERVAL_MS;

  Serial.println("ESP-NOW transmitter ready");
  Serial.println("Sending status every 500 ms");
}

// ================================================
// MAIN LOOP
// ================================================

void loop() {
  updatePresence();
  updateSound();
  updateTemperature();

  updateLocalAlerts();

  if (millis() - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = millis();
    sendBabyStatus();
  }

  Serial.print("Distance: ");

  if (echoReceived) {
    Serial.print(distanceCM, 1);
    Serial.print(" cm");
  } else {
    Serial.print("NO ECHO");
  }

  Serial.print(" | Presence warning: ");
  Serial.print(presenceWarning ? "YES" : "NO");

  Serial.print(" | Cry-like sound: ");
  Serial.print(cryWarning ? "YES" : "NO");

  Serial.print(" | Temperature: ");

  if (temperatureValid) {
    Serial.print(temperatureC, 1);
    Serial.print(" C");
  } else {
    Serial.print("SENSOR ERROR");
  }

  Serial.print(" | Temperature warning: ");
  Serial.println(temperatureWarning ? "YES" : "NO");

  delay(50);
}
