const MQTT_DEFAULTS = Object.freeze({
  host: "36259d97745649d69b665673ad1883e7.s1.eu.hivemq.cloud",
  port: 8884,
  username: "jardinero_cc2",
});

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

const zoneStates = new Map(ZONES.map((zone) => [zone.id, "unknown"]));

const elements = {
  connectionButton: document.querySelector("#open-settings"),
  connectionLabel: document.querySelector("#connection-label"),
  systemSummary: document.querySelector("#system-summary"),
  zonesGrid: document.querySelector("#zones-grid"),
  dialog: document.querySelector("#settings-dialog"),
  closeSettings: document.querySelector("#close-settings"),
  form: document.querySelector("#settings-form"),
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

function sprinklerMarkup() {
  return `
    <div class="device-visual" aria-hidden="true">
      <div class="water-rings"><span></span><span></span><span></span></div>
      <svg class="sprinkler" viewBox="0 0 180 130">
        <path class="sprinkler__water sprinkler__water--left" d="M78 41C52 30 25 37 12 57" />
        <path class="sprinkler__water sprinkler__water--right" d="M102 41c26-11 53-4 66 16" />
        <path class="sprinkler__water sprinkler__water--far-left" d="M72 32C43 13 15 21 4 40" />
        <path class="sprinkler__water sprinkler__water--far-right" d="M108 32c29-19 57-11 68 8" />
        <path class="sprinkler__body" d="M78 34h24v17H78zM84 51h12v42H84zM67 93h46v10H67z" />
      </svg>
    </div>`;
}

function renderZones() {
  elements.zonesGrid.innerHTML = ZONES.map(
    (zone) => `
      <article class="device-card" id="zone-card-${zone.id}" data-zone="${zone.id}" data-state="unknown">
        <div class="device-card__head">
          <div class="device-identity">
            <span class="device-identity__icon" aria-hidden="true">
              <svg viewBox="0 0 24 24">
                <path d="M12 3v5M8.5 8h7M7 12c-2.5 0-4 1.4-4 3.5M17 12c2.5 0 4 1.4 4 3.5M8 12c0 3-1.5 5.5-4.5 7M16 12c0 3 1.5 5.5 4.5 7M12 12v8" />
              </svg>
            </span>
            <span>
              <small>Jardín Frontal</small>
              <h3>${zone.name}</h3>
            </span>
          </div>

          <span class="state-badge" id="zone-badge-${zone.id}">
            <span aria-hidden="true"></span>
            Desconocido
          </span>
        </div>

        ${sprinklerMarkup()}

        <div class="device-status">
          <p id="zone-status-${zone.id}">Conecta el panel para consultar el estado.</p>
          <time id="last-update-${zone.id}" datetime="">Sin datos todavía</time>
        </div>

        <button class="action-button zone-action" type="button" data-zone="${zone.id}" disabled>
          <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 21a8 8 0 0 0 0-16M12 2v10l4 2" /></svg>
          <span>Encender</span>
        </button>
      </article>`,
  ).join("");
}

function getSessionConfig() {
  try {
    const config = JSON.parse(sessionStorage.getItem("riego-mqtt-config")) ?? {};

    if (config.username === "jardinero-cc2") {
      config.username = MQTT_DEFAULTS.username;
      saveSessionConfig(config);
    }

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

function setConnectionStatus(status, label) {
  elements.connectionButton.dataset.status = status;
  elements.connectionLabel.textContent = label;

  const summaries = {
    connected: "Sistema conectado",
    connecting: "Conectando…",
    error: "Conexión interrumpida",
    idle: "Esperando conexión",
  };

  elements.systemSummary.textContent = summaries[status] || summaries.idle;
  document.querySelectorAll(".zone-action").forEach((button) => {
    button.disabled = status !== "connected";
  });
}

function updateActionButton(zoneId, state) {
  const button = document.querySelector(`.zone-action[data-zone="${zoneId}"]`);
  const label = button.querySelector("span");
  const path = button.querySelector("path");
  const isOn = state === "on";

  button.dataset.command = isOn ? "OFF" : "ON";
  button.classList.toggle("action-button--off", isOn);
  button.classList.toggle("action-button--on", !isOn);
  label.textContent = isOn ? "Apagar" : "Encender";
  path.setAttribute("d", isOn ? "M18.4 6.3a8 8 0 1 1-12.8 0M12 3v9" : "M12 21a8 8 0 0 0 0-16M12 2v10l4 2");
}

function setZoneState(zoneId, rawState) {
  const state = rawState.trim().toUpperCase();
  const card = document.querySelector(`#zone-card-${zoneId}`);
  const badge = document.querySelector(`#zone-badge-${zoneId}`);
  const status = document.querySelector(`#zone-status-${zoneId}`);
  const lastUpdate = document.querySelector(`#last-update-${zoneId}`);
  const timestamp = new Date();

  if (state === "ON") {
    zoneStates.set(zoneId, "on");
    card.dataset.state = "on";
    badge.innerHTML = '<span aria-hidden="true"></span>Regando';
    status.textContent = "La válvula está abierta y la zona se está regando.";
    updateActionButton(zoneId, "on");
  } else if (state === "OFF") {
    zoneStates.set(zoneId, "off");
    card.dataset.state = "off";
    badge.innerHTML = '<span aria-hidden="true"></span>Apagado';
    status.textContent = "La válvula está cerrada. La zona está en reposo.";
    updateActionButton(zoneId, "off");
  } else {
    return;
  }

  lastUpdate.dateTime = timestamp.toISOString();
  lastUpdate.textContent = `Actualizado a las ${timestamp.toLocaleTimeString("es-ES", {
    hour: "2-digit",
    minute: "2-digit",
  })}`;
}

function showToast(message) {
  clearTimeout(toastTimer);
  elements.toast.textContent = message;
  elements.toast.classList.add("is-visible");
  toastTimer = setTimeout(() => elements.toast.classList.remove("is-visible"), 2600);
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
      ZONES.map((zone) => zone.stateTopic),
      { qos: 1 },
      (error) => {
        showToast(error ? "No se pudo escuchar el estado de las zonas." : "Panel conectado a HiveMQ");
      },
    );
  });

  client.on("message", (topic, payload) => {
    const zone = ZONES.find((item) => item.stateTopic === topic);
    if (zone) setZoneState(zone.id, payload.toString());
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

function publishCommand(zoneId, command) {
  const zone = ZONES.find((item) => item.id === zoneId);

  if (!client?.connected || !zone) {
    showToast("El panel no está conectado.");
    return;
  }

  client.publish(zone.commandTopic, command, { qos: 1, retain: false }, (error) => {
    if (error) {
      showToast(`No se pudo enviar el comando a ${zone.name}.`);
      return;
    }

    showToast(`${zone.name}: orden de ${command === "ON" ? "encendido" : "apagado"} enviada`);
  });
}

renderZones();
populateSettings();
setConnectionStatus("idle", "Configurar");

elements.zonesGrid.addEventListener("click", (event) => {
  const button = event.target.closest(".zone-action");
  if (!button) return;

  const zoneId = Number(button.dataset.zone);
  const currentState = zoneStates.get(zoneId);
  const command = currentState === "on" ? "OFF" : "ON";
  publishCommand(zoneId, command);
});

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

elements.form.addEventListener("submit", (event) => {
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

window.addEventListener("beforeunload", disconnectCurrentClient);

const savedConfig = getSessionConfig();
if (savedConfig.password) {
  connectMqtt(savedConfig);
} else {
  setTimeout(() => elements.dialog.showModal(), 450);
}

if ("serviceWorker" in navigator) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("./service-worker.js").catch((error) => {
      console.error("No se pudo registrar el service worker:", error);
    });
  });
}
