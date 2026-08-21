# Challenge-1: Sistema IoT Integrado de Borde para el Monitoreo Hidrometeorológico y Alerta Temprana en Sabana Centro

---

## 🌊 Visión del Proyecto
Nodo IoT de procesamiento en el borde (*Edge Computing*) concebido como una solución ultra económica (**BOM de $22.70 USD**, donde el ESP32 a $4.50 USD es el componente más caro) para la detección temprana de riesgo hídrico en reservorios y acueductos veredales de la provincia de Sabana Centro, Cundinamarca (Latitud 4.81° N), en el contexto de la crisis por el Fenómeno de El Niño 2026.

El sistema opera con **100% de autonomía local (cero costos mensuales de conectividad o infraestructura en la nube)**, combinando:
1. **Compensación termo-acústica dinámica** del tiempo de vuelo ultrasónico (HC-SR04 + BMP280).
2. **Cuantificación volumétrica de caudal** mediante interrupciones por hardware (ISR) en sensor Hall (YF-S201).
3. **Cálculo astronómico de Evapotranspiración de Referencia ($ET_0$)** mediante el modelo de Hargreaves-Samani adaptado a la latitud local.
4. **Matriz de Fusión Multisensorial de Borde** que clasifica el estado en 4 niveles de riesgo (Normal, Estrés, Alerta, Emergencia).
5. **Señalización In-Situ** mediante display LCD 1602A (paralelo 4 bits), código cromático de LEDs y patrones acústicos en buzzer de 85 dB.

---

## 📁 Estructura del Repositorio
* `src/`: Firmware simplificado y modular para ESP32 junto con suite de pruebas unitarias.
  * `WaterMonitorCore.h`: Declaración de interfaces matemáticas y constantes físicas.
  * `WaterMonitorCore.cpp`: Implementación en C++ estándar de los algoritmos de borde.
  * `esp32_water_monitor.ino`: Firmware para ESP32 DevKit v1 con lectura no bloqueante de sensores y HMI.
  * `test_water_monitor.cpp`: Suite de pruebas unitarias numéricas.
  * `Makefile`: Automatización de compilación y ejecución de tests locales.
* `wokwi/`:
  * `diagram.json`: Configuración del circuito en simulador Wokwi (ESP32, HC-SR04, BMP280, LCD1602A, Buzzer, LEDs, Pulsador Hall).
  * `bmp280.chip.json`: Descriptor de chip personalizado Wokwi para el sensor BMP280.
* `../Challenge-1.wiki/`: Documentación técnica completa en formato Wiki con normas IEEE.

---

## 🚀 Cómo Empezar

### 1. Validación de Algoritmos Locales (C++ Unit Tests)
Para compilar y ejecutar la suite de pruebas unitarias en tu máquina local:
```powershell
cd src
make run
```
*O con compilación directa g++:*
```powershell
g++ -std=c++17 -Wall -Wextra -pedantic WaterMonitorCore.cpp test_water_monitor.cpp -o test.exe
.\test.exe
```

### 2. Simulación Interactiva en Wokwi
1. Abrir [Wokwi](https://wokwi.com/) e importar `wokwi/diagram.json` y el código de `src/esp32_water_monitor.ino` con `src/WaterMonitorCore.h` y `src/WaterMonitorCore.cpp`.
2. Iniciar la simulación:
   * Ajustar el deslizador de distancia del sensor ultrasónico HC-SR04 para variar el nivel del reservorio.
   * Modificar la temperatura en el sensor BMP280 para observar la corrección acústica de velocidad del sonido y el recálculo de $ET_0$.
   * Presionar el botón azul (*Flow Pulse*) para simular pulsos del caudalímetro Hall en GPIO 14.
   * Observar las transiciones automáticas entre los estados de riesgo en el display LCD 1602A, LEDs de estado y buzzer.

---

## 📊 Especificaciones Técnicas y Mapeo de Pines

| Dispositivo | Pin Dispositivo | Pin ESP32 | Función |
| :--- | :--- | :--- | :--- |
| **HC-SR04** | Trigger | **GPIO 18** | Disparo de pulso acústico (10 $\mu$s) |
| **HC-SR04** | Echo | **GPIO 19** | Entrada eco (divisor resistivo 5V a 3.3V) |
| **Sensor Hall** | Pulso | **GPIO 14** | Entrada interrupción externa (ISR) |
| **BMP280** | SDA / SCL | **GPIO 21 / 22** | Bus I2C (dirección `0x76`) |
| **LCD 1602A** | RS | **GPIO 23** | Register Select (comando / dato) |
| **LCD 1602A** | E | **GPIO 17** | Enable (strobe de escritura) |
| **LCD 1602A** | D4 | **GPIO 16** | Bus paralelo 4 bits – bit 4 |
| **LCD 1602A** | D5 | **GPIO 4** | Bus paralelo 4 bits – bit 5 |
| **LCD 1602A** | D6 | **GPIO 5** | Bus paralelo 4 bits – bit 6 |
| **LCD 1602A** | D7 | **GPIO 13** | Bus paralelo 4 bits – bit 7 |
| **LCD 1602A** | RW | **GND** | Solo escritura |
| **LCD 1602A** | VDD / A | **5V** | Alimentación panel y retroiluminación |
| **LCD 1602A** | VSS / V0 / K | **GND** | Tierra, contraste máximo, retroiluminación (–) |
| **Buzzer** | Ánodo (+) | **GPIO 27** | Alerta acústica 85 dB |
| **LEDs Estado** | R / G / B | **GPIO 25, 26, 33** | Señalización óptica tricolor |
