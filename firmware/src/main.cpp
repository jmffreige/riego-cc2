#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include <cctype>
#include <cstdint>
#include <cstring>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"
#endif

#include "secrets.h"

namespace {

constexpr uint8_t ZONE_COUNT = 4;
constexpr uint8_t MAX_ROUTINE_STEPS = 8;
constexpr uint8_t MAX_PROGRAM_ROUTINES = 8;
constexpr uint8_t BATTERY_PIN = 34;
constexpr uint16_t VALVE_PULSE_MS = 50;
constexpr uint32_t SAFE_BOOT_CLOSE_RECHARGE_MS = 2000;
constexpr uint32_t VALVE_RECHARGE_MS = 5000;
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
constexpr uint32_t MQTT_TIMEOUT_MS = 15000;
constexpr uint32_t MQTT_LISTEN_WINDOW_MS = 7000;
constexpr uint32_t OTA_TIMEOUT_MS = 60000;
constexpr uint32_t NTP_TIMEOUT_MS = 8000;
constexpr uint64_t DEFAULT_SLEEP_MINUTES = 5;
constexpr uint64_t DEFAULT_SLEEP_SECONDS = DEFAULT_SLEEP_MINUTES * 60ULL;
constexpr uint64_t ROUTINE_POLL_SECONDS = 60;
constexpr uint64_t SCHEDULE_WAKE_EARLY_SECONDS = 5;
constexpr uint64_t WAKE_GRACE_SECONDS = 180;
constexpr uint64_t US_PER_SECOND = 1000ULL * 1000ULL;

// External 100k/100k divider from the 1S2P Li-ion pack to GPIO34.
// Calibrated from 2.08-2.09 V measured at GPIO34 while the ADC reported 3.99 V.
constexpr float ADC_REFERENCE_V = 3.3f;
constexpr float ADC_MAX = 4095.0f;
constexpr float BATTERY_DIVIDER_FACTOR = 2.09f;
constexpr float BATTERY_EMPTY_V = 3.20f;
constexpr float BATTERY_FULL_V = 4.20f;
constexpr uint8_t OTA_MIN_BATTERY_PERCENT = 60;

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
  uint32_t programHash;
  uint8_t stepCount;
  uint8_t currentStepIndex;
  int8_t openZoneIndex;
  uint32_t currentStepRemainingSeconds;
  RoutineStep steps[MAX_ROUTINE_STEPS];
};

struct ScheduledRoutine {
  bool enabled;
  uint32_t hash;
  uint8_t dayMask;
  uint8_t startHour;
  uint8_t startMinute;
  uint8_t stepCount;
  RoutineStep steps[MAX_ROUTINE_STEPS];
};

struct RetainedCheckProblem {
  bool pending;
  uint32_t count;
  uint64_t occurredAtEpoch;
  uint32_t retrySeconds;
  char code[24];
  char operation[24];
  char detail[112];
};

struct OtaCommand {
  bool pending;
  char version[40];
  char url[256];
  char sha256[65];
};

const ValveZone zones[ZONE_COUNT] = {
  {16, 4, "riego/zona1/cmd", "riego/zona1/state"},
  {18, 17, "riego/zona2/cmd", "riego/zona2/state"},
  {27, 26, "riego/zona3/cmd", "riego/zona3/state"},
  {33, 32, "riego/zona4/cmd", "riego/zona4/state"},
};

const char* ROUTINE_CONFIG_TOPIC = "riego/routine/config";
const char* PROGRAM_CONFIG_TOPIC = "riego/programacion/cmd";
const char* ROUTINE_STATE_TOPIC = "riego/routine/state";
const char* OTA_COMMAND_TOPIC = "riego/device/ota/cmd";
const char* OTA_STATE_TOPIC = "riego/device/ota/state";
const char* BATTERY_TOPIC = "riego/device/battery";
const char* STATUS_TOPIC = "riego/device/status";
const char* SLEEP_TOPIC = "riego/device/sleep";
const char* PROBLEM_TOPIC = "riego/device/problem";
const char* CLIENT_ID = "ESP32_Riegos_CC2";
const char* TZ_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3";
const char* FIRMWARE_VERSION_STRING = FIRMWARE_VERSION;

WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

RTC_DATA_ATTR int8_t lastAppliedState[ZONE_COUNT] = {-1, -1, -1, -1};
RTC_DATA_ATTR RoutineRuntime routine = {false, 0, 0, 0, 0, -1, 0, {}};
RTC_DATA_ATTR uint32_t completedRoutineId = 0;
RTC_DATA_ATTR ScheduledRoutine programRoutines[MAX_PROGRAM_ROUTINES] = {};
RTC_DATA_ATTR uint8_t programRoutineCount = 0;
RTC_DATA_ATTR uint32_t lastStartedRoutineHash = 0;
RTC_DATA_ATTR int32_t lastStartedDayKey = -1;
RTC_DATA_ATTR RetainedCheckProblem retainedCheckProblem = {};
RTC_DATA_ATTR bool retainedProblemResolutionPublished = false;
RTC_DATA_ATTR uint32_t lastRoutineSleepSeconds = 0;
RTC_DATA_ATTR char lastRoutineStatus[16] = "idle";

uint64_t nextSleepSeconds = DEFAULT_SLEEP_SECONDS;
bool routineFinishedThisWake = false;
bool clockReady = false;
OtaCommand pendingOta = {};
uint8_t lastBatteryPercent = 0;
float lastBatteryVoltage = 0.0f;

void flushMqttPublishes(uint32_t durationMs);
void waitForValveRecharge(const char* reason);
void clearRoutine(bool resetSummary = true);

gpio_num_t valveGpio(uint8_t pin) {
  return static_cast<gpio_num_t>(pin);
}

void disableValvePinHolds() {
  gpio_deep_sleep_hold_dis();
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    gpio_hold_dis(valveGpio(zones[i].openPin));
    gpio_hold_dis(valveGpio(zones[i].closePin));
  }
}

void setAllValvePinsLow() {
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    digitalWrite(zones[i].openPin, LOW);
    digitalWrite(zones[i].closePin, LOW);
  }
}

void enableValvePinHoldsForSleep() {
  setAllValvePinsLow();
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    gpio_hold_en(valveGpio(zones[i].openPin));
    gpio_hold_en(valveGpio(zones[i].closePin));
  }
  gpio_deep_sleep_hold_en();
}

void configureValvePins() {
  disableValvePinHolds();
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    gpio_pullup_dis(valveGpio(zones[i].openPin));
    gpio_pullup_dis(valveGpio(zones[i].closePin));
    gpio_pulldown_en(valveGpio(zones[i].openPin));
    gpio_pulldown_en(valveGpio(zones[i].closePin));
    pinMode(zones[i].openPin, OUTPUT);
    pinMode(zones[i].closePin, OUTPUT);
  }
  setAllValvePinsLow();
}

bool isTimerWakeFromDeepSleep() {
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

void pulseValve(uint8_t zoneIndex, bool openValve) {
  if (zoneIndex >= ZONE_COUNT) {
    return;
  }

  const ValveZone& zone = zones[zoneIndex];
  Serial.printf("Zona %u: pulso de %s\n", zoneIndex + 1, openValve ? "apertura" : "cierre");

  disableValvePinHolds();
  setAllValvePinsLow();
  digitalWrite(zone.openPin, openValve ? HIGH : LOW);
  digitalWrite(zone.closePin, openValve ? LOW : HIGH);
  delay(VALVE_PULSE_MS);
  setAllValvePinsLow();

  lastAppliedState[zoneIndex] = openValve ? 1 : 0;
}

void closeAllValvesPhysical(const char* reason, uint32_t rechargeMs = VALVE_RECHARGE_MS) {
  Serial.println(reason);
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    pulseValve(i, false);
    if (i + 1 < ZONE_COUNT) {
      Serial.printf("Esperando %lu ms antes de cerrar la siguiente zona\n", static_cast<unsigned long>(rechargeMs));
      delay(rechargeMs);
    }
  }
}

void closeAllValvesForSafeBoot() {
  closeAllValvesPhysical("Arranque no temporizado: cierre fisico rapido de seguridad de todas las zonas",
                         SAFE_BOOT_CLOSE_RECHARGE_MS);

  clearRoutine();
  completedRoutineId = 0;
  routineFinishedThisWake = false;
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    lastAppliedState[i] = 0;
  }
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
  if (zoneIndex >= ZONE_COUNT) {
    return;
  }
  lastAppliedState[zoneIndex] = isOpen ? 1 : 0;
  if (!mqtt.connected()) {
    return;
  }
  mqtt.publish(zones[zoneIndex].stateTopic, isOpen ? "ON" : "OFF", true);
}

void publishRoutineState(const char* status) {
  strncpy(lastRoutineStatus, status ? status : "idle", sizeof(lastRoutineStatus) - 1);
  lastRoutineStatus[sizeof(lastRoutineStatus) - 1] = '\0';
  if (!mqtt.connected()) {
    return;
  }

  char payload[160] = {};
  const int8_t openZone = routine.openZoneIndex >= 0 ? routine.openZoneIndex + 1 : 0;
  snprintf(payload, sizeof(payload),
           "{\"status\":\"%s\",\"id\":%lu,\"step\":%u,\"stepCount\":%u,\"openZone\":%d,\"nextWakeSeconds\":%llu}",
           status, static_cast<unsigned long>(routine.id), routine.currentStepIndex + 1, routine.stepCount, openZone,
           static_cast<unsigned long long>(nextSleepSeconds));
  mqtt.publish(ROUTINE_STATE_TOPIC, payload, true);
}

void publishKnownZoneStates() {
  if (!mqtt.connected()) {
    return;
  }

  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    if (lastAppliedState[i] >= 0) {
      mqtt.publish(zones[i].stateTopic, lastAppliedState[i] == 1 ? "ON" : "OFF", true);
    }
  }
}

void publishOtaState(const char* status, const char* reason = "", const char* version = "", int progress = -1) {
  if (!mqtt.connected()) {
    return;
  }

  char payload[256] = {};
  snprintf(payload, sizeof(payload),
           "{\"status\":\"%s\",\"version\":\"%s\",\"currentVersion\":\"%s\",\"progress\":%d,\"reason\":\"%s\"}",
           status, version ? version : "", FIRMWARE_VERSION_STRING, progress, reason ? reason : "");
  mqtt.publish(OTA_STATE_TOPIC, payload, true);
  flushMqttPublishes(250);
  Serial.printf("Estado OTA: %s\n", payload);
}

bool isHexSha256(const char* value) {
  if (!value || strlen(value) != 64) {
    return false;
  }
  for (uint8_t i = 0; i < 64; ++i) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

bool isSafeVersionToken(const char* value) {
  if (!value || value[0] == '\0' || strlen(value) >= sizeof(pendingOta.version)) {
    return false;
  }
  for (uint8_t i = 0; value[i] != '\0'; ++i) {
    const char c = value[i];
    const bool safe = isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-';
    if (!safe) {
      return false;
    }
  }
  return true;
}

void normalizeSha256(char* value) {
  for (uint8_t i = 0; value[i] != '\0'; ++i) {
    value[i] = static_cast<char>(tolower(static_cast<unsigned char>(value[i])));
  }
}

void bytesToHex(const uint8_t* bytes, size_t length, char* output, size_t outputSize) {
  static const char* hex = "0123456789abcdef";
  if (outputSize < length * 2 + 1) {
    return;
  }
  for (size_t i = 0; i < length; ++i) {
    output[i * 2] = hex[(bytes[i] >> 4) & 0x0F];
    output[i * 2 + 1] = hex[bytes[i] & 0x0F];
  }
  output[length * 2] = '\0';
}

void copyProblemText(char* target, size_t targetSize, const char* value) {
  if (targetSize == 0) {
    return;
  }
  strncpy(target, value ? value : "", targetSize - 1);
  target[targetSize - 1] = '\0';
}

void recordRetainedCheckProblem(const char* code, const char* operation, const char* detail, uint64_t retrySeconds) {
  retainedCheckProblem.pending = true;
  retainedCheckProblem.count++;
  retainedCheckProblem.occurredAtEpoch = clockReady ? static_cast<uint64_t>(time(nullptr)) : 0;
  retainedCheckProblem.retrySeconds = retrySeconds > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(retrySeconds);
  copyProblemText(retainedCheckProblem.code, sizeof(retainedCheckProblem.code), code);
  copyProblemText(retainedCheckProblem.operation, sizeof(retainedCheckProblem.operation), operation);
  copyProblemText(retainedCheckProblem.detail, sizeof(retainedCheckProblem.detail), detail);
}

void publishRetainedCheckProblem(bool resolved) {
  if (!mqtt.connected() || !retainedCheckProblem.pending) {
    return;
  }

  char payload[320] = {};
  const uint64_t resolvedAtEpoch = clockReady ? static_cast<uint64_t>(time(nullptr)) : 0;
  const uint64_t retryAtEpoch = retainedCheckProblem.occurredAtEpoch == 0
                                  ? 0
                                  : retainedCheckProblem.occurredAtEpoch + retainedCheckProblem.retrySeconds;
  snprintf(payload, sizeof(payload),
           "{\"active\":%s,\"resolved\":%s,\"code\":\"%s\",\"operation\":\"%s\",\"detail\":\"%s\","
           "\"count\":%lu,\"occurredAtEpoch\":%llu,\"retrySeconds\":%lu,\"retryAtEpoch\":%llu,"
           "\"resolvedAtEpoch\":%llu}",
           resolved ? "false" : "true", resolved ? "true" : "false", retainedCheckProblem.code,
           retainedCheckProblem.operation, retainedCheckProblem.detail,
           static_cast<unsigned long>(retainedCheckProblem.count),
           static_cast<unsigned long long>(retainedCheckProblem.occurredAtEpoch),
           static_cast<unsigned long>(retainedCheckProblem.retrySeconds),
           static_cast<unsigned long long>(retryAtEpoch), static_cast<unsigned long long>(resolvedAtEpoch));
  mqtt.publish(PROBLEM_TOPIC, payload, true);

  if (resolved) {
    memset(&retainedCheckProblem, 0, sizeof(retainedCheckProblem));
    retainedProblemResolutionPublished = true;
  }
}

void publishNoProblemState() {
  if (!mqtt.connected()) {
    return;
  }

  mqtt.publish(PROBLEM_TOPIC, "{\"active\":false,\"resolved\":false}", true);
  retainedProblemResolutionPublished = false;
}

const char* sleepWakeReason(uint64_t sleepSeconds) {
  if (routine.active && routine.openZoneIndex >= 0) {
    if (routine.currentStepRemainingSeconds > sleepSeconds) {
      return "routine_check";
    }
    return "routine_step";
  }
  if (!routine.active && sleepSeconds < DEFAULT_SLEEP_SECONDS) {
    return "scheduled_start";
  }
  return "poll";
}

void publishSleepTelemetry(uint64_t sleepSeconds) {
  if (!mqtt.connected()) {
    return;
  }

  char payload[192] = {};
  const char* reason = sleepWakeReason(sleepSeconds);
  if (clockReady) {
    const uint64_t publishedAtEpoch = static_cast<uint64_t>(time(nullptr));
    const uint64_t wakeAtEpoch = publishedAtEpoch + sleepSeconds;
    snprintf(payload, sizeof(payload),
             "{\"sleepSeconds\":%llu,\"publishedAtEpoch\":%llu,\"wakeAtEpoch\":%llu,\"reason\":\"%s\","
             "\"routineActive\":%s}",
             static_cast<unsigned long long>(sleepSeconds), static_cast<unsigned long long>(publishedAtEpoch),
             static_cast<unsigned long long>(wakeAtEpoch), reason, routine.active ? "true" : "false");
  } else {
    snprintf(payload, sizeof(payload),
             "{\"sleepSeconds\":%llu,\"publishedAtEpoch\":null,\"wakeAtEpoch\":null,\"reason\":\"%s\","
             "\"routineActive\":%s}",
             static_cast<unsigned long long>(sleepSeconds), reason, routine.active ? "true" : "false");
  }

  mqtt.publish(SLEEP_TOPIC, payload, true);
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

void waitForValveRecharge(const char* reason) {
  Serial.printf("Esperando %lu ms para recargar condensador antes de %s\n",
                static_cast<unsigned long>(VALVE_RECHARGE_MS),
                reason);
  delay(VALVE_RECHARGE_MS);
}

void clearRoutine(bool resetSummary) {
  routine.active = false;
  routine.openZoneIndex = -1;
  routine.currentStepRemainingSeconds = 0;
  lastRoutineSleepSeconds = 0;
  strncpy(lastRoutineStatus, "idle", sizeof(lastRoutineStatus) - 1);
  lastRoutineStatus[sizeof(lastRoutineStatus) - 1] = '\0';
  if (resetSummary) {
    routine.stepCount = 0;
    routine.currentStepIndex = 0;
    routine.programHash = 0;
  }
  nextSleepSeconds = DEFAULT_SLEEP_SECONDS;
}

uint64_t nextRoutineSleepSeconds() {
  if (!routine.active || routine.openZoneIndex < 0) {
    return DEFAULT_SLEEP_SECONDS;
  }

  if (routine.currentStepRemainingSeconds == 0) {
    return 1;
  }

  return routine.currentStepRemainingSeconds > ROUTINE_POLL_SECONDS ? ROUTINE_POLL_SECONDS
                                                                    : routine.currentStepRemainingSeconds;
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
  routine.currentStepRemainingSeconds = static_cast<uint32_t>(step.durationMinutes) * 60UL;
  nextSleepSeconds = nextRoutineSleepSeconds();
  publishRoutineState("watering");
}

void advanceRoutineAfterCheck(bool resumeCurrentStep = false) {
  if (!routine.active) {
    return;
  }

  Serial.println("Rutina activa: programando siguiente zona segun temporizador local");

  const uint8_t nextStepIndex = resumeCurrentStep ? routine.currentStepIndex : routine.currentStepIndex + 1;
  if (nextStepIndex >= routine.stepCount) {
    Serial.println("Rutina finalizada");
    completedRoutineId = routine.id;
    routineFinishedThisWake = true;
    clearRoutine(false);
    publishRoutineState("finished");
    return;
  }

  if (!resumeCurrentStep) {
    waitForValveRecharge("abrir la siguiente zona");
  }
  startRoutineStep(nextStepIndex);
}

uint32_t fnv1aUpdate(uint32_t hash, const char* value) {
  while (value && *value) {
    hash ^= static_cast<uint8_t>(*value++);
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t fnv1aUpdateByte(uint32_t hash, uint8_t value) {
  hash ^= value;
  hash *= 16777619UL;
  return hash;
}

uint32_t scheduledRoutineHash(JsonObject routineDoc, const RoutineStep* steps, uint8_t stepCount, uint8_t dayMask,
                              uint8_t startHour, uint8_t startMinute) {
  uint32_t hash = 2166136261UL;
  hash = fnv1aUpdate(hash, routineDoc["id"] | "");
  hash = fnv1aUpdate(hash, routineDoc["name"] | "");
  hash = fnv1aUpdateByte(hash, dayMask);
  hash = fnv1aUpdateByte(hash, startHour);
  hash = fnv1aUpdateByte(hash, startMinute);
  hash = fnv1aUpdateByte(hash, stepCount);
  for (uint8_t i = 0; i < stepCount; ++i) {
    hash = fnv1aUpdateByte(hash, steps[i].zoneIndex);
    hash = fnv1aUpdateByte(hash, static_cast<uint8_t>(steps[i].durationMinutes & 0xFF));
    hash = fnv1aUpdateByte(hash, static_cast<uint8_t>(steps[i].durationMinutes >> 8));
  }
  return hash == 0 ? 1 : hash;
}

bool parseStartTime(const char* startTime, uint8_t& hour, uint8_t& minute) {
  int parsedHour = -1;
  int parsedMinute = -1;
  if (!startTime || sscanf(startTime, "%d:%d", &parsedHour, &parsedMinute) != 2) {
    return false;
  }
  if (parsedHour < 0 || parsedHour > 23 || parsedMinute < 0 || parsedMinute > 59) {
    return false;
  }
  hour = static_cast<uint8_t>(parsedHour);
  minute = static_cast<uint8_t>(parsedMinute);
  return true;
}

int32_t localDayKey(time_t value) {
  tm localTime = {};
  localtime_r(&value, &localTime);
  return (localTime.tm_year + 1900) * 400 + localTime.tm_yday;
}

bool dayMatches(const ScheduledRoutine& scheduled, const tm& localTime) {
  return (scheduled.dayMask & (1U << localTime.tm_wday)) != 0;
}

bool findScheduledRoutineByHash(uint32_t hash) {
  for (uint8_t i = 0; i < programRoutineCount; ++i) {
    if (programRoutines[i].enabled && programRoutines[i].hash == hash) {
      return true;
    }
  }
  return false;
}

bool secondsUntilRoutine(const ScheduledRoutine& scheduled, time_t now, int64_t& secondsUntil, int32_t& dayKey) {
  for (uint8_t dayOffset = 0; dayOffset <= 7; ++dayOffset) {
    const time_t probe = now + static_cast<time_t>(dayOffset) * 24L * 60L * 60L;
    tm candidateLocal = {};
    localtime_r(&probe, &candidateLocal);
    if (!dayMatches(scheduled, candidateLocal)) {
      continue;
    }

    candidateLocal.tm_hour = scheduled.startHour;
    candidateLocal.tm_min = scheduled.startMinute;
    candidateLocal.tm_sec = 0;
    candidateLocal.tm_isdst = -1;
    const time_t candidate = mktime(&candidateLocal);
    const int64_t diff = static_cast<int64_t>(candidate) - static_cast<int64_t>(now);
    if (diff < -static_cast<int64_t>(WAKE_GRACE_SECONDS)) {
      continue;
    }

    secondsUntil = diff < 0 ? 0 : diff;
    dayKey = localDayKey(candidate);
    return true;
  }
  return false;
}

void startScheduledRoutine(const ScheduledRoutine& scheduled, int32_t dayKey) {
  Serial.printf("Arrancando rutina programada %lu\n", static_cast<unsigned long>(scheduled.hash));
  const bool hadOpenZone = routine.openZoneIndex >= 0;
  closeRoutineOpenZone();
  if (hadOpenZone) {
    waitForValveRecharge("abrir la rutina programada");
  }

  routine.active = true;
  routine.id = scheduled.hash;
  routine.programHash = scheduled.hash;
  routine.stepCount = scheduled.stepCount;
  routine.currentStepIndex = 0;
  routine.openZoneIndex = -1;
  completedRoutineId = 0;
  routineFinishedThisWake = false;
  for (uint8_t i = 0; i < scheduled.stepCount; ++i) {
    routine.steps[i] = scheduled.steps[i];
  }

  lastStartedRoutineHash = scheduled.hash;
  lastStartedDayKey = dayKey;
  startRoutineStep(0);
}

void evaluateScheduledProgram() {
  if (routine.active || !clockReady || programRoutineCount == 0) {
    return;
  }

  time_t now = time(nullptr);
  const ScheduledRoutine* nextRoutine = nullptr;
  int64_t bestSecondsUntil = INT64_MAX;
  int32_t bestDayKey = -1;

  for (uint8_t i = 0; i < programRoutineCount; ++i) {
    const ScheduledRoutine& scheduled = programRoutines[i];
    if (!scheduled.enabled || scheduled.stepCount == 0) {
      continue;
    }

    int64_t secondsUntil = 0;
    int32_t dayKey = -1;
    if (!secondsUntilRoutine(scheduled, now, secondsUntil, dayKey)) {
      continue;
    }
    if (scheduled.hash == lastStartedRoutineHash && dayKey == lastStartedDayKey) {
      continue;
    }
    if (secondsUntil < bestSecondsUntil) {
      bestSecondsUntil = secondsUntil;
      bestDayKey = dayKey;
      nextRoutine = &scheduled;
    }
  }

  if (!nextRoutine) {
    nextSleepSeconds = DEFAULT_SLEEP_SECONDS;
    return;
  }

  if (bestSecondsUntil == 0) {
    startScheduledRoutine(*nextRoutine, bestDayKey);
    return;
  }

  if (bestSecondsUntil <= static_cast<int64_t>(DEFAULT_SLEEP_SECONDS)) {
    nextSleepSeconds = bestSecondsUntil > static_cast<int64_t>(SCHEDULE_WAKE_EARLY_SECONDS)
                         ? static_cast<uint64_t>(bestSecondsUntil) - SCHEDULE_WAKE_EARLY_SECONDS
                         : 1;
    Serial.printf("Proxima rutina en %lld segundos; despertando %llu segundos antes\n", bestSecondsUntil,
                  static_cast<unsigned long long>(SCHEDULE_WAKE_EARLY_SECONDS));
    publishRoutineState("scheduled");
  } else {
    nextSleepSeconds = DEFAULT_SLEEP_SECONDS;
  }
}

void applyProgramConfig(const byte* payload, unsigned int length) {
  if (length == 0) {
    Serial.println("Programacion borrada desde MQTT");
    memset(programRoutines, 0, sizeof(programRoutines));
    programRoutineCount = 0;
    if (routine.active && routine.programHash != 0) {
      closeRoutineOpenZone();
      clearRoutine();
      publishRoutineState("stopped");
    }
    return;
  }

  DynamicJsonDocument doc(4096);
  const DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.printf("Programacion ignorada: JSON no valido (%s)\n", error.c_str());
    return;
  }

  JsonArray routines = doc["routines"].as<JsonArray>();
  if (routines.isNull()) {
    Serial.println("Programacion ignorada: falta routines");
    return;
  }

  ScheduledRoutine parsed[MAX_PROGRAM_ROUTINES] = {};
  uint8_t parsedCount = 0;

  for (JsonObject routineDoc : routines) {
    if (parsedCount >= MAX_PROGRAM_ROUTINES) {
      break;
    }
    if (!(routineDoc["enabled"] | false)) {
      continue;
    }

    uint8_t startHour = 0;
    uint8_t startMinute = 0;
    if (!parseStartTime(routineDoc["startTime"] | "", startHour, startMinute)) {
      Serial.println("Rutina programada ignorada: startTime no valido");
      continue;
    }

    uint8_t dayMask = 0;
    const char* dayMode = routineDoc["dayMode"] | "daily";
    if (strcmp(dayMode, "selected") == 0) {
      JsonArray days = routineDoc["days"].as<JsonArray>();
      for (JsonVariant day : days) {
        const int value = day.as<int>();
        if (value >= 0 && value <= 6) {
          dayMask |= static_cast<uint8_t>(1U << value);
        }
      }
    } else {
      dayMask = 0x7F;
    }
    if (dayMask == 0) {
      Serial.println("Rutina programada ignorada: sin dias activos");
      continue;
    }

    JsonArray durations = routineDoc["durations"].as<JsonArray>();
    if (durations.isNull() || durations.size() < ZONE_COUNT) {
      Serial.println("Rutina programada ignorada: falta durations");
      continue;
    }

    RoutineStep steps[MAX_ROUTINE_STEPS] = {};
    uint8_t stepCount = 0;
    for (uint8_t zone = 0; zone < ZONE_COUNT && stepCount < MAX_ROUTINE_STEPS; ++zone) {
      const int minutes = durations[zone] | 0;
      if (minutes < 0 || minutes > 180) {
        stepCount = 0;
        break;
      }
      if (minutes > 0) {
        steps[stepCount++] = {zone, static_cast<uint16_t>(minutes)};
      }
    }
    if (stepCount == 0) {
      Serial.println("Rutina programada ignorada: sin zonas con duracion");
      continue;
    }

    ScheduledRoutine& target = parsed[parsedCount++];
    target.enabled = true;
    target.dayMask = dayMask;
    target.startHour = startHour;
    target.startMinute = startMinute;
    target.stepCount = stepCount;
    for (uint8_t i = 0; i < stepCount; ++i) {
      target.steps[i] = steps[i];
    }
    target.hash = scheduledRoutineHash(routineDoc, steps, stepCount, dayMask, startHour, startMinute);
  }

  memset(programRoutines, 0, sizeof(programRoutines));
  for (uint8_t i = 0; i < parsedCount; ++i) {
    programRoutines[i] = parsed[i];
  }
  programRoutineCount = parsedCount;
  Serial.printf("Programacion recibida: %u rutinas activas\n", programRoutineCount);

  if (routine.active && routine.programHash != 0 && !findScheduledRoutineByHash(routine.programHash)) {
    Serial.println("Rutina en curso detenida: la programacion cambio o fue desactivada");
    closeRoutineOpenZone();
    clearRoutine();
    publishRoutineState("stopped");
  }
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
    if (routine.active) {
      closeRoutineOpenZone();
      clearRoutine();
      publishRoutineState("disabled");
    } else {
      clearRoutine();
      publishRoutineState("idle");
    }
    completedRoutineId = 0;
    routineFinishedThisWake = false;
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
  const bool hadOpenZone = routine.openZoneIndex >= 0;
  closeRoutineOpenZone();
  if (hadOpenZone) {
    waitForValveRecharge("abrir la rutina inmediata");
  }

  routine.active = true;
  routine.id = id;
  routine.programHash = 0;
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

void applyOtaCommand(const byte* payload, unsigned int length) {
  if (length == 0) {
    pendingOta.pending = false;
    publishOtaState("idle");
    return;
  }

  StaticJsonDocument<768> doc;
  const DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    publishOtaState("failed", "invalid_json");
    return;
  }

  if (!(doc["enabled"] | true)) {
    pendingOta.pending = false;
    publishOtaState("idle");
    return;
  }

  const char* version = doc["version"] | "";
  const char* url = doc["url"] | "";
  const char* sha256 = doc["sha256"] | "";
  if (version[0] == '\0' || url[0] == '\0' || sha256[0] == '\0') {
    publishOtaState("failed", "missing_fields");
    return;
  }
  if (!isSafeVersionToken(version)) {
    publishOtaState("failed", "invalid_version");
    return;
  }
  if (strlen(url) >= sizeof(pendingOta.url)) {
    publishOtaState("failed", "url_too_long", version);
    return;
  }
  if (strncmp(url, "https://", 8) != 0) {
    publishOtaState("failed", "invalid_url", version);
    return;
  }
  if (!isHexSha256(sha256)) {
    publishOtaState("failed", "invalid_sha256", version);
    return;
  }

  strncpy(pendingOta.version, version, sizeof(pendingOta.version) - 1);
  pendingOta.version[sizeof(pendingOta.version) - 1] = '\0';
  strncpy(pendingOta.url, url, sizeof(pendingOta.url) - 1);
  pendingOta.url[sizeof(pendingOta.url) - 1] = '\0';
  strncpy(pendingOta.sha256, sha256, sizeof(pendingOta.sha256) - 1);
  pendingOta.sha256[sizeof(pendingOta.sha256) - 1] = '\0';
  normalizeSha256(pendingOta.sha256);
  pendingOta.pending = true;
  publishOtaState("queued", "", pendingOta.version);
}

bool runHttpOta() {
  WiFiClientSecure otaClient;
  otaClient.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  http.setUserAgent("riego-cc2-esp32-ota");

  if (!http.begin(otaClient, pendingOta.url)) {
    publishOtaState("failed", "http_begin", pendingOta.version);
    return false;
  }

  publishOtaState("downloading", "", pendingOta.version, 0);
  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    char reason[28] = {};
    snprintf(reason, sizeof(reason), "http_%d", httpCode);
    publishOtaState("failed", reason, pendingOta.version);
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0) {
    publishOtaState("failed", "no_content_length", pendingOta.version);
    http.end();
    return false;
  }

  if (!Update.begin(static_cast<size_t>(contentLength), U_FLASH)) {
    publishOtaState("failed", "no_ota_space", pendingOta.version);
    http.end();
    return false;
  }

  mbedtls_sha256_context shaContext;
  mbedtls_sha256_init(&shaContext);
  mbedtls_sha256_starts_ret(&shaContext, 0);

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[1024] = {};
  size_t written = 0;
  int lastProgress = -1;
  const uint32_t startMs = millis();

  while (http.connected() && written < static_cast<size_t>(contentLength)) {
    if (millis() - startMs > OTA_TIMEOUT_MS) {
      Update.abort();
      mbedtls_sha256_free(&shaContext);
      publishOtaState("failed", "timeout", pendingOta.version);
      http.end();
      return false;
    }

    const size_t available = stream->available();
    if (available == 0) {
      mqtt.loop();
      delay(10);
      continue;
    }

    const size_t toRead = min(sizeof(buffer), min(available, static_cast<size_t>(contentLength) - written));
    const size_t readBytes = stream->readBytes(buffer, toRead);
    if (readBytes == 0) {
      continue;
    }

    if (Update.write(buffer, readBytes) != readBytes) {
      Update.abort();
      mbedtls_sha256_free(&shaContext);
      publishOtaState("failed", "flash_write", pendingOta.version);
      http.end();
      return false;
    }

    mbedtls_sha256_update_ret(&shaContext, buffer, readBytes);
    written += readBytes;
    const int progress = static_cast<int>((written * 100ULL) / static_cast<size_t>(contentLength));
    if (progress >= lastProgress + 20 || progress == 100) {
      lastProgress = progress;
      publishOtaState("downloading", "", pendingOta.version, progress);
    }
  }

  uint8_t digest[32] = {};
  char calculatedSha[65] = {};
  mbedtls_sha256_finish_ret(&shaContext, digest);
  mbedtls_sha256_free(&shaContext);
  bytesToHex(digest, sizeof(digest), calculatedSha, sizeof(calculatedSha));

  if (written != static_cast<size_t>(contentLength)) {
    Update.abort();
    publishOtaState("failed", "incomplete_download", pendingOta.version);
    http.end();
    return false;
  }

  if (strcmp(calculatedSha, pendingOta.sha256) != 0) {
    Update.abort();
    publishOtaState("failed", "sha256_mismatch", pendingOta.version);
    http.end();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("OTA Update.end fallo: %s\n", Update.errorString());
    publishOtaState("failed", "update_end", pendingOta.version);
    http.end();
    return false;
  }

  http.end();
  publishOtaState("updated", "", pendingOta.version, 100);
  mqtt.publish(STATUS_TOPIC, "restarting", true);
  flushMqttPublishes(1000);
  delay(250);
  ESP.restart();
  return true;
}

bool processPendingOta() {
  if (!pendingOta.pending) {
    return false;
  }

  if (strcmp(pendingOta.version, FIRMWARE_VERSION_STRING) == 0) {
    publishOtaState("skipped", "up_to_date", pendingOta.version);
    pendingOta.pending = false;
    return false;
  }

  if (routine.active) {
    publishOtaState("failed", "routine_active", pendingOta.version);
    pendingOta.pending = false;
    return false;
  }

  if (lastBatteryPercent < OTA_MIN_BATTERY_PERCENT) {
    publishOtaState("failed", "battery_low", pendingOta.version);
    pendingOta.pending = false;
    return false;
  }

  publishOtaState("checking", "", pendingOta.version);
  const bool started = runHttpOta();
  pendingOta.pending = false;
  return started;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, PROGRAM_CONFIG_TOPIC) == 0) {
    applyProgramConfig(payload, length);
    return;
  }

  if (strcmp(topic, ROUTINE_CONFIG_TOPIC) == 0) {
    applyRoutineConfig(payload, length);
    return;
  }

  if (strcmp(topic, OTA_COMMAND_TOPIC) == 0) {
    applyOtaCommand(payload, length);
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
      if (!desiredOpen) {
        pulseValve(i, false);
      } else if (lastAppliedState[i] != desiredState) {
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
  mqtt.setBufferSize(4096);
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
  mqtt.subscribe(PROGRAM_CONFIG_TOPIC, 1);
  mqtt.subscribe(ROUTINE_CONFIG_TOPIC, 1);
  mqtt.subscribe(OTA_COMMAND_TOPIC, 1);
  for (uint8_t i = 0; i < ZONE_COUNT; ++i) {
    mqtt.subscribe(zones[i].commandTopic, 1);
  }

  return true;
}

bool syncClock() {
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sincronizando hora");
  const uint32_t start = millis();
  tm timeInfo = {};
  while (millis() - start < NTP_TIMEOUT_MS) {
    if (getLocalTime(&timeInfo, 250)) {
      Serial.printf("\nHora local: %04d-%02d-%02d %02d:%02d:%02d\n", timeInfo.tm_year + 1900, timeInfo.tm_mon + 1,
                    timeInfo.tm_mday, timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
      return true;
    }
    Serial.print(".");
  }
  Serial.println("\nNo se pudo sincronizar hora");
  return false;
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
  lastBatteryVoltage = voltage;
  lastBatteryPercent = percent;

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

void flushMqttPublishes(uint32_t durationMs = 500) {
  const uint32_t start = millis();
  while (mqtt.connected() && millis() - start < durationMs) {
    mqtt.loop();
    delay(25);
  }
}

void goToSleep(uint64_t sleepSeconds) {
  disableValvePinHolds();
  setAllValvePinsLow();

  if (sleepSeconds == 0) {
    sleepSeconds = 1;
  }

  if (mqtt.connected()) {
    publishRetainedCheckProblem(false);
    publishSleepTelemetry(sleepSeconds);
    mqtt.publish(STATUS_TOPIC, "sleeping", true);
    flushMqttPublishes();
    mqtt.disconnect();
  }

  lastRoutineSleepSeconds = (routine.active && routine.openZoneIndex >= 0)
                              ? (sleepSeconds > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sleepSeconds))
                              : 0;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  enableValvePinHoldsForSleep();

  Serial.printf("Deep sleep durante %llu segundos\n", static_cast<unsigned long long>(sleepSeconds));
  Serial.flush();
  esp_sleep_enable_timer_wakeup(sleepSeconds * US_PER_SECOND);
  esp_deep_sleep_start();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Riegos CC2 - ciclo de despertar");

  configureValvePins();
  const bool timerWake = isTimerWakeFromDeepSleep();
  const bool safeBootClosedValves = !timerWake;
  if (!timerWake) {
    closeAllValvesForSafeBoot();
  }

  bool pendingRoutineAdvance = false;
  bool resumeRoutineStep = false;
  bool routinePollWake = false;
  if (timerWake && routine.active) {
    if (routine.openZoneIndex >= 0) {
      if (routine.currentStepRemainingSeconds == 0 && routine.currentStepIndex < routine.stepCount) {
        routine.currentStepRemainingSeconds =
          static_cast<uint32_t>(routine.steps[routine.currentStepIndex].durationMinutes) * 60UL;
      }

      if (lastRoutineSleepSeconds >= routine.currentStepRemainingSeconds) {
        routine.currentStepRemainingSeconds = 0;
      } else {
        routine.currentStepRemainingSeconds -= lastRoutineSleepSeconds;
      }

      if (routine.currentStepRemainingSeconds == 0) {
        Serial.println("Rutina activa: fin de zona, cerrando antes de comprobar cambios");
        closeRoutineOpenZone();
        pendingRoutineAdvance = true;
      } else {
        Serial.printf("Rutina activa: chequeo MQTT intermedio, quedan %lu segundos en la zona actual\n",
                      static_cast<unsigned long>(routine.currentStepRemainingSeconds));
        nextSleepSeconds = nextRoutineSleepSeconds();
        routinePollWake = true;
      }
    } else {
      Serial.println("Rutina activa sin zona abierta: comprobando antes de avanzar");
      pendingRoutineAdvance = true;
      resumeRoutineStep = true;
    }
  }

  const bool wifiConnected = connectWiFi();
  bool mqttReady = false;
  if (wifiConnected) {
    clockReady = syncClock();
    mqttReady = connectMqtt();
  }

  if (mqttReady) {
    publishBattery();
    publishKnownZoneStates();
    publishRoutineState(lastRoutineStatus[0] == '\0' ? "idle" : lastRoutineStatus);
    waitForRetainedCommands();
    if (retainedCheckProblem.pending) {
      publishRetainedCheckProblem(true);
    } else if (retainedProblemResolutionPublished) {
      publishNoProblemState();
    }
    processPendingOta();
  }

  if (pendingRoutineAdvance) {
    if (routine.active) {
      advanceRoutineAfterCheck(resumeRoutineStep);
    } else {
      nextSleepSeconds = DEFAULT_SLEEP_SECONDS;
    }
  } else if (routinePollWake) {
    if (routine.active) {
      nextSleepSeconds = nextRoutineSleepSeconds();
      publishRoutineState("watering");
    } else {
      nextSleepSeconds = DEFAULT_SLEEP_SECONDS;
    }
  } else {
    evaluateScheduledProgram();
  }

  if (!wifiConnected) {
    recordRetainedCheckProblem("wifi_failed", pendingRoutineAdvance ? "routine_check" : "retained_commands",
                               "No se pudo conectar a WiFi; no se pudieron leer las ordenes retenidas.",
                               nextSleepSeconds);
  } else if (!mqttReady) {
    recordRetainedCheckProblem("mqtt_failed", pendingRoutineAdvance ? "routine_check" : "retained_commands",
                               "No se pudo conectar a MQTT; no se pudieron leer las ordenes retenidas.",
                               nextSleepSeconds);
  } else if (!pendingRoutineAdvance && !clockReady && programRoutineCount > 0) {
    recordRetainedCheckProblem("clock_failed", "scheduled_program",
                               "No se pudo sincronizar la hora; no se pudo comprobar la programacion retenida.",
                               nextSleepSeconds);
  }

  goToSleep(nextSleepSeconds);
}

void loop() {
  // El firmware trabaja por ciclos de despertar y vuelve a deep sleep desde setup().
}
