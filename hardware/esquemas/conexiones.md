# Esquema de conexiones definitivo

## Proyecto Riego CC2

Este documento contiene el mapa exacto pin a pin para soldar en la placa PCB perforada.

## Regla de oro para la soldadura

- Los cables marcados con la etiqueta **[⚡ POTENCIA]** van a soportar corrientes altas, con latigazos de 1 amperio. Deben soldarse con cables de cobre gruesos o creando pistas muy anchas de estaño.
- Los cables marcados con **[🧠 LÓGICA]** llevan corrientes minúsculas. Puedes usar cablecillos finos normales.

## 1. Subsistema de energía: baterías y carga

La placa Lolin32 Lite gestiona la energía. Las baterías alimentan el sistema y el panel solar recarga las baterías a través de la placa.

> Importante: La **Lolin32 Lite no tiene pin BAT/VBAT accesible** en las tiras laterales. El único acceso directo a la batería es el conector blanco JST PH2.0. Por eso el elevador MT3608 debe alimentarse desde una bifurcación física de los cables de batería, no desde un pin de la Lolin32.

### Baterías: portapilas

Usa dos clemas como entrada general de batería en la placa perforada:

- **Clema BATERIA+**: punto de reparto para el positivo.
- **Clema BATERIA-/GND**: punto de reparto para el negativo y masa común.

### Reparto del positivo: bifurcación en Y

- Cable **ROJO (+)** del portapilas -> clema **BATERIA+**. **[⚡ POTENCIA]**
- Desde clema **BATERIA+** -> cable rojo del conector **JST PH2.0** que va a la Lolin32. **[⚡ POTENCIA]**
- Desde clema **BATERIA+** -> pin **VIN+** del elevador MT3608. **[⚡ POTENCIA]**

### Reparto del negativo: masa común

- Cable **NEGRO (-)** del portapilas -> clema **BATERIA-/GND**. **[⚡ POTENCIA]**
- Desde clema **BATERIA-/GND** -> cable negro del conector **JST PH2.0** que va a la Lolin32. **[⚡ POTENCIA]**
- Desde clema **BATERIA-/GND** -> pin **VIN-** del elevador MT3608. **[⚡ POTENCIA]**
- Desde clema **BATERIA-/GND** -> línea de masa común **GND** de la placa perforada. **[⚡ POTENCIA]**

> Nota: Usa clemas, Wagos o empalme soldado para unir el portapilas al cablecito JST.

### Medición de batería con divisor 100k/100k

La batería son dos celdas Li-ion en paralelo (**1S2P**), así que el voltaje máximo
del pack sigue siendo **4.20 V**. Para que el ESP32 pueda medirlo sin superar
3.3 V en el ADC, se usa un divisor resistivo externo 1:2.

- Clema **BATERIA+** -> resistencia **100 kΩ** -> punto medio del divisor. **[🧠 LÓGICA]**
- Punto medio del divisor -> pin **GPIO 34** de la Lolin32. **[🧠 LÓGICA]**
- Punto medio del divisor -> resistencia **100 kΩ** -> **BATERIA-/GND**. **[🧠 LÓGICA]**

Con la batería llena a 4.20 V, el GPIO34 recibe aproximadamente 2.10 V. El GND
de la batería ya es común con la Lolin32, el MT3608 y los DRV8833.

> Importante: este divisor solo es válido para un pack 1S/1S2P. No usarlo con
> celdas en serie, porque el punto medio podría superar el rango seguro del ESP32.

### Panel solar: 5 W

- Cable **ROJO (+)** -> a un pin de una clema -> cable hasta el pin **5V** o **USB/VBUS** de la Lolin32. **[⚡ POTENCIA]**
- Cable **NEGRO (-)** -> al pin de la clema -> cable hasta cualquier pin **GND** de la Lolin32. **[⚡ POTENCIA]**

## 2. Subsistema de potencia: generación de los 9 V

Aquí tomamos energía directamente de las baterías, sin pasar por el regulador de 3.3 V ni por ningún pin de potencia de la Lolin32, y la subimos a 9 V, almacenándola en el condensador.

### Entrada del elevador MT3608

- Clema **BATERIA+** -> pin **VIN+** del MT3608. **[⚡ POTENCIA]**
- Clema **BATERIA-/GND** -> pin **VIN-** del MT3608. **[⚡ POTENCIA]**

> Nota: El MT3608 queda en una rama paralela a la Lolin32. Cuando pida corriente para abrir o cerrar una válvula, la corriente sale directamente de las baterías hacia el elevador, sin atravesar el ESP32 ni su regulador.

### Salida del elevador: autopista de 9 V

- Pin **VOUT+** del MT3608 -> pata larga **(+)** del condensador de 4700 uF. **[⚡ POTENCIA]**
- Pin **VOUT-** del MT3608 -> pata corta **(-/franja)** del condensador de 4700 uF. **[⚡ POTENCIA]**

> Nota: El polo negativo del condensador debe unirse también a la masa común **GND** de toda la placa.

## 3. Subsistema de motores: puentes H

Alimentación bruta y activación de los dos chips DRV8833.

### El músculo: alimentación a 9 V

- Pata larga **(+)** del condensador -> pin **VCC** o **VM** del DRV8833 Nº1. **[⚡ POTENCIA]**
- Pata larga **(+)** del condensador -> pin **VCC** o **VM** del DRV8833 Nº2. **[⚡ POTENCIA]**

### La masa

- Masa común **GND** -> pin **GND** del DRV8833 Nº1. **[⚡ POTENCIA]**
- Masa común **GND** -> pin **GND** del DRV8833 Nº2. **[⚡ POTENCIA]**

### El despertador: activación a 3.3 V

- Pin **3V3** de la Lolin32 -> pin **EEP** del DRV8833 Nº1. **[🧠 LÓGICA]**
- Pin **3V3** de la Lolin32 -> pin **EEP** del DRV8833 Nº2. **[🧠 LÓGICA]**

## 4. Señales de control: cerebro

Estas son las órdenes de apertura y cierre que viajan desde el ESP32 hasta los puentes H. Todas estas conexiones son **[🧠 LÓGICA]**.

> Importante: esta polaridad está verificada con las válvulas reales. En este
> montaje los pines `IN1/IN3` de cada DRV cierran y los pines `IN2/IN4` abren.
> Si se invierten los dos cables de una válvula en su clema, también se invierte
> su sentido físico.

### Módulo DRV8833 Nº1: zonas 1 y 2

- Pin **GPIO 4** de la Lolin32 -> pin **IN1**: cierra zona 1.
- Pin **GPIO 16** de la Lolin32 -> pin **IN2**: abre zona 1.
- Pin **GPIO 17** de la Lolin32 -> pin **IN3**: cierra zona 2.
- Pin **GPIO 18** de la Lolin32 -> pin **IN4**: abre zona 2.

### Módulo DRV8833 Nº2: zonas 3 y 4

- Pin **GPIO 26** de la Lolin32 -> pin **IN1**: cierra zona 3.
- Pin **GPIO 27** de la Lolin32 -> pin **IN2**: abre zona 3.
- Pin **GPIO 32** de la Lolin32 -> pin **IN3**: cierra zona 4.
- Pin **GPIO 33** de la Lolin32 -> pin **IN4**: abre zona 4.

## 5. Salidas a las válvulas Rain Bird

Conexiones desde los módulos DRV8833 hasta las clemas atornillables donde irán los cables del jardín. Todas estas pistas son **[⚡ POTENCIA]**.

- **Válvula 1:** pines **OUT1** y **OUT2** del DRV Nº1 -> clema de la zona 1.
- **Válvula 2:** pines **OUT3** y **OUT4** del DRV Nº1 -> clema de la zona 2.
- **Válvula 3:** pines **OUT1** y **OUT2** del DRV Nº2 -> clema de la zona 3.
- **Válvula 4:** pines **OUT3** y **OUT4** del DRV Nº2 -> clema de la zona 4.

> Nota: Si alguna válvula se cierra al darle a "Abrir", simplemente desatornilla sus dos cables de la clema, dales la vuelta e inviértelos, o cámbialo en el código de C++.

> Importante: las salidas de válvulas no comparten común entre ellas. Cada
> electroválvula usa solamente su pareja `OUTx/OUTy`; no unir negativos ni
> retornos de válvulas distintas. Lo que sí debe ser común es la masa electrónica:
> batería, Lolin32, MT3608 y GND de ambos DRV8833.

## 6. Notas de firmware relacionadas con el cableado

- Los pulsos de apertura/cierre son de **50 ms**.
- Entre cerrar una zona y abrir la siguiente el firmware espera **15 s** para
  que el condensador de 4700 uF recupere carga.
- El entorno PlatformIO `valve_test` permite probar apertura y cierre de cada
  zona sin Wi-Fi ni deep sleep:

```powershell
pio run -d .\firmware -e valve_test --target upload
```

- Para volver al firmware real:

```powershell
pio run -d .\firmware -e esp32dev --target upload
```
