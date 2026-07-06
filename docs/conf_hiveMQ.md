# Configuración del Cluster de HiveMQ

## Credenciales
* **Usuario ESP32**: jardinero_cc2
* **Usuario PWA**: usuario_cc2
* **Usuario admin OTA**: crear uno específico para publicar en `riego/device/ota/cmd`
* **Password**: Pepe-cc2

## General
* **URL**: 36259d97745649d69b665673ad1883e7.s1.eu.hivemq.cloud
* **Puerto**: 8883
* **Puerto WebSocket**: 8884

Capacidad para hasta 100 conexiones y tráfico de hasta 10 GB.

## Permisos recomendados

* `usuario_cc2` (PWA normal): publicar en `riego/programacion/cmd`,
  `riego/routine/config` y `riego/zona+/cmd`; suscribirse a `riego/#`; sin
  permiso de publicación en `riego/device/ota/cmd`.
* `jardinero_cc2` (ESP32): suscribirse a `riego/programacion/cmd`,
  `riego/routine/config`, `riego/device/ota/cmd` y `riego/zona+/cmd`; publicar
  en `riego/device/#`, `riego/routine/state` y `riego/zona+/state`.
* Admin OTA: publicar en `riego/device/ota/cmd` y suscribirse a
  `riego/device/ota/state`.

## Información del Cluster
* **Plan** Serverless
* **Cluster ID**: 36259d97745649d69b665673ad1883e7
