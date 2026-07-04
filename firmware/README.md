# Firmware - Control Remoto Riegos CC2

Firmware Arduino/C++ para un programador de riego IoT de 4 zonas basado en ESP32,
alimentado por batería Li-ion y panel solar. El equipo despierta periódicamente,
lee comandos MQTT retenidos desde HiveMQ, acciona electroválvulas latching Rain
Bird mediante puentes H DRV8833, publica telemetría de batería y vuelve a deep
sleep para reducir el consumo.

Además puede ejecutar rutinas de riego completas en el propio ESP32. La PWA
publica una programación retenida con días, hora de inicio y duración por zona;
el controlador despierta cada 5 minutos por defecto, pero acorta ese deep sleep
cuando detecta una rutina que debe empezar antes del siguiente ciclo.

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
| 3 | GPIO 19 | GPIO 23 | DRV8833 2 |
| 4 | GPIO 26 | GPIO 25 | DRV8833 2 |

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

## Programación de riego

La PWA publica la programación completa en `riego/programacion/cmd` con
`retain: true`. El firmware conserva hasta 8 rutinas activas en memoria RTC y
las vuelve a evaluar en cada despertar.

Ejemplo: una rutina diaria a las 07:00:

```json
{
  "version": 2,
  "routines": [
    {
      "id": "routine-manana",
      "name": "Mañana",
      "enabled": true,
      "dayMode": "daily",
      "days": [],
      "startTime": "07:00",
      "durations": [10, 8, 12, 5]
    }
  ]
}
```

Formato:

- `version`: versión del formato publicado por la PWA.
- `routines`: lista de rutinas; el firmware usa como máximo las 8 primeras
  rutinas activas y válidas.
- `id`: identificador de la rutina.
- `name`: nombre descriptivo, usado también para detectar cambios de revisión.
- `enabled`: `true` para programarla; `false` para ignorarla o detenerla si era
  la rutina en curso.
- `dayMode`: `daily` para todos los días o `selected` para usar `days`.
- `days`: días de la semana cuando `dayMode` es `selected`, con `0` domingo,
  `1` lunes ... `6` sábado.
- `startTime`: hora local de inicio en formato `HH:MM`.
- `durations`: minutos por zona, en orden zona 1 a zona 4. Un `0` omite esa
  zona. Cada duración debe estar entre `0` y `180`, y al menos una zona debe
  tener más de `0` minutos.

La hora local se sincroniza por NTP y usa zona horaria de España peninsular:
`CET-1CEST,M3.5.0/2,M10.5.0/3`.

Para cancelar o modificar una rutina programada, publicar de nuevo
`riego/programacion/cmd` con esa rutina desactivada, eliminada o cambiada. Si
hay una rutina en curso y el firmware detecta que ya no coincide con la
programación retenida, cierra la zona abierta y no continúa con el siguiente
paso.

El topic antiguo `riego/routine/config` sigue existiendo para pruebas manuales
de una rutina inmediata:

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

Para cancelar esa rutina inmediata, publicar en `riego/routine/config`:

```json
{ "enabled": false }
```

## Topics MQTT

La PWA debe publicar comandos con `retain: true`.

| Función | Topic | Payload aceptado |
| --- | --- | --- |
| Programación | `riego/programacion/cmd` | JSON retenido |
| Rutina inmediata/manual | `riego/routine/config` | JSON retenido |
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
| Próximo despertar | `riego/device/sleep` | JSON retenido |
| Incidencia de conexión | `riego/device/problem` | JSON retenido |

Ejemplo de telemetría de batería:

```json
{"voltage":3.91,"percent":71}
```

Ejemplo de próximo despertar:

```json
{"sleepSeconds":300,"publishedAtEpoch":1782896400,"wakeAtEpoch":1782896700,"reason":"poll","routineActive":false}
```

Campos de `riego/device/sleep`:

- `sleepSeconds`: duración de deep sleep que el ESP32 acaba de programar.
- `publishedAtEpoch`: instante Unix en segundos cuando se publicó el mensaje,
  o `null` si no se pudo sincronizar NTP.
- `wakeAtEpoch`: instante Unix estimado del próximo despertar, o `null` sin
  NTP.
- `reason`: motivo del despertar previsto: `poll` para el ciclo normal,
  `scheduled_start` para arrancar una rutina programada, `routine_check` para
  comprobar cambios durante una zona activa y `routine_step` para pasar al
  siguiente paso de una rutina en curso.
- `routineActive`: `true` si queda una rutina local en progreso.

La PWA puede mostrar una cuenta atrás fiable usando `wakeAtEpoch`:

```js
const secondsLeft = Math.max(0, sleep.wakeAtEpoch - Math.floor(Date.now() / 1000));
```

Si `wakeAtEpoch` es `null`, el frontend puede usar `sleepSeconds` como contador
aproximado desde el momento de recepción del mensaje, pero no sabrá cuánto
tiempo llevaba retenido en MQTT.

Ejemplo de incidencia recuperada:

```json
{"active":false,"resolved":true,"code":"mqtt_failed","operation":"retained_commands","detail":"No se pudo conectar a MQTT; no se pudieron leer las ordenes retenidas.","count":1,"occurredAtEpoch":1782896400,"retrySeconds":300,"retryAtEpoch":1782896700,"resolvedAtEpoch":1782896712}
```

El ESP32 guarda estas incidencias en memoria RTC cuando no puede conectar a
Wi-Fi o MQTT y, por tanto, no puede leer los mensajes retenidos. Si en ese
momento no tiene conexión, no puede avisar inmediatamente; publica la incidencia
en `riego/device/problem` en el siguiente contacto MQTT exitoso. La PWA además
detecta si pasa `wakeAtEpoch` sin recibir una nueva telemetría de sueño y muestra
un reintento estimado usando el ciclo normal de 5 minutos.

Ejemplo de estado de rutina:

```json
{"status":"watering","id":101,"step":1,"stepCount":2,"openZone":1,"nextWakeSeconds":300}
```

Estados posibles:

- `idle`: no hay rutina activa.
- `scheduled`: hay una rutina programada antes del siguiente ciclo y el deep
  sleep se ha ajustado para despertar unos segundos antes de su hora de inicio.
- `watering`: hay una zona abierta y el ESP32 dormira hasta el siguiente chequeo
  de 1 minuto o hasta el siguiente paso, lo que llegue antes.
- `finished`: la rutina termino correctamente.
- `disabled`: la rutina fue cancelada desde MQTT.
- `stopped`: la rutina en curso se detuvo porque la programación retenida fue
  cambiada, desactivada o borrada.

## Ciclo de deep sleep

El intervalo por defecto, cuando no hay rutina activa, esta definido en
`src/main.cpp`:

```cpp
constexpr uint64_t DEFAULT_SLEEP_MINUTES = 5;
```

En cada ciclo normal:

1. Configura todos los pines de válvulas como salida y los deja en `LOW`.
2. Si había una rutina activa antes de dormir, calcula cuánto le queda a la zona
   actual. Si todavía queda tiempo, mantiene la zona abierta y solo hace un
   chequeo MQTT; si terminó el tiempo, cierra la zona y deja pendiente el cambio
   hasta comprobar MQTT.
3. Conecta a Wi-Fi y sincroniza hora por NTP.
4. Conecta a HiveMQ mediante TLS.
5. Publica batería y estado de rutina.
6. Se suscribe a la programación, al topic de rutina inmediata y a los 4 topics
   de comando.
7. Escucha durante 7 segundos para recibir mensajes retenidos.
8. Si estaba cambiando de zona, comprueba que la rutina siga existiendo sin
   cambios en la programación retenida. Si cambió o fue desactivada, se detiene;
   si sigue vigente, abre la siguiente zona o marca la rutina como `finished`.
9. Si no hay rutina activa, calcula la próxima rutina programada. Si empieza
   antes del siguiente ciclo por defecto, ajusta el deep sleep para despertar
   unos segundos antes de su `startTime`; si no, duerme los 5 minutos por
   defecto.
10. Publica `riego/device/sleep`, publica `sleeping`, desconecta y entra en
    deep sleep.

Con una rutina activa, el deep sleep despierta cada minuto para comprobar si se
ha modificado o cancelado la rutina, y también despierta justo cuando toca el
siguiente cambio de zona. Por ejemplo, una rutina de 5 minutos en zona 1 y
12 minutos en zona 2 se ejecuta así:

1. Detecta que la rutina empieza antes del siguiente ciclo y duerme hasta unos
   segundos antes de su hora de inicio.
2. Despierta, comprueba la programación retenida, abre zona 1 y duerme 5
   minutos en tramos máximos de 1 minuto, manteniendo la válvula abierta entre
   chequeos.
3. Al agotarse la zona 1, cierra zona 1, comprueba que la programación no
   cambió, abre zona 2 y repite chequeos de 1 minuto hasta completar sus
   12 minutos.
4. Al agotarse la zona 2, cierra zona 2, marca la rutina como `finished` y
   vuelve al ciclo
   normal de 5 minutos.

Si falla Wi-Fi o MQTT, el equipo vuelve igualmente a dormir para proteger la
batería y reintenta. Si el fallo ocurre durante una zona activa, la zona sigue
abierta y se reintenta en 1 minuto; si ocurre justo al cambiar de zona, la zona
terminada ya queda cerrada y no se abre la siguiente hasta poder comprobar MQTT.

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
