const MQTT_DEFAULTS = Object.freeze({
  host: "36259d97745649d69b665673ad1883e7.s1.eu.hivemq.cloud",
  port: 8884,
  username: "jardinero_cc2",
});

const PROGRAM_TOPIC = "riego/programacion/cmd";
const BATTERY_TOPIC = "riego/device/battery";
const PROGRAM_STORAGE_KEY = "riego-programacion";
const WEEKDAYS = Object.freeze(["domingo", "lunes", "martes", "miércoles", "jueves", "viernes", "sábado"]);
const WEEKDAY_OPTIONS = Object.freeze([
  { value: 1, short: "L", name: "Lunes" },
  { value: 2, short: "M", name: "Martes" },
  { value: 3, short: "X", name: "Miércoles" },
  { value: 4, short: "J", name: "Jueves" },
  { value: 5, short: "V", name: "Viernes" },
  { value: 6, short: "S", name: "Sábado" },
  { value: 0, short: "D", name: "Domingo" },
]);
const ZONES = Object.freeze(
  Array.from({ length: 4 }, (_, index) => {
    const id = index + 1;
    return {
      id,
      name: `Zona ${id}`,
      commandTopic: `riego/zona${id}/cmd`,
      stateTopic: `riego/zona${id}/state`,
    };
  }),
);

const elements = {
  connectionButton: document.querySelector("#open-settings"),
  connectionLabel: document.querySelector("#connection-label"),
  systemSummary: document.querySelector("#system-summary"),
  batteryMetric: document.querySelector("#battery-metric"),
  batteryPercent: document.querySelector("#battery-percent"),
  batteryBar: document.querySelector("#battery-bar"),
  batteryVoltage: document.querySelector("#battery-voltage"),
  zonesGrid: document.querySelector("#zones-grid"),
  programForm: document.querySelector("#program-form"),
  programSummary: document.querySelector("#program-summary"),
  routinesList: document.querySelector("#routines-list"),
  addRoutine: document.querySelector("#add-routine"),
  stopCycle: document.querySelector("#stop-cycle"),
  cycleStatus: document.querySelector("#cycle-status"),
  cycleDetail: document.querySelector("#cycle-detail"),
  dialog: document.querySelector("#settings-dialog"),
  closeSettings: document.querySelector("#close-settings"),
  settingsForm: document.querySelector("#settings-form"),
  host: document.querySelector("#mqtt-host"),
  port: document.querySelector("#mqtt-port"),
  username: document.querySelector("#mqtt-user"),
  password: document.querySelector("#mqtt-password"),
  togglePassword: document.querySelector("#toggle-password"),
  formError: document.querySelector("#form-error"),
  connectButton: document.querySelector("#connect-button"),
  toast: document.querySelector("#toast"),
};

let client = null;
let toastTimer = null;
let cycleTimer = null;
let activeZoneId = null;
let cycleRunning = false;

function createRoutine(index = 0) {
  return {
    id: `routine-${Date.now()}-${Math.random().toString(16).slice(2)}`,
    name: `Rutina ${index + 1}`,
    enabled: true,
    dayMode: "daily",
    days: [],
    startTime: index === 0 ? "07:00" : "19:00",
    durations: [10, 10, 10, 10],
  };
}

function normalizeRoutine(rawRoutine, index) {
  const fallback = createRoutine(index);
  return {
    id: /^routine-[a-zA-Z0-9-]+$/.test(rawRoutine?.id) ? rawRoutine.id : fallback.id,
    name: typeof rawRoutine?.name === "string" && rawRoutine.name.trim()
      ? rawRoutine.name.trim().slice(0, 40)
      : fallback.name,
    enabled: rawRoutine?.enabled !== false,
    dayMode: rawRoutine?.dayMode === "selected" ? "selected" : "daily",
    days: Array.isArray(rawRoutine?.days)
      ? [...new Set(rawRoutine.days.map(Number).filter((day) => day >= 0 && day <= 6))]
      : [],
    startTime: /^\d{2}:\d{2}$/.test(rawRoutine?.startTime) ? rawRoutine.startTime : fallback.startTime,
    durations: ZONES.map((_, zoneIndex) => {
      const value = Number(rawRoutine?.durations?.[zoneIndex]);
      return Number.isInteger(value) && value >= 0 && value <= 180 ? value : fallback.durations[zoneIndex];
    }),
  };
}

function getSavedRoutines() {
  try {
    const saved = JSON.parse(localStorage.getItem(PROGRAM_STORAGE_KEY));
    if (!saved) return [createRoutine(0)];

    const routines = Array.isArray(saved) ? saved : saved.routines;
    if (Array.isArray(routines) && routines.length > 0) {
      return routines.map(normalizeRoutine);
    }

    // Migra la programación única de la versión anterior.
    if (saved.startTime || saved.durations) {
      return [normalizeRoutine({ ...saved, name: "Rutina 1" }, 0)];
    }
  } catch {
    // Si el almacenamiento está dañado, se crea una programación inicial segura.
  }
  return [createRoutine(0)];
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function weekdayMarkup(routine) {
  return WEEKDAY_OPTIONS.map(
    (day) => `
      <label>
        <input type="checkbox" name="weekday-${routine.id}" value="${day.value}"
          ${routine.days.includes(day.value) ? "checked" : ""} />
        <span>${day.short}</span>
        <small>${day.name}</small>
      </label>`,
  ).join("");
}

function durationMarkup(routine) {
  return ZONES.map(
    (zone, index) => `
      <label class="duration-row">
        <span class="duration-row__order">${zone.id}</span>
        <span class="duration-row__name">
          <strong>${zone.name}</strong>
          <small>${index === 0 ? "Primera en regar" : `Después de la zona ${zone.id - 1}`}</small>
        </span>
        <span class="number-field">
          <input class="duration-input" name="duration-${routine.id}-${zone.id}" type="number"
            inputmode="numeric" min="0" max="180" step="1" value="${routine.durations[index]}"
            aria-label="Minutos de la ${zone.name}" required />
          <span>min</span>
        </span>
      </label>`,
  ).join("");
}

function routineMarkup(routine, index) {
  const selectedDays = routine.dayMode === "selected";
  return `
    <article class="routine-card" data-routine-id="${routine.id}">
      <header class="routine-card__header">
        <span class="routine-index">${index + 1}</span>
        <label class="routine-name">
          <span class="sr-only">Nombre de la rutina</span>
          <input class="routine-name-input" type="text" maxlength="40" value="${escapeHtml(routine.name)}"
            aria-label="Nombre de la rutina ${index + 1}" required />
        </label>
        <label class="routine-enabled">
          <input type="checkbox" class="routine-enabled-input" ${routine.enabled ? "checked" : ""} />
          <span>Activa</span>
        </label>
        <button class="icon-button delete-routine" type="button" aria-label="Eliminar ${escapeHtml(routine.name)}">
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M4 7h16M9 7V4h6v3m-8 0 1 13h8l1-13M10 11v5m4-5v5" /></svg>
        </button>
      </header>

      <div class="routine-card__body">
        <section class="routine-panel">
          <div class="program-card__heading">
            <span class="step-number">1</span>
            <div>
              <h3>Días de riego</h3>
              <p>Todos los días o solamente los seleccionados.</p>
            </div>
          </div>

          <div class="segmented-control" role="radiogroup" aria-label="Frecuencia de ${escapeHtml(routine.name)}">
            <label>
              <input type="radio" name="day-mode-${routine.id}" value="daily"
                ${selectedDays ? "" : "checked"} />
              <span>Todos los días</span>
            </label>
            <label>
              <input type="radio" name="day-mode-${routine.id}" value="selected"
                ${selectedDays ? "checked" : ""} />
              <span>Días seleccionados</span>
            </label>
          </div>

          <fieldset class="weekday-picker" ${selectedDays ? "" : "disabled"}>
            <legend class="sr-only">Días de ${escapeHtml(routine.name)}</legend>
            ${weekdayMarkup(routine)}
          </fieldset>
        </section>

        <section class="routine-panel routine-panel--time">
          <div class="program-card__heading">
            <span class="step-number">2</span>
            <div>
              <h3>Hora de inicio</h3>
              <p>Esta rutina comenzará a la hora indicada.</p>
            </div>
          </div>
          <label class="time-field">
            <span>Inicio del ciclo</span>
            <input class="start-time-input" type="time" value="${routine.startTime}" required />
          </label>
        </section>

        <section class="routine-panel routine-panel--durations">
          <div class="program-card__heading">
            <span class="step-number">3</span>
            <div>
              <h3>Duración por zona</h3>
              <p>Siempre se ejecutan en orden y sin solaparse.</p>
            </div>
          </div>
          <div class="duration-list">${durationMarkup(routine)}</div>
        </section>
      </div>

      <p class="program-error" role="alert"></p>
      <footer class="routine-card__footer">
        <span class="routine-total">
          <small>Duración total</small>
          <strong></strong>
        </span>
        <span class="routine-preview"></span>
        <button class="primary-button primary-button--compact run-routine" type="button" disabled>
          Regar ahora
        </button>
      </footer>
    </article>`;
}

function renderRoutines(routines) {
  elements.routinesList.innerHTML = routines.map(routineMarkup).join("");
  elements.routinesList.querySelectorAll(".routine-card").forEach(updateRoutinePreview);
  updateProgramSummary();
  updateRunButtons();
}

function renderZones() {
  elements.zonesGrid.innerHTML = ZONES.map(
    (zone) => `
      <article class="sequence-zone" id="zone-card-${zone.id}" data-zone="${zone.id}" data-state="unknown">
        <span class="sequence-zone__number">${zone.id}</span>
        <span class="sequence-zone__line" aria-hidden="true"></span>
        <div class="sequence-zone__content">
          <span>
            <small>Paso ${zone.id} de 4</small>
            <strong>${zone.name}</strong>
          </span>
          <span class="sequence-zone__duration" id="zone-duration-${zone.id}">—</span>
        </div>
        <span class="state-badge" id="zone-badge-${zone.id}">
          <span aria-hidden="true"></span>
          Sin datos
        </span>
      </article>`,
  ).join("");
}

function readRoutine(card) {
  return {
    id: card.dataset.routineId,
    name: card.querySelector(".routine-name-input").value.trim(),
    enabled: card.querySelector(".routine-enabled-input").checked,
    dayMode: card.querySelector('input[type="radio"]:checked').value,
    days: [...card.querySelectorAll('.weekday-picker input[type="checkbox"]:checked')].map((input) =>
      Number(input.value),
    ),
    startTime: card.querySelector(".start-time-input").value,
    durations: [...card.querySelectorAll(".duration-input")].map((input) => Number(input.value)),
  };
}

function readAllRoutines() {
  return [...elements.routinesList.querySelectorAll(".routine-card")].map(readRoutine);
}

function describeDays(routine) {
  if (routine.dayMode === "daily") return "Todos los días";
  if (routine.days.length === 0) return "Sin días";
  return routine.days.map((day) => WEEKDAYS[day].slice(0, 3)).join(", ");
}

function updateRoutinePreview(card) {
  const routine = readRoutine(card);
  const total = routine.durations.reduce((sum, duration) => sum + (Number.isFinite(duration) ? duration : 0), 0);
  card.classList.toggle("is-disabled", !routine.enabled);
  card.querySelector(".routine-total strong").textContent = `${total} ${total === 1 ? "minuto" : "minutos"}`;
  card.querySelector(".routine-preview").textContent =
    `${describeDays(routine)} · ${routine.startTime || "--:--"} · ` +
    routine.durations.map((duration, index) => `Z${index + 1} ${duration || 0} min`).join(" → ");
}

function updateProgramSummary() {
  const routines = readAllRoutines();
  const activeCount = routines.filter((routine) => routine.enabled).length;
  elements.programSummary.textContent =
    `${routines.length} ${routines.length === 1 ? "rutina" : "rutinas"} · ` +
    `${activeCount} ${activeCount === 1 ? "activa" : "activas"}`;
}

function validateRoutine(card) {
  const routine = readRoutine(card);
  const error = card.querySelector(".program-error");
  error.textContent = "";

  if (!routine.name) {
    error.textContent = "Escribe un nombre para la rutina.";
    return null;
  }
  if (routine.dayMode === "selected" && routine.days.length === 0) {
    error.textContent = "Selecciona al menos un día para esta rutina.";
    return null;
  }
  if (!routine.startTime) {
    error.textContent = "Selecciona una hora de inicio.";
    return null;
  }
  if (
    !routine.durations.every((duration) => Number.isInteger(duration) && duration >= 0 && duration <= 180)
  ) {
    error.textContent = "Las duraciones deben estar entre 0 y 180 minutos.";
    return null;
  }
  if (routine.durations.every((duration) => duration === 0)) {
    error.textContent = "Configura al menos una zona con más de 0 minutos.";
    return null;
  }
  return routine;
}

function validateAllRoutines() {
  const routines = [];
  for (const card of elements.routinesList.querySelectorAll(".routine-card")) {
    const routine = validateRoutine(card);
    if (!routine) {
      card.scrollIntoView({ behavior: "smooth", block: "center" });
      return null;
    }
    routines.push(routine);
  }
  return routines;
}

function addRoutine() {
  const routine = createRoutine(elements.routinesList.children.length);
  elements.routinesList.insertAdjacentHTML("beforeend", routineMarkup(routine, elements.routinesList.children.length));
  const card = elements.routinesList.lastElementChild;
  updateRoutinePreview(card);
  updateProgramSummary();
  updateRunButtons();
  card.scrollIntoView({ behavior: "smooth", block: "center" });
  card.querySelector(".routine-name-input").select();
}

function removeRoutine(card) {
  if (elements.routinesList.children.length === 1) {
    showToast("Debe existir al menos una rutina.");
    return;
  }
  card.remove();
  elements.routinesList.querySelectorAll(".routine-card").forEach((routineCard, index) => {
    routineCard.querySelector(".routine-index").textContent = index + 1;
  });
  updateProgramSummary();
}

function getSessionConfig() {
  try {
    const config = JSON.parse(sessionStorage.getItem("riego-mqtt-config")) ?? {};
    if (config.username === "jardinero-cc2") config.username = MQTT_DEFAULTS.username;
    return config;
  } catch {
    return {};
  }
}

function populateSettings() {
  const saved = getSessionConfig();
  elements.host.value = saved.host || MQTT_DEFAULTS.host;
  elements.port.value = saved.port || MQTT_DEFAULTS.port;
  elements.username.value = saved.username || MQTT_DEFAULTS.username;
  elements.password.value = saved.password || "";
}

function saveSessionConfig(config) {
  sessionStorage.setItem("riego-mqtt-config", JSON.stringify(config));
}

function updateRunButtons() {
  document.querySelectorAll(".run-routine").forEach((button) => {
    button.disabled = !client?.connected || cycleRunning;
  });
}

function setConnectionStatus(status, label) {
  elements.connectionButton.dataset.status = status;
  elements.connectionLabel.textContent = label;
  const summaries = {
    connected: cycleRunning ? "Ciclo de riego en marcha" : "Programador conectado",
    connecting: "Conectando…",
    error: "Conexión interrumpida",
    idle: "Esperando conexión",
  };
  elements.systemSummary.textContent = summaries[status] || summaries.idle;
  updateRunButtons();
}

function setZoneState(zoneId, rawState) {
  const state = rawState.trim().toUpperCase();
  if (state !== "ON" && state !== "OFF") return;

  const normalizedState = state.toLowerCase();
  const card = document.querySelector(`#zone-card-${zoneId}`);
  const badge = document.querySelector(`#zone-badge-${zoneId}`);
  card.dataset.state = normalizedState;
  badge.innerHTML =
    normalizedState === "on"
      ? '<span aria-hidden="true"></span>Regando'
      : '<span aria-hidden="true"></span>En espera';
}

function parseBatteryPayload(rawPayload) {
  const payload = rawPayload.trim();
  if (!payload) return null;

  try {
    const data = JSON.parse(payload);
    const percent = Number(data.percent);
    const voltage = Number(data.voltage);
    if (Number.isFinite(percent)) {
      return {
        percent: Math.min(100, Math.max(0, Math.round(percent))),
        voltage: Number.isFinite(voltage) ? voltage : null,
      };
    }
  } catch {
    // Se permite tambien publicar solo el porcentaje durante pruebas manuales.
  }

  const percent = Number(payload);
  if (!Number.isFinite(percent)) return null;
  return {
    percent: Math.min(100, Math.max(0, Math.round(percent))),
    voltage: null,
  };
}

function setBatteryState(rawPayload) {
  const battery = parseBatteryPayload(rawPayload);
  if (!battery) return;

  elements.batteryPercent.textContent = `${battery.percent}%`;
  elements.batteryBar.style.width = `${battery.percent}%`;
  elements.batteryVoltage.textContent = battery.voltage === null
    ? "Voltaje no disponible"
    : `${battery.voltage.toFixed(2)} V`;

  const level = battery.percent <= 20 ? "low" : battery.percent <= 45 ? "medium" : "high";
  elements.batteryMetric.dataset.level = level;
}

function showToast(message) {
  clearTimeout(toastTimer);
  elements.toast.textContent = message;
  elements.toast.classList.add("is-visible");
  toastTimer = setTimeout(() => elements.toast.classList.remove("is-visible"), 2800);
}

function disconnectCurrentClient() {
  if (!client) return;
  client.removeAllListeners();
  client.end(true);
  client = null;
}

function connectMqtt(config) {
  if (!window.mqtt) {
    elements.formError.textContent = "No se pudo cargar MQTT.js. Comprueba tu conexión a internet.";
    setConnectionStatus("error", "Librería no disponible");
    return;
  }

  disconnectCurrentClient();
  setConnectionStatus("connecting", "Conectando…");
  elements.formError.textContent = "";
  elements.connectButton.disabled = true;

  client = mqtt.connect(`wss://${config.host}:${config.port}/mqtt`, {
    username: config.username,
    password: config.password,
    clientId: `riego-web-${crypto.randomUUID?.() || Math.random().toString(16).slice(2)}`,
    clean: true,
    connectTimeout: 10_000,
    reconnectPeriod: 4_000,
    keepalive: 45,
    protocolVersion: 4,
  });

  client.on("connect", () => {
    setConnectionStatus("connected", "En línea");
    elements.formError.textContent = "";
    elements.connectButton.disabled = false;
    elements.dialog.close();
    client.subscribe(
      [...ZONES.map((zone) => zone.stateTopic), BATTERY_TOPIC],
      { qos: 1 },
      (error) => showToast(error ? "No se pudo consultar el estado del programador." : "Programador conectado"),
    );
  });

  client.on("message", (topic, payload) => {
    const zone = ZONES.find((item) => item.stateTopic === topic);
    if (zone) setZoneState(zone.id, payload.toString());
    if (topic === BATTERY_TOPIC) setBatteryState(payload.toString());
  });
  client.on("reconnect", () => setConnectionStatus("connecting", "Reconectando…"));
  client.on("offline", () => setConnectionStatus("error", "Sin conexión"));
  client.on("error", (error) => {
    console.error("Error MQTT:", error);
    elements.connectButton.disabled = false;
    elements.formError.textContent = "No se pudo conectar. Revisa servidor, usuario y contraseña.";
    setConnectionStatus("error", "Error de conexión");
  });
}

function publish(topic, payload, options = {}) {
  return new Promise((resolve, reject) => {
    if (!client?.connected) {
      reject(new Error("Panel desconectado"));
      return;
    }
    client.publish(topic, payload, { qos: 1, retain: false, ...options }, (error) => {
      if (error) reject(error);
      else resolve();
    });
  });
}

async function closeAllZones(exceptZoneId = null) {
  await Promise.all(
    ZONES.filter((zone) => zone.id !== exceptZoneId).map((zone) =>
      publish(zone.commandTopic, "OFF", { retain: true }),
    ),
  );
}

function showRoutineInSequence(routine) {
  routine.durations.forEach((duration, index) => {
    document.querySelector(`#zone-duration-${index + 1}`).textContent = duration > 0 ? `${duration} min` : "Omitida";
  });
}

function finishCycle(message = "Ciclo completado") {
  clearTimeout(cycleTimer);
  cycleTimer = null;
  cycleRunning = false;
  activeZoneId = null;
  elements.cycleStatus.textContent = message;
  elements.cycleDetail.textContent = "Todas las zonas están cerradas.";
  elements.stopCycle.disabled = true;
  elements.systemSummary.textContent = "Programador conectado";
  updateRunButtons();
}

async function stopCycle(message = "Ciclo detenido") {
  clearTimeout(cycleTimer);
  cycleTimer = null;
  cycleRunning = false;
  try {
    await closeAllZones();
    finishCycle(message);
  } catch {
    finishCycle("Detención enviada");
    showToast("No se pudo confirmar el cierre de todas las zonas.");
  }
}

async function runZone(routine, index) {
  if (!cycleRunning) return;
  if (index >= ZONES.length) {
    await stopCycle(`${routine.name} completada`);
    return;
  }

  const zone = ZONES[index];
  const duration = routine.durations[index];
  if (duration === 0) {
    runZone(routine, index + 1);
    return;
  }

  try {
    await closeAllZones(zone.id);
    await publish(zone.commandTopic, "ON", { retain: true });
    activeZoneId = zone.id;
    elements.cycleStatus.textContent = `${routine.name} · Regando ${zone.name}`;
    elements.cycleDetail.textContent = ZONES[index + 1]
      ? `${duration} min · después continuará la ${ZONES[index + 1].name}.`
      : `${duration} min · última zona del ciclo.`;
    cycleTimer = setTimeout(async () => {
      try {
        await publish(zone.commandTopic, "OFF", { retain: true });
        runZone(routine, index + 1);
      } catch {
        stopCycle("Ciclo interrumpido");
      }
    }, duration * 60_000);
  } catch {
    stopCycle("Ciclo interrumpido");
    showToast(`No se pudo iniciar la ${zone.name}.`);
  }
}

async function startRoutine(card) {
  const routine = validateRoutine(card);
  if (!routine) return;
  if (!client?.connected) {
    showToast("Conecta el programador antes de iniciar el riego.");
    return;
  }

  cycleRunning = true;
  showRoutineInSequence(routine);
  updateRunButtons();
  elements.stopCycle.disabled = false;
  elements.systemSummary.textContent = "Ciclo de riego en marcha";
  await runZone(routine, 0);
}

renderZones();
renderRoutines(getSavedRoutines());
populateSettings();
setConnectionStatus("idle", "Configurar");

elements.addRoutine.addEventListener("click", addRoutine);
elements.routinesList.addEventListener("input", (event) => {
  const card = event.target.closest(".routine-card");
  if (!card) return;
  updateRoutinePreview(card);
  updateProgramSummary();
});
elements.routinesList.addEventListener("change", (event) => {
  const card = event.target.closest(".routine-card");
  if (!card) return;
  if (event.target.matches('input[type="radio"]')) {
    card.querySelector(".weekday-picker").disabled = event.target.value !== "selected";
    card.querySelector(".program-error").textContent = "";
  }
  updateRoutinePreview(card);
  updateProgramSummary();
});
elements.routinesList.addEventListener("click", (event) => {
  const card = event.target.closest(".routine-card");
  if (!card) return;
  if (event.target.closest(".delete-routine")) removeRoutine(card);
  if (event.target.closest(".run-routine")) startRoutine(card);
});
elements.programForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  const routines = validateAllRoutines();
  if (!routines) return;

  const program = { version: 2, routines };
  localStorage.setItem(PROGRAM_STORAGE_KEY, JSON.stringify(program));
  updateProgramSummary();

  if (client?.connected) {
    try {
      await publish(PROGRAM_TOPIC, JSON.stringify(program), { retain: true });
      showToast("Todas las rutinas se han guardado y enviado.");
    } catch {
      showToast("Guardadas en este dispositivo; no se pudieron enviar.");
    }
  } else {
    showToast("Rutinas guardadas en este dispositivo.");
  }
});

elements.stopCycle.addEventListener("click", () => stopCycle());
elements.connectionButton.addEventListener("click", () => elements.dialog.showModal());
elements.closeSettings.addEventListener("click", () => elements.dialog.close());
elements.dialog.addEventListener("click", (event) => {
  if (event.target === elements.dialog) elements.dialog.close();
});
elements.togglePassword.addEventListener("click", () => {
  const showPassword = elements.password.type === "password";
  elements.password.type = showPassword ? "text" : "password";
  elements.togglePassword.textContent = showPassword ? "Ocultar" : "Mostrar";
  elements.togglePassword.setAttribute("aria-label", showPassword ? "Ocultar contraseña" : "Mostrar contraseña");
});
elements.settingsForm.addEventListener("submit", (event) => {
  event.preventDefault();
  const config = {
    host: elements.host.value.trim().replace(/^wss?:\/\//, "").replace(/\/.*$/, ""),
    port: Number(elements.port.value),
    username: elements.username.value.trim(),
    password: elements.password.value,
  };
  saveSessionConfig(config);
  connectMqtt(config);
});

window.addEventListener("beforeunload", () => {
  if (cycleRunning && activeZoneId) {
    client?.publish(ZONES[activeZoneId - 1].commandTopic, "OFF", { qos: 1, retain: true });
  }
  disconnectCurrentClient();
});

const savedConfig = getSessionConfig();
if (savedConfig.password) connectMqtt(savedConfig);
else setTimeout(() => elements.dialog.showModal(), 450);

if ("serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("./service-worker.js").catch((error) => {
      console.error("No se pudo registrar el service worker:", error);
    });
  });
}
