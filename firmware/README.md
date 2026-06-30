# Firmware - Control Remoto Riegos CC2

Firmware Arduino/C++ para un programador de riego IoT de 4 zonas basado en ESP32,
alimentado por batería Li-ion y panel solar. El equipo despierta periódicamente,
lee comandos MQTT retenidos desde HiveMQ, acciona electroválvulas latching Rain
Bird mediante puentes H DRV8833, publica telemetría de batería y vuelve a deep
sleep para reducir el consumo.

Además puede ejecutar rutinas de riego completas en el propio ESP32: abre una
zona, duerme exactamente los minutos configurados, despierta, cierra esa zona,
abre la siguiente y repite hasta terminar.

## Arquitectura

- Microcontrolador: AZDelivery Lolin32 Lite V1.0, compatible ESP32.
- Alimentación: panel solar 5 W y 2x Samsung INR18650-30Q en paralelo.
- Válvulas: 4x Rain Bird latching 9 V DC.
- Potencia: MT3608 ajustado a 9 V con condensador de apoyo de 4700 uF / 16 V.
- Drivers: 2x DRV8833 usados como puentes H para invertir polaridad.
- Comunicaciones: Wi-Fi + MQTT TLS contra HiveMQ Cloud.
- Modo energía: ciclo wake -> Wi-Fi -> MQTT -> acciones -> batería -> deep sleep.

## Pinout

| Zona | Apertura | Cierre | Driver |
| --- | --- | --- | --- |
| 1 | GPIO 4 | GPIO 16 | DRV8833 1 |
| 2 | GPIO 17 | GPIO 18 | DRV8833 1 |
| 3 | GPIO 19 | GPIO 21 | DRV8833 2 |
| 4 | GPIO 22 | GPIO 23 | DRV8833 2 |

La batería se lee en `GPIO35`, usando el divisor interno del Lolin32 Lite.

## Lógica de válvulas latching

Cada válvula se acciona con un pulso de 50 ms:

- Abrir: pin de apertura `HIGH`, pin de cierre `LOW`.
- Cerrar: pin de apertura `LOW`, pin de cierre `HIGH`.
- Reposo: todos los pines quedan en `LOW`.

El firmware guarda en memoria RTC el último estado aplicado durante deep sleep.
Así, si la PWA mantiene un comando retenido sin cambios, el ESP32 publica el
estado pero no repite el pulso en cada despertar.

Cuando hay una rutina activa, los comandos manuales de zona se ignoran para
evitar conflictos con la secuencia programada.

## Rutinas de riego

La PWA debe publicar la rutina activa en `riego/routine/config` con
`retain: true`. El campo `id` funciona como revision de la rutina: si la rutina
termina, el ESP32 no la vuelve a ejecutar mientras el mensaje retenido conserve
el mismo `id`. Para lanzarla de nuevo, publicar la misma rutina con un `id`
nuevo.

Ejemplo: zona 1 durante 5 minutos y zona 2 durante 12 minutos:

```json
{
  "enabled": true,
  "id": 101,
  "steps": [
    { "zone": 1, "minutes": 5 },
    { "zone": 2, "minutes": 12 }
  ]
}
```

Formato:

- `enabled`: `true` para iniciar o mantener una rutina; `false` para cancelar.
- `id`: entero mayor que `0`. Debe cambiar cada vez que se quiera ejecutar una
  rutina nueva o repetir una ya terminada.
- `steps`: lista de 1 a 8 pasos.
- `zone`: zona entre `1` y `4`.
- `minutes`: duracion del paso, entre `1` y `1440` minutos.

Tambien se acepta `durationMinutes` en lugar de `minutes`.

Para cancelar una rutina en curso, publicar:

```json
{ "enabled": false }
```

Si se cancela mientras una zona esta abierta, el ESP32 la cierra en cuanto
recibe el mensaje.

## Topics MQTT

La PWA debe publicar comandos con `retain: true`.

| Función | Topic | Payload aceptado |
| --- | --- | --- |
| Rutina activa | `riego/routine/config` | JSON retenido |
| Estado de rutina | `riego/routine/state` | JSON retenido |
| Comando zona 1 | `riego/zona1/cmd` | `ON`, `OFF`, `open`, `close`, `abrir`, `cerrar`, `1`, `0`, `true`, `false` |
| Comando zona 2 | `riego/zona2/cmd` | Igual |
| Comando zona 3 | `riego/zona3/cmd` | Igual |
| Comando zona 4 | `riego/zona4/cmd` | Igual |
| Estado zona 1 | `riego/zona1/state` | `ON` / `OFF`, retenido |
| Estado zona 2 | `riego/zona2/state` | `ON` / `OFF`, retenido |
| Estado zona 3 | `riego/zona3/state` | `ON` / `OFF`, retenido |
| Estado zona 4 | `riego/zona4/state` | `ON` / `OFF`, retenido |
| Batería | `riego/device/battery` | JSON retenido |
| Estado dispositivo | `riego/device/status` | `online`, `sleeping`, `offline` |

Ejemplo de telemetría de batería:

```json
{"voltage":3.91,"percent":71}
```

Ejemplo de estado de rutina:

```json
{"status":"watering","id":101,"step":1,"stepCount":2,"openZone":1,"nextWakeMinutes":5}
```

Estados posibles:

- `idle`: no hay rutina activa.
- `watering`: hay una zona abierta y el ESP32 dormira hasta el siguiente paso.
- `finished`: la rutina termino correctamente.
- `disabled`: la rutina fue cancelada desde MQTT.

## Ciclo de deep sleep

El intervalo por defecto, cuando no hay rutina activa, esta definido en
`src/main.cpp`:

```cpp
constexpr uint64_t DEFAULT_SLEEP_MINUTES = 10;
```

En cada ciclo:

1. Configura todos los pines de válvulas como salida y los deja en `LOW`.
2. Si habia una rutina activa antes de dormir, ejecuta inmediatamente el cambio
   programado: cierra la zona actual y abre la siguiente, o finaliza la rutina.
3. Conecta a Wi-Fi.
4. Conecta a HiveMQ mediante TLS.
5. Publica batería y estado de rutina.
6. Se suscribe al topic de rutina y a los 4 topics de comando.
7. Escucha durante 7 segundos para recibir mensajes retenidos.
8. Si recibe una rutina nueva, la copia a memoria RTC y abre el primer paso.
9. Publica `sleeping`, desconecta y entra en deep sleep.

Con una rutina activa, el deep sleep ya no usa siempre 10 minutos: usa la
duracion del paso actual. Por ejemplo, una rutina de 5 minutos en zona 1 y
12 minutos en zona 2 se ejecuta asi:

1. Recibe la rutina, abre zona 1 y duerme 5 minutos.
2. Despierta, cierra zona 1, abre zona 2 y duerme 12 minutos.
3. Despierta, cierra zona 2, marca la rutina como `finished` y vuelve al ciclo
   normal de 10 minutos.

Si falla Wi-Fi o MQTT, el equipo vuelve igualmente a dormir para proteger la
batería y reintenta en el siguiente ciclo. Si ya habia una rutina guardada en
memoria RTC, el cambio de zona se ejecuta antes de intentar conectar, por lo que
un fallo de red no deja la zona abierta esperando a MQTT.

## Configuración privada

Las credenciales viven en `include/secrets.h`, excluido de Git. Debe definir:

```cpp
const char* ssid = "TU_WIFI";
const char* password = "TU_PASSWORD";

const char* mqtt_server = "cluster.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_user = "usuario";
const char* mqtt_password = "password";
```

## Compilación

Proyecto PlatformIO:

```bash
pio run
```

Subida a la placa:

```bash
pio run --target upload
```

Monitor serie:

```bash
pio device monitor --baud 115200
```

## Notas de calibración

- La lectura de batería asume divisor 1:2 y ADC de 12 bits con referencia de
  3.3 V. Si la lectura real difiere, ajustar `BATTERY_DIVIDER_FACTOR`.
- El porcentaje usa una escala lineal entre 3.20 V y 4.20 V por celda. Es
  suficiente para telemetría operativa, aunque no equivale a una curva SoC
  precisa para Li-ion.
- Para producción conviene sustituir `secureClient.setInsecure()` por validación
  del certificado raíz de HiveMQ.
