#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <esp_sleep.h>
#include <time.h>
#include <cstdint>
#include <cstring>

#include "secrets.h"

namespace {

constexpr uint8_t ZONE_COUNT = 4;
constexpr uint8_t MAX_ROUTINE_STEPS = 8;
constexpr uint8_t MAX_PROGRAM_ROUTINES = 8;
constexpr uint8_t BATTERY_PIN = 35;
constexpr uint16_t VALVE_PULSE_MS = 50;
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
constexpr uint32_t MQTT_TIMEOUT_MS = 15000;
constexpr uint32_t MQTT_LISTEN_WINDOW_MS = 7000;
constexpr uint32_t NTP_TIMEOUT_MS = 8000;
constexpr uint64_t DEFAULT_SLEEP_MINUTES = 5;
constexpr uint64_t DEFAULT_SLEEP_SECONDS = DEFAULT_SLEEP_MINUTES * 60ULL;
constexpr uint64_t ROUTINE_POLL_SECONDS = 60;
constexpr uint64_t SCHEDULE_WAKE_EARLY_SECONDS = 5;
constexpr uint64_t WAKE_GRACE_SECONDS = 180;
constexpr uint64_t US_PER_SECOND = 1000ULL * 1000ULL;

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

const ValveZone zones[ZONE_COUNT] = {
  {4, 16, "riego/zona1/cmd", "riego/zona1/state"},
  {17, 18, "riego/zona2/cmd", "riego/zona2/state"},
  {19, 21, "riego/zona3/cmd", "riego/zona3/state"},
  {22, 23, "riego/zona4/cmd", "riego/zona4/state"},
};

const char* ROUTINE_CONFIG_TOPIC = "riego/routine/config";
const char* PROGRAM_CONFIG_TOPIC = "riego/programacion/cmd";
const char* ROUTINE_STATE_TOPIC = "riego/routine/state";
const char* BATTERY_TOPIC = "riego/device/battery";
const char* STATUS_TOPIC = "riego/device/status";
const char* SLEEP_TOPIC = "riego/device/sleep";
const char* PROBLEM_TOPIC = "riego/device/problem";
const char* CLIENT_ID = "ESP32_Riegos_CC2";
const char* TZ_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3";

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
RTC_DATA_ATTR uint32_t lastRoutineSleepSeconds = 0;

uint64_t nextSleepSeconds = DEFAULT_SLEEP_SECONDS;
bool routineFinishedThisWake = false;
bool clockReady = false;

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
           "{\"status\":\"%s\",\"id\":%lu,\"step\":%u,\"stepCount\":%u,\"openZone\":%d,\"nextWakeSeconds\":%llu}",
           status, static_cast<unsigned long>(routine.id), routine.currentStepIndex + 1, routine.stepCount, openZone,
           static_cast<unsigned long long>(nextSleepSeconds));
  mqtt.publish(ROUTINE_STATE_TOPIC, payload, true);
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
  }
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

void clearRoutine(bool resetSummary = true) {
  routine.active = false;
  routine.openZoneIndex = -1;
  routine.currentStepRemainingSeconds = 0;
  lastRoutineSleepSeconds = 0;
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

void advanceRoutineAfterCheck() {
  if (!routine.active) {
    return;
  }

  Serial.println("Rutina activa: programando siguiente zona tras comprobar MQTT");

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
  closeRoutineOpenZone();

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

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, PROGRAM_CONFIG_TOPIC) == 0) {
    applyProgramConfig(payload, length);
    return;
  }

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

void goToSleep(uint64_t sleepSeconds) {
  setAllValvePinsLow();

  if (sleepSeconds == 0) {
    sleepSeconds = 1;
  }

  if (mqtt.connected()) {
    publishRetainedCheckProblem(false);
    publishSleepTelemetry(sleepSeconds);
    mqtt.publish(STATUS_TOPIC, "sleeping", true);
    mqtt.loop();
    mqtt.disconnect();
  }

  lastRoutineSleepSeconds = (routine.active && routine.openZoneIndex >= 0)
                              ? (sleepSeconds > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sleepSeconds))
                              : 0;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

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

  bool pendingRoutineAdvance = false;
  bool routinePollWake = false;
  if (routine.active) {
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
    publishRoutineState(routine.active ? "watering" : (routineFinishedThisWake ? "finished" : "idle"));
    waitForRetainedCommands();
    publishRetainedCheckProblem(true);
  }

  if (pendingRoutineAdvance) {
    if (routine.active && mqttReady) {
      advanceRoutineAfterCheck();
    } else if (routine.active) {
      nextSleepSeconds = ROUTINE_POLL_SECONDS;
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
