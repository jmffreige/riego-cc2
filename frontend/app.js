const MQTT_DEFAULTS = Object.freeze({
  host: "36259d97745649d69b665673ad1883e7.s1.eu.hivemq.cloud",
  port: 8884,
  username: "jardinero_cc2",
  commandTopic: "riego/zona1/cmd",
  stateTopic: "riego/zona1/state",
});

const elements = {
  connectionButton: document.querySelector("#open-settings"),
  connectionLabel: document.querySelector("#connection-label"),
  systemSummary: document.querySelector("#system-summary"),
  zoneCard: document.querySelector("#zone-card"),
  zoneBadge: document.querySelector("#zone-badge"),
  zoneStatus: document.querySelector("#zone-status"),
  lastUpdate: document.querySelector("#last-update"),
  turnOn: document.querySelector("#turn-on"),
  turnOff: document.querySelector("#turn-off"),
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
  const enabled = status === "connected";
  elements.turnOn.disabled = !enabled;
  elements.turnOff.disabled = !enabled;
}

function setZoneState(rawState) {
  const state = rawState.trim().toUpperCase();
  const timestamp = new Date();

  if (state === "ON") {
    elements.zoneCard.dataset.state = "on";
    elements.zoneBadge.innerHTML = '<span aria-hidden="true"></span>Regando';
    elements.zoneStatus.textContent = "La válvula está abierta y la zona se está regando.";
  } else if (state === "OFF") {
    elements.zoneCard.dataset.state = "off";
    elements.zoneBadge.innerHTML = '<span aria-hidden="true"></span>Apagado';
    elements.zoneStatus.textContent = "La válvula está cerrada. El jardín está en reposo.";
  } else {
    return;
  }

  elements.lastUpdate.dateTime = timestamp.toISOString();
  elements.lastUpdate.textContent = `Actualizado a las ${timestamp.toLocaleTimeString("es-ES", {
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

  const brokerUrl = `wss://${config.host}:${config.port}/mqtt`;
  const clientId = `riego-web-${crypto.randomUUID?.() || Math.random().toString(16).slice(2)}`;

  client = mqtt.connect(brokerUrl, {
    username: config.username,
    password: config.password,
    clientId,
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

    client.subscribe(MQTT_DEFAULTS.stateTopic, { qos: 1 }, (error) => {
      if (error) {
        showToast("No se pudo escuchar el estado de la zona.");
        return;
      }
      showToast("Panel conectado a HiveMQ");
    });
  });

  client.on("message", (topic, payload) => {
    if (topic === MQTT_DEFAULTS.stateTopic) {
      setZoneState(payload.toString());
    }
  });

  client.on("reconnect", () => {
    setConnectionStatus("connecting", "Reconectando…");
  });

  client.on("offline", () => {
    setConnectionStatus("error", "Sin conexión");
  });

  client.on("error", (error) => {
    console.error("Error MQTT:", error);
    elements.connectButton.disabled = false;
    elements.formError.textContent = "No se pudo conectar. Revisa servidor, usuario y contraseña.";
    setConnectionStatus("error", "Error de conexión");
  });
}

function publishCommand(command) {
  if (!client?.connected) {
    showToast("El panel no está conectado.");
    return;
  }

  client.publish(MQTT_DEFAULTS.commandTopic, command, { qos: 1, retain: false }, (error) => {
    if (error) {
      showToast("No se pudo enviar el comando.");
      return;
    }

    showToast(command === "ON" ? "Orden de encendido enviada" : "Orden de apagado enviada");
  });
}

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

elements.turnOn.addEventListener("click", () => publishCommand("ON"));
elements.turnOff.addEventListener("click", () => publishCommand("OFF"));

window.addEventListener("beforeunload", disconnectCurrentClient);

populateSettings();
setConnectionStatus("idle", "Configurar");

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
