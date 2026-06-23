#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <cstring>
#include "secrets.h" // Importamos nuestras credenciales seguras

// Definición de pines (Usamos el LED amarillo en GPIO2 como prueba del relé)
const int pinRele = 2; 
const char* topicComando = "riego/zona1/cmd";
const char* topicEstado = "riego/zona1/state";

// Instancias de los clientes
WiFiClientSecure espClient;
PubSubClient client(espClient);

// La publicación se hace fuera del callback para no reutilizar el búfer MQTT
// mientras todavía se está procesando el mensaje recibido.
const char* estadoPendiente = nullptr;

// Declaración de funciones
void setup_wifi();
void callback(char* topic, byte* payload, unsigned int length);
void reconnect();

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();

  pinMode(pinRele, OUTPUT);
  digitalWrite(pinRele, LOW); // Aseguramos que la válvula empiece cerrada

  // 1. Conectar al WiFi
  setup_wifi();

  // 2. Configurar la conexión segura para HiveMQ
  espClient.setInsecure(); // Omite la validación de certificados complejos para facilitar el desarrollo inicial

  // 3. Configurar el Broker MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Conectando a red WiFi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  // Bucle de espera que ya dominas
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n¡WiFi conectado!");
  Serial.print("IP asignada: ");
  Serial.println(WiFi.localIP());
}

// La función mágica: se ejecuta automáticamente al recibir un mensaje
void callback(char* topic, byte* payload, unsigned int length) {
  // Solo aceptamos los comandos "ON" y "OFF".
  char mensaje[4] = {};
  if (length == 0 || length >= sizeof(mensaje)) {
    Serial.println("Mensaje MQTT ignorado: longitud no valida");
    return;
  }

  memcpy(mensaje, payload, length);
  mensaje[length] = '\0';

  Serial.print("Mensaje recibido en [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(mensaje);

  // Lógica de encendido/apagado para la Zona 1
  if (strcmp(topic, topicComando) == 0) {
    if (strcmp(mensaje, "ON") == 0) {
      digitalWrite(pinRele, HIGH);
      estadoPendiente = "ON";
      Serial.println("Accion: Valvula ABIERTA");
    } 
    else if (strcmp(mensaje, "OFF") == 0) {
      digitalWrite(pinRele, LOW);
      estadoPendiente = "OFF";
      Serial.println("Accion: Valvula CERRADA");
    } else {
      Serial.println("Comando desconocido");
    }
  }
}

// Rutina de seguridad: Reconexión automática
void reconnect() {
  while (!client.connected()) {
    Serial.print("Intentando conexión MQTT... ");
    
    // Intentamos conectar usando un ID único y nuestras credenciales
    if (client.connect("ESP32_Jardinero_01", mqtt_user, mqtt_password)) {
      Serial.println("¡Conectado al Broker!");
      
      // Una vez conectados, nos suscribimos al topic para escuchar órdenes
      client.subscribe(topicComando);
    } else {
      Serial.print("Falló, código de error = ");
      Serial.print(client.state());
      Serial.println(" -> Intentando de nuevo en 5 segundos");
      delay(5000);
    }
  }
}

void loop() {
  // Si se pierde la conexión con el Broker, la reconectamos
  if (!client.connected()) {
    reconnect();
  }
  
  // Mantiene vivo el hilo de escucha con el Broker
  client.loop();

  // Publicamos después de terminar de procesar el mensaje entrante.
  if (estadoPendiente != nullptr && client.connected()) {
    if (client.publish(topicEstado, estadoPendiente, true)) {
      estadoPendiente = nullptr;
    } else {
      Serial.println("No se pudo publicar el estado; se reintentara");
    }
  }
}
