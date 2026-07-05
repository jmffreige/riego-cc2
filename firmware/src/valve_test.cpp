#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>

namespace {

struct ValveZone {
  uint8_t openPin;
  uint8_t closePin;
};

constexpr ValveZone ZONES[] = {
  {16, 4},
  {18, 17},
  {27, 26},
  {33, 32},
};

constexpr uint8_t ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

// Ajustes de prueba:
// Las valvulas latching Rain Bird se accionan con pulsos cortos.
constexpr uint32_t PULSE_MS = 50;
constexpr uint32_t DEAD_TIME_MS = 100;
constexpr uint32_t PAUSE_BETWEEN_ACTIONS_MS = 15000;
constexpr uint32_t PAUSE_BETWEEN_ZONES_MS = 15000;
constexpr uint32_t PAUSE_BETWEEN_CYCLES_MS = 20000;

void configureNoSleep() {
  btStop();
  WiFi.mode(WIFI_OFF);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
}

void setAllValvePinsLow() {
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    digitalWrite(ZONES[i].openPin, LOW);
    digitalWrite(ZONES[i].closePin, LOW);
  }
}

void configureValvePins() {
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    pinMode(ZONES[i].openPin, OUTPUT);
    pinMode(ZONES[i].closePin, OUTPUT);
  }
  setAllValvePinsLow();
}

void driveZone(uint8_t zoneIndex, bool openValve) {
  const ValveZone& zone = ZONES[zoneIndex];
  const uint8_t activePin = openValve ? zone.openPin : zone.closePin;
  const uint8_t inactivePin = openValve ? zone.closePin : zone.openPin;

  Serial.printf("Zona %u: %s durante %lu ms. Pin activo GPIO %u, pin opuesto GPIO %u\n",
                zoneIndex + 1,
                openValve ? "ABRIR" : "CERRAR",
                static_cast<unsigned long>(PULSE_MS),
                activePin,
                inactivePin);

  setAllValvePinsLow();
  delay(DEAD_TIME_MS);
  digitalWrite(inactivePin, LOW);
  digitalWrite(activePin, HIGH);
  delay(PULSE_MS);
  setAllValvePinsLow();
}

void runTestCycle() {
  Serial.println();
  Serial.println("Ciclo de prueba corregido: abrir y cerrar cada zona secuencialmente");
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    driveZone(i, true);
    delay(PAUSE_BETWEEN_ACTIONS_MS);
    driveZone(i, false);
    delay(PAUSE_BETWEEN_ZONES_MS);
  }

  Serial.printf("Ciclo completo. Repite en %lu ms.\n",
                static_cast<unsigned long>(PAUSE_BETWEEN_CYCLES_MS));
  delay(PAUSE_BETWEEN_CYCLES_MS);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Riegos CC2 - prueba secuencial de valvulas");
  Serial.println("Firmware de diagnostico: no usa WiFi, no entra en deep sleep y repite el ciclo.");
  Serial.println("Envia pulsos corregidos: abrir/cerrar zona 1, luego zona 2, zona 3 y zona 4.");

  configureNoSleep();
  configureValvePins();
}

void loop() {
  runTestCycle();
}
