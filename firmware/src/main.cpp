#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <esp_sleep.h>
#include <cstring>

#include "secrets.h"

namespace {

constexpr uint8_t ZONE_COUNT = 4;
constexpr uint8_t MAX_ROUTINE_STEPS = 8;
constexpr uint8_t BATTERY_PIN = 35;
constexpr uint16_t VALVE_PULSE_MS = 50;
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
constexpr uint32_t MQTT_TIMEOUT_MS = 15000;
constexpr uint32_t MQTT_LISTEN_WINDOW_MS = 7000;
constexpr uint64_t DEFAULT_SLEEP_MINUTES = 10;
constexpr uint64_t US_PER_MINUTE = 60ULL * 1000ULL * 1000ULL;

// Lolin32 Lite battery input uses an onboard 1:2 divider on GPIO35.
constexpr float ADC_REFERENCE_V = 3.3f;
constexpr float ADC_MAX = 4095.0f;
constexpr float BATTERY_DIVIDER_FACTOR = 2.0f;
constexpr float BATTERY_EMPTY_V = 3.20f;
constexpr float BATTERY_FULL_V = 4.20f;

struct ValveZone {
  uint8_t openPin;
  uint8_t closePin;
  const char* commandTopic;
  const char* stateTopic;
};

struct RoutineStep {
  uint8_t zoneIndex;
  uint16_t durationMinutes;
};

struct RoutineRuntime {
  bool active;
  uint32_t id;
  uint8_t stepCount;
  uint8_t currentStepIndex;
  int8_t openZoneIndex;
  RoutineStep steps[MAX_ROUTINE_STEPS];
};

const ValveZone zones[ZONE_COUNT] = {
  {4, 16, "riego/zona1/cmd", "riego/zona1/state"},
  {17, 18, "riego/zona2/cmd", "riego/zona2/state"},
  {19, 21, "riego/zona3/cmd", "riego/zona3/state"},
  {22, 23, "riego/zona4/cmd", "riego/zona4/state"},
};

const char* ROUTINE_CONFIG_TOPIC = "riego/routine/config";
const char* ROUTINE_STATE_TOPIC = "riego/routine/state";
const char* BATTERY_TOPIC = "riego/device/battery";
const char* STATUS_TOPIC = "riego/device/status";
const char* CLIENT_ID = "ESP32_Riegos_CC2";

WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

RTC_DATA_ATTR int8_t lastAppliedState[ZONE_COUNT] = {-1, -1, -1, -1};
RTC_DATA_ATTR RoutineRuntime routine = {false, 0, 0, 0, -1, {}};
RTC_DATA_ATTR uint32_t completedRoutineId = 0;

uint64_t nextSleepMinutes = DEFAULT_SLEEP_MINUTES;
bool routineFinishedThisWake = false;

void setAllValvePinsLow() {
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    digitalWrite(zones[i].openPin, LOW);
    digitalWrite(zones[i].closePin, LOW);
  }
}

void configureValvePins() {
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    pinMode(zones[i].openPin, OUTPUT);
    pinMode(zones[i].closePin, OUTPUT);
  }
  setAllValvePinsLow();
}

void pulseValve(uint8_t zoneIndex, bool openValve) {
  if (zoneIndex >= ZONE_COUNT) {
    return;
  }

  const ValveZone& zone = zones[zoneIndex];
  Serial.printf("Zona %u: pulso de %s\n", zoneIndex + 1, openValve ? "apertura" : "cierre");

  setAllValvePinsLow();
  digitalWrite(zone.openPin, openValve ? HIGH : LOW);
  digitalWrite(zone.closePin, openValve ? LOW : HIGH);
  delay(VALVE_PULSE_MS);
  setAllValvePinsLow();

  lastAppliedState[zoneIndex] = openValve ? 1 : 0;
}

bool payloadToDesiredState(const byte* payload, unsigned int length, bool& desiredOpen) {
  char message[12] = {};
  if (length == 0 || length >= sizeof(message)) {
    return false;
  }

  memcpy(message, payload, length);
  message[length] = '\0';

  for (uint8_t i = 0; message[i] != '\0'; ++i) {
    message[i] = static_cast<char>(tolower(message[i]));
  }

  if (strcmp(message, "on") == 0 || strcmp(message, "open") == 0 || strcmp(message, "abrir") == 0 ||
      strcmp(message, "1") == 0 || strcmp(message, "true") == 0) {
    desiredOpen = true;
    return true;
  }

  if (strcmp(message, "off") == 0 || strcmp(message, "close") == 0 || strcmp(message, "cerrar") == 0 ||
      strcmp(message, "0") == 0 || strcmp(message, "false") == 0) {
    desiredOpen = false;
    return true;
  }

  return false;
}

void publishZoneState(uint8_t zoneIndex, bool isOpen) {
  mqtt.publish(zones[zoneIndex].stateTopic, isOpen ? "ON" : "OFF", true);
}

void publishRoutineState(const char* status) {
  if (!mqtt.connected()) {
    return;
  }

  char payload[160] = {};
  const int8_t openZone = routine.openZoneIndex >= 0 ? routine.openZoneIndex + 1 : 0;
  snprintf(payload, sizeof(payload),
           "{\"status\":\"%s\",\"id\":%lu,\"step\":%u,\"stepCount\":%u,\"openZone\":%d,\"nextWakeMinutes\":%llu}",
           status, static_cast<unsigned long>(routine.id), routine.currentStepIndex + 1, routine.stepCount, openZone,
           nextSleepMinutes);
  mqtt.publish(ROUTINE_STATE_TOPIC, payload, true);
}

void closeRoutineOpenZone() {
  if (routine.openZoneIndex < 0 || routine.openZoneIndex >= ZONE_COUNT) {
    routine.openZoneIndex = -1;
    return;
  }

  const uint8_t zoneIndex = static_cast<uint8_t>(routine.openZoneIndex);
  pulseValve(zoneIndex, false);
  publishZoneState(zoneIndex, false);
  routine.openZoneIndex = -1;
}

void clearRoutine(bool resetSummary = true) {
  routine.active = false;
  routine.openZoneIndex = -1;
  if (resetSummary) {
    routine.stepCount = 0;
    routine.currentStepIndex = 0;
  }
  nextSleepMinutes = DEFAULT_SLEEP_MINUTES;
}

void startRoutineStep(uint8_t stepIndex) {
  if (stepIndex >= routine.stepCount) {
    completedRoutineId = routine.id;
    routineFinishedThisWake = true;
    clearRoutine(false);
    publishRoutineState("finished");
    return;
  }

  routine.currentStepIndex = stepIndex;
  const RoutineStep& step = routine.steps[stepIndex];
  pulseValve(step.zoneIndex, true);
  publishZoneState(step.zoneIndex, true);
  routine.openZoneIndex = static_cast<int8_t>(step.zoneIndex);
  nextSleepMinutes = step.durationMinutes;
  publishRoutineState("watering");
}

void advanceRoutineAfterSleep() {
  if (!routine.active) {
    return;
  }

  Serial.println("Rutina activa: cambio de zona programado");
  closeRoutineOpenZone();

  const uint8_t nextStepIndex = routine.currentStepIndex + 1;
  if (nextStepIndex >= routine.stepCount) {
    Serial.println("Rutina finalizada");
    completedRoutineId = routine.id;
    routineFinishedThisWake = true;
    clearRoutine(false);
    publishRoutineState("finished");
    return;
  }

  startRoutineStep(nextStepIndex);
}

void applyRoutineConfig(const byte* payload, unsigned int length) {
  StaticJsonDocument<768> doc;
  const DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.printf("Rutina ignorada: JSON no valido (%s)\n", error.c_str());
    return;
  }

  const bool enabled = doc["enabled"] | false;
  if (!enabled) {
    Serial.println("Rutina desactivada por MQTT");
    closeRoutineOpenZone();
    clearRoutine();
    completedRoutineId = 0;
    routineFinishedThisWake = false;
    publishRoutineState("disabled");
    return;
  }

  if (doc["id"].isNull()) {
    Serial.println("Rutina ignorada: falta id");
    return;
  }

  const uint32_t id = doc["id"].as<uint32_t>();
  if (id == 0) {
    Serial.println("Rutina ignorada: id debe ser mayor que 0");
    return;
  }

  if (routine.active && routine.id == id) {
    Serial.printf("Rutina %lu ya esta en curso; se conserva el progreso\n", static_cast<unsigned long>(id));
    publishRoutineState("watering");
    return;
  }

  if (!routine.active && completedRoutineId == id) {
    Serial.printf("Rutina %lu ya fue completada; esperando un id nuevo\n", static_cast<unsigned long>(id));
    publishRoutineState("finished");
    return;
  }

  JsonArray steps = doc["steps"].as<JsonArray>();
  if (steps.isNull() || steps.size() == 0 || steps.size() > MAX_ROUTINE_STEPS) {
    Serial.println("Rutina ignorada: numero de pasos no valido");
    return;
  }

  RoutineStep parsedSteps[MAX_ROUTINE_STEPS] = {};
  uint8_t parsedCount = 0;
  for (JsonObject step : steps) {
    const int zone = step["zone"] | 0;
    int minutes = step["minutes"] | 0;
    if (minutes == 0) {
      minutes = step["durationMinutes"] | 0;
    }
    if (zone < 1 || zone > ZONE_COUNT || minutes < 1 || minutes > 1440) {
      Serial.println("Rutina ignorada: paso no valido");
      return;
    }

    parsedSteps[parsedCount++] = {static_cast<uint8_t>(zone - 1), static_cast<uint16_t>(minutes)};
  }

  Serial.printf("Rutina %lu recibida: %u pasos\n", static_cast<unsigned long>(id), parsedCount);
  closeRoutineOpenZone();

  routine.active = true;
  routine.id = id;
  completedRoutineId = 0;
  routineFinishedThisWake = false;
  routine.stepCount = parsedCount;
  routine.currentStepIndex = 0;
  routine.openZoneIndex = -1;
  for (uint8_t i = 0; i < parsedCount; ++i) {
    routine.steps[i] = parsedSteps[i];
  }

  startRoutineStep(0);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, ROUTINE_CONFIG_TOPIC) == 0) {
    applyRoutineConfig(payload, length);
    return;
  }

  bool desiredOpen = false;
  if (!payloadToDesiredState(payload, length, desiredOpen)) {
    Serial.printf("Mensaje ignorado en %s: comando no valido\n", topic);
    return;
  }

  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    if (strcmp(topic, zones[i].commandTopic) == 0) {
      if (routine.active) {
        Serial.printf("Comando manual ignorado en zona %u: rutina activa\n", i + 1);
        publishZoneState(i, lastAppliedState[i] == 1);
        return;
      }

      const int8_t desiredState = desiredOpen ? 1 : 0;
      if (lastAppliedState[i] != desiredState) {
        pulseValve(i, desiredOpen);
      } else {
        Serial.printf("Zona %u ya estaba en estado %s; no se repite pulso\n", i + 1, desiredOpen ? "ON" : "OFF");
      }

      publishZoneState(i, desiredOpen);
      return;
    }
  }
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(ssid, password);

  Serial.printf("Conectando WiFi a %s", ssid);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No se pudo conectar a WiFi dentro del tiempo limite");
    return false;
  }

  Serial.print("WiFi conectado. IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

bool connectMqtt() {
  secureClient.setInsecure();
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setBufferSize(768);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(5);

  Serial.print("Conectando MQTT");
  const uint32_t start = millis();
  while (!mqtt.connected() && millis() - start < MQTT_TIMEOUT_MS) {
    if (mqtt.connect(CLIENT_ID, mqtt_user, mqtt_password, STATUS_TOPIC, 1, true, "offline")) {
      break;
    }
    Serial.print(".");
    delay(1000);
  }
  Serial.println();

  if (!mqtt.connected()) {
    Serial.printf("No se pudo conectar a MQTT. Estado: %d\n", mqtt.state());
    return false;
  }

  mqtt.publish(STATUS_TOPIC, "online", true);
  mqtt.subscribe(ROUTINE_CONFIG_TOPIC, 1);
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    mqtt.subscribe(zones[i].commandTopic, 1);
  }

  return true;
}

float readBatteryVoltage() {
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);

  uint32_t accumulated = 0;
  constexpr uint8_t samples = 16;
  for (uint8_t i = 0; i < samples; ++i) {
    accumulated += analogRead(BATTERY_PIN);
    delay(4);
  }

  const float raw = static_cast<float>(accumulated) / samples;
  return (raw / ADC_MAX) * ADC_REFERENCE_V * BATTERY_DIVIDER_FACTOR;
}

uint8_t batteryPercent(float voltage) {
  if (voltage <= BATTERY_EMPTY_V) {
    return 0;
  }
  if (voltage >= BATTERY_FULL_V) {
    return 100;
  }

  return static_cast<uint8_t>(((voltage - BATTERY_EMPTY_V) * 100.0f) / (BATTERY_FULL_V - BATTERY_EMPTY_V) + 0.5f);
}

void publishBattery() {
  const float voltage = readBatteryVoltage();
  const uint8_t percent = batteryPercent(voltage);

  char payload[64] = {};
  snprintf(payload, sizeof(payload), "{\"voltage\":%.2f,\"percent\":%u}", voltage, percent);
  mqtt.publish(BATTERY_TOPIC, payload, true);
  Serial.printf("Bateria: %.2f V, %u %%\n", voltage, percent);
}

void waitForRetainedCommands() {
  const uint32_t start = millis();
  while (mqtt.connected() && millis() - start < MQTT_LISTEN_WINDOW_MS) {
    mqtt.loop();
    delay(10);
  }
}

void goToSleep(uint64_t sleepMinutes) {
  setAllValvePinsLow();

  if (mqtt.connected()) {
    mqtt.publish(STATUS_TOPIC, "sleeping", true);
    mqtt.loop();
    mqtt.disconnect();
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.printf("Deep sleep durante %llu minutos\n", sleepMinutes);
  Serial.flush();
  esp_sleep_enable_timer_wakeup(sleepMinutes * US_PER_MINUTE);
  esp_deep_sleep_start();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Riegos CC2 - ciclo de despertar");

  configureValvePins();

  if (routine.active) {
    advanceRoutineAfterSleep();
  }

  if (connectWiFi() && connectMqtt()) {
    publishBattery();
    publishRoutineState(routine.active ? "watering" : (routineFinishedThisWake ? "finished" : "idle"));
    waitForRetainedCommands();
  }

  goToSleep(nextSleepMinutes);
}

void loop() {
  // El firmware trabaja por ciclos de despertar y vuelve a deep sleep desde setup().
}
