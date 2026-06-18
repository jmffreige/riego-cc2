#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "secrets.h" // Importamos nuestras credenciales seguras

// Definición de pines (Usamos el LED amarillo en GPIO2 como prueba del relé)
const int pinRele = 2; 

// Instancias de los clientes
WiFiClientSecure espClient;
PubSubClient client(espClient);

// Declaración de funciones
void setup_wifi();
void callback(char* topic, byte* payload, unsigned int length);
void reconnect();

void setup() {
  Serial.begin(115200);
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
  String messageTemp;
  
  // Convertimos el payload (bytes) en un String (texto)
  for (int i = 0; i < length; i++) {
    messageTemp += (char)payload[i];
  }

  Serial.print("Mensaje recibido en [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(messageTemp);

  // Lógica de encendido/apagado para la Zona 1
  if (String(topic) == "riego/zona1/cmd") {
    if (messageTemp == "ON") {
      digitalWrite(pinRele, HIGH);
      Serial.println("Acción: Válvula ABIERTA");
      client.publish("riego/zona1/state", "ON"); // Reportamos el estado al cartero
    } 
    else if (messageTemp == "OFF") {
      digitalWrite(pinRele, LOW);
      Serial.println("Acción: Válvula CERRADA");
      client.publish("riego/zona1/state", "OFF"); // Reportamos el estado al cartero
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
      client.subscribe("riego/zona1/cmd");
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
}