# Frontend · Control-CC2

Panel web instalable para programar las cuatro áreas de riego del Jardín Frontal mediante MQTT y HiveMQ Cloud. El riego siempre se ejecuta de forma secuencial, de la zona 1 a la 4, con una sola válvula abierta.

## 1. Probar la interfaz en el ordenador

No abras `index.html` haciendo doble clic. La PWA y el service worker necesitan que la carpeta se sirva desde una dirección `http://`.

### Opción recomendada en Windows

Abre una terminal en la raíz del repositorio, donde están las carpetas `firmware` y `frontend`, y ejecuta:

```powershell
powershell -ExecutionPolicy Bypass -File .\frontend\serve.ps1
```

La terminal debería mostrar:

```text
Control-CC2 disponible en http://localhost:8080
Pulsa Ctrl+C para detener el servidor.
```

Abre entonces esta dirección en el navegador:

```text
http://localhost:8080
```

Para detener el servidor, vuelve a la terminal y pulsa `Ctrl+C`.

### Alternativa con Visual Studio Code

También puedes instalar la extensión **Live Server**, abrir `frontend/index.html` y pulsar **Go Live** en la barra inferior de VS Code.

## 2. Conectar la web con HiveMQ

Al abrir el panel aparecerá la ventana **Configurar HiveMQ**.

Los campos deben contener:

| Campo | Valor |
| --- | --- |
| Servidor | `36259d97745649d69b665673ad1883e7.s1.eu.hivemq.cloud` |
| Puerto WSS | `8884` |
| Usuario | `jardinero_cc2` |
| Contraseña | La contraseña configurada en HiveMQ |

Pulsa **Conectar panel**. Si todo funciona, la parte superior mostrará **En línea**.

La web utiliza:

| Función | Topic o mensaje |
| --- | --- |
| Recibir el estado | `riego/zona1/state` hasta `riego/zona4/state` |
| Enviar comandos | `riego/zona1/cmd` hasta `riego/zona4/cmd` |
| Guardar las rutinas | `riego/programacion/cmd` con un objeto JSON retenido |
| Abrir la válvula | `ON` |
| Cerrar la válvula | `OFF` |

Para probar el sistema completo:

1. Enciende el ESP32 y comprueba en el monitor serie que se conecta al Wi-Fi y a HiveMQ.
2. Abre la web y conecta el panel.
3. Entra en **Jardín Frontal** y configura la primera rutina: días, hora y minutos de cada zona.
4. Pulsa **Añadir rutina** para crear otros ciclos, por ejemplo uno por la mañana y otro por la tarde.
5. Pulsa **Guardar todas las rutinas**. La web publica un JSON retenido en `riego/programacion/cmd`.
6. Pulsa **Regar ahora** dentro de una rutina. La web abre la zona 1 y, al terminar su tiempo, la cierra antes de abrir la zona 2.
7. Comprueba que la secuencia continúa hasta la zona 4 sin que haya dos válvulas abiertas a la vez.
8. Pulsa **Detener** y comprueba que las cuatro zonas reciben la orden `OFF`.

Las rutinas se guardan también en `localStorage` para conservarlas en el navegador. Se pueden nombrar, activar, desactivar y eliminar individualmente. El mensaje MQTT tiene esta estructura:

```json
{
  "version": 2,
  "routines": [
    {
      "id": "routine-...",
      "name": "Mañana",
      "enabled": true,
      "dayMode": "daily",
      "days": [],
      "startTime": "07:00",
      "durations": [10, 8, 12, 5]
    },
    {
      "id": "routine-...",
      "name": "Tarde",
      "enabled": true,
      "dayMode": "selected",
      "days": [1, 3, 5],
      "startTime": "20:00",
      "durations": [6, 5, 8, 4]
    }
  ]
}
```

El firmware deberá suscribirse a `riego/programacion/cmd`, persistir la lista y ejecutar cada rutina activa de forma autónoma. Si dos rutinas se solapan, el firmware deberá mantener la regla de una sola válvula abierta y encolar o rechazar el segundo ciclo.

Si la web conecta pero el dispositivo no responde, revisa que el ESP32 y la web utilicen exactamente los mismos topics. El firmware debe implementar los topics de las zonas 2, 3 y 4 para que sus tarjetas controlen dispositivos reales.

## 3. Publicar rápidamente con Netlify

Esta es la forma más corta de obtener una URL HTTPS para probarla en el iPhone.

1. Entra en [Netlify Drop](https://app.netlify.com/drop).
2. Inicia sesión o crea una cuenta.
3. Abre el explorador de archivos de Windows.
4. Arrastra la carpeta `frontend` completa al área de publicación de Netlify.
5. Espera a que termine la subida.
6. Netlify mostrará una URL parecida a:

   ```text
   https://nombre-aleatorio.netlify.app
   ```

7. Abre esa URL y comprueba la conexión MQTT.

Debes arrastrar la carpeta `frontend`, no la carpeta completa `riego-cc2`. El archivo `index.html` tiene que quedar en la raíz de lo publicado.

Cuando cambies algún archivo, vuelve a arrastrar la carpeta `frontend` en la sección **Deploys** del proyecto.

## 4. Desplegar desde GitHub automáticamente

Esta opción requiere que los archivos de `frontend` ya estén guardados en GitHub. Cada nuevo `push` actualizará la web automáticamente.

Primero, desde la raíz del repositorio:

```powershell
git add frontend
git commit -m "Añadir frontend PWA para control de riego"
git push -u origin develop
```

Después, en Netlify:

1. Entra en [Netlify](https://app.netlify.com/).
2. Selecciona **Add new project**.
3. Selecciona **Import an existing project** o la opción para conectar un repositorio.
4. Elige **GitHub** y autoriza el acceso.
5. Selecciona el repositorio `riego-cc2`.
6. Configura el despliegue:

   | Ajuste | Valor |
   | --- | --- |
   | Branch to deploy | `develop` |
   | Base directory | Déjalo vacío |
   | Build command | Déjalo vacío |
   | Publish directory | `frontend` |

7. Pulsa **Deploy**.

No hay que configurar variables de entorno porque este frontend no tiene proceso de compilación.

## 5. Instalarla en el iPhone

Hazlo con la URL HTTPS de Netlify, no con `localhost`.

1. Abre la URL publicada utilizando **Safari**.
2. Pulsa el botón **Compartir**.
3. Selecciona **Añadir a pantalla de inicio**.
4. Revisa el nombre `Control-CC2`.
5. Pulsa **Añadir**.

El icono aparecerá en la pantalla de inicio y la aplicación se abrirá sin la barra normal del navegador.

## 6. Solución de problemas

### Aparece “No se pudo conectar”

- Comprueba la contraseña.
- Verifica que el usuario siga activo en HiveMQ.
- Utiliza el puerto WebSocket seguro `8884`, no el puerto MQTT del ESP32 `8883`.
- Comprueba que el host no incluya `https://`, `wss://` ni `/mqtt`.

### Los botones están desactivados

Los controles solo se habilitan cuando la etiqueta superior muestra **En línea**.

### La web no refleja el estado

- El ESP32 debe publicar en el topic de estado correspondiente, desde `riego/zona1/state` hasta `riego/zona4/state`.
- El mensaje debe ser exactamente `ON` u `OFF`.
- El firmware publica el estado después de recibir un comando.

### Los cambios no aparecen después de desplegar

El service worker puede conservar una versión anterior. Cierra la app y vuelve a abrirla. Si continúa igual, elimina los datos del sitio en el navegador o incrementa `CACHE_NAME` en `service-worker.js`.

## Seguridad

La contraseña se guarda únicamente en `sessionStorage` y desaparece al cerrar la sesión del navegador.

Una web estática no puede esconder credenciales incluidas en JavaScript. Para un despliegue público conviene crear en HiveMQ un usuario exclusivo para la web, con permisos limitados a:

- Suscripción a `riego/zona1/state` hasta `riego/zona4/state`.
- Publicación en `riego/zona1/cmd` hasta `riego/zona4/cmd`.
- Publicación en `riego/programacion/cmd`.
