# Firmware del sistema de riego

Firmware inicial para controlar una zona de riego con una placa ESP32 mediante
MQTT.

## Estado actual

- Conexión del ESP32 a una red Wi-Fi.
- Conexión MQTT segura mediante TLS con un clúster de HiveMQ Cloud.
- Autenticación mediante las credenciales guardadas en `include/secrets.h`.
- Suscripción al topic `riego/zona1/cmd`.
- Encendido y apagado del LED del GPIO 2, usado como simulación del relé:
  - `ON`: abre la válvula.
  - `OFF`: cierra la válvula.
- Publicación del resultado en `riego/zona1/state`.
- Reconexión automática al broker si se pierde la conexión.
- Validación de los mensajes recibidos y salida de diagnóstico por el monitor
  serie a 115200 baudios.

## Prueba

Compilar y cargar el proyecto con PlatformIO. Después, desde el Web Client de
HiveMQ, publicar `ON` o `OFF` en `riego/zona1/cmd` y observar el LED y el
monitor serie.

El archivo `include/secrets.h` contiene información privada y está excluido de
Git. Debe definir el SSID y la contraseña Wi-Fi, además del servidor, puerto,
usuario y contraseña MQTT.
