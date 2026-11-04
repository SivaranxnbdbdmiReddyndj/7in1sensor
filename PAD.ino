
#include <WiFi.h>
#include <esp_now.h>

// ================================================
// PARENT MONITORING DEVICE (PMD)
// ESP-NOW receiver
// ================================================

#define RED_PIN     25
#define GREEN_PIN   26
#define BLUE_PIN    27
#define BUZZER_PIN  4
#define VIB_PIN     5

// false = common-cathode RGB LED
// true  = common-anode RGB LED
const bool COMMON_ANODE = false;

// Set true if your buzzer/motor driver turns ON with HIGH.
const bool ACTIVE_HIGH_OUTPUTS = true;

// Heartbeat timeout
const unsigned long CONNECTION_TIMEOUT_MS = 4000;

// Pairing / waiting beep timing
const unsigned long BEEP_INTERVAL_MS = 500;
const unsigned long BEEP_ON_MS = 200;

// Expected packet from the baby device.
// The baby sender must use this exact structure.
struct BabyStatus {
  uint32_t magic;
  uint32_t sequence;
  uint8_t presenceWarning;
  uint8_t cryWarning;
  uint8_t temperatureWarning;
};

const uint32_t PACKET_MAGIC = 0xB2026001;

// Connection state
bool connected = false;
bool everConnected = false;

unsigned long lastPacketTime = 0;
unsigned long lastBeepToggle = 0;

bool beepOn = false;

// Latest baby-device alerts
bool presenceWarning = false;
bool cryWarning = false;
bool temperatureWarning = false;

// ================================================
// OUTPUT CONTROL
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

void setBuzzer(bool on) {
  digitalWrite(
    BUZZER_PIN,
    (on == ACTIVE_HIGH_OUTPUTS) ? HIGH : LOW
  );
}

void setVibration(bool on) {
  digitalWrite(
    VIB_PIN,
    (on == ACTIVE_HIGH_OUTPUTS) ? HIGH : LOW
  );
}

void allOutputsOff() {
  setRGB(false, false, false);
  setBuzzer(false);
  setVibration(false);
}

// ================================================
// ESP-NOW RECEIVE CALLBACK
// ================================================

void onDataReceived(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len
) {
  if (len != sizeof(BabyStatus)) {
    return;
  }

  BabyStatus packet;
  memcpy(&packet, data, sizeof(packet));

  if (packet.magic != PACKET_MAGIC) {
    return;
  }

  lastPacketTime = millis();
  connected = true;
  everConnected = true;

  presenceWarning = packet.presenceWarning != 0;
  cryWarning = packet.cryWarning != 0;
  temperatureWarning = packet.temperatureWarning != 0;

  Serial.println("Baby-device status received");
}

// ================================================
// SETUP
// ================================================

void setup() {
  Serial.begin(115200);

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(VIB_PIN, OUTPUT);

  allOutputsOff();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  Serial.println();
  Serial.println("PARENT MONITORING DEVICE");
  Serial.print("Parent MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW initialization failed!");
    return;
  }

  esp_now_register_recv_cb(onDataReceived);

  Serial.println("Waiting for baby-device communication...");
}

// ================================================
// MAIN LOOP
// ================================================

void loop() {
  unsigned long now = millis();

  // Determine whether communication is alive.
  connected =
    everConnected &&
    (now - lastPacketTime <= CONNECTION_TIMEOUT_MS);

  // Not connected: blue LED, repeated beep, vibration.
  if (!connected) {
    setRGB(false, false, true);
    setVibration(true);

    if (now - lastBeepToggle >= BEEP_INTERVAL_MS) {
      lastBeepToggle = now;
      beepOn = !beepOn;
    }

    setBuzzer(beepOn);

    if (!everConnected) {
      Serial.println("NOT PAIRED / WAITING FOR BABY DEVICE");
    } else {
      Serial.println("CONNECTION LOST!");
    }

    delay(50);
    return;
  }

  // Connected but no active warning.
  bool anyWarning =
    presenceWarning ||
    cryWarning ||
    temperatureWarning;

  if (!anyWarning) {
    setRGB(false, true, false); // GREEN
    setBuzzer(false);
    setVibration(false);
    beepOn = false;

    Serial.println("CONNECTED - ALL NORMAL");
    delay(100);
    return;
  }

  // Multiple alerts: solid red.
  int warningCount = 0;

  if (presenceWarning) warningCount++;
  if (cryWarning) warningCount++;
  if (temperatureWarning) warningCount++;

  if (warningCount >= 2) {
    setRGB(true, false, false);
  } else if (cryWarning) {
    setRGB(true, false, false);   // RED
  } else if (temperatureWarning) {
    setRGB(true, true, false);    // YELLOW
  } else {
    setRGB(false, false, true);   // BLUE
  }

  setBuzzer(true);
  setVibration(true);

  Serial.println("BABY DEVICE ALERT ACTIVE");

  delay(100);
}
