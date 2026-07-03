# Esquema de conexiones definitivo

## Proyecto Riego CC2

Este documento contiene el mapa exacto pin a pin para soldar en la placa PCB perforada.

## Regla de oro para la soldadura

- Los cables marcados con la etiqueta **[⚡ POTENCIA]** van a soportar corrientes altas, con latigazos de 1 amperio. Deben soldarse con cables de cobre gruesos o creando pistas muy anchas de estaño.
- Los cables marcados con **[🧠 LÓGICA]** llevan corrientes minúsculas. Puedes usar cablecillos finos normales.

## 1. Subsistema de energía: baterías y carga

La placa Lolin32 Lite gestiona la energía. Las baterías alimentan el sistema y el panel solar recarga las baterías a través de la placa.

### Baterías: portapilas

- Cable **ROJO (+)** -> al polo **POSITIVO** del conector blanco JST PH2.0 de la Lolin32.
- Cable **NEGRO (-)** -> al polo **NEGATIVO** del conector blanco JST PH2.0.

> Nota: Usa clemas, Wagos o empalme soldado para unir el portapilas al cablecito JST.

### Panel solar: 5 W

- Cable **ROJO (+)** -> a un pin de una clema -> cable hasta el pin **5V** o **USB/VBUS** de la Lolin32. **[⚡ POTENCIA]**
- Cable **NEGRO (-)** -> al pin de la clema -> cable hasta cualquier pin **GND** de la Lolin32. **[⚡ POTENCIA]**

## 2. Subsistema de potencia: generación de los 9 V

Aquí robamos energía directamente de las baterías, sin pasar por el delicado chip de 3.3 V, y la subimos a 9 V, almacenándola en el condensador.

### Entrada del elevador MT3608

- Pin **BAT** o **VBAT** de la Lolin32 -> pin **VIN+** del MT3608. **[⚡ POTENCIA]**
- Pin **GND** de la Lolin32 -> pin **VIN-** del MT3608. **[⚡ POTENCIA]**

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

### Módulo DRV8833 Nº1: zonas 1 y 2

- Pin **GPIO 4** de la Lolin32 -> pin **IN1**: abre zona 1.
- Pin **GPIO 16** de la Lolin32 -> pin **IN2**: cierra zona 1.
- Pin **GPIO 17** de la Lolin32 -> pin **IN3**: abre zona 2.
- Pin **GPIO 18** de la Lolin32 -> pin **IN4**: cierra zona 2.

### Módulo DRV8833 Nº2: zonas 3 y 4

- Pin **GPIO 19** de la Lolin32 -> pin **IN1**: abre zona 3.
- Pin **GPIO 21** de la Lolin32 -> pin **IN2**: cierra zona 3.
- Pin **GPIO 22** de la Lolin32 -> pin **IN3**: abre zona 4.
- Pin **GPIO 23** de la Lolin32 -> pin **IN4**: cierra zona 4.

## 5. Salidas a las válvulas Rain Bird

Conexiones desde los módulos DRV8833 hasta las clemas atornillables donde irán los cables del jardín. Todas estas pistas son **[⚡ POTENCIA]**.

- **Válvula 1:** pines **OUT1** y **OUT2** del DRV Nº1 -> clema de la zona 1.
- **Válvula 2:** pines **OUT3** y **OUT4** del DRV Nº1 -> clema de la zona 2.
- **Válvula 3:** pines **OUT1** y **OUT2** del DRV Nº2 -> clema de la zona 3.
- **Válvula 4:** pines **OUT3** y **OUT4** del DRV Nº2 -> clema de la zona 4.

> Nota: Si alguna válvula se cierra al darle a "Abrir", simplemente desatornilla sus dos cables de la clema, dales la vuelta e inviértelos, o cámbialo en el código de C++.
