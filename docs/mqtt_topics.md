# TOPICS MQTT Riego CC2

## Cluster HiveMQ
**URL:** 36259d97745649d69b665673ad1883e7.s1.eu.hivemq.cloud
**Puerto:** 8883

## OTA

* `riego/device/ota/cmd`: comando retenido que se publica manualmente al querer actualizar.
* `riego/device/ota/state`: estado retenido publicado por el ESP32.

Si el cliente web de HiveMQ no ofrece `retain`, usar `tools/ota-publisher.html`
desde el navegador local para publicar la orden OTA retenida.

Ejemplo:

```json
{
  "enabled": true,
  "version": "firmware-v2026.07.06-2",
  "url": "https://github.com/jmffreige/riego-cc2/releases/download/firmware-v2026.07.06-2/firmware.bin",
  "sha256": "..."
}
```
