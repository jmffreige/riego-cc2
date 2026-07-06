# Configuración del Cluster de HiveMQ

## Credenciales
* **Usuario ESP32**: jardinero_cc2
* **Usuario PWA**: usuario_cc2
* **Password**: Pepe-cc2

## General
* **URL**: 36259d97745649d69b665673ad1883e7.s1.eu.hivemq.cloud
* **Puerto**: 8883
* **Puerto WebSocket**: 8884

Capacidad para hasta 100 conexiones y tráfico de hasta 10 GB.

## OTA

La actualización OTA se activa manualmente publicando un JSON retenido en
`riego/device/ota/cmd`. No se configuran permisos especiales por usuario en
HiveMQ; basta con no exponer esa acción en la PWA normal.

## Información del Cluster
* **Plan** Serverless
* **Cluster ID**: 36259d97745649d69b665673ad1883e7
