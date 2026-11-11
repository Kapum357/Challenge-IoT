# Challenge: Sistema IoT Integrado de Borde para el Monitoreo Hidrometeorológico y Alerta Temprana en Sabana Centro

---

## 🌊 Visión del Proyecto
Nodo IoT de procesamiento en el borde (*Edge Computing*) concebido como una solución de ultra bajo costo (**BOM optimizado de $20.50 USD en Challenge #3**, donde el ESP32 a **$4.50 USD** representa el **21.95%** del BOM y se preserva rigurosamente como el componente individual más costoso del sistema) para la detección temprana de riesgo hídrico, sequía y balance hidrometeorológico en reservorios y acueductos veredales de la provincia de Sabana Centro, Cundinamarca (Latitud 4.81° N, 2,600 m.s.n.m.), en el marco de la crisis climática por el Fenómeno de El Niño.

El sistema opera con **100% de autonomía in-situ (cero costos mensuales de conectividad o infraestructura cloud)** mediante una **arquitectura concurrente dual-core sobre FreeRTOS** y un **Gateway IoT en Raspberry Pi 100% en C++17**:
1. **Compensación termo-acústica dinámica**: Corrección del tiempo de vuelo ultrasónico (HC-SR04) en función de la temperatura ambiental leída por el sensor **BME280** vía I2C. Modo *fail-safe* que emite centinela de error en eco inválido en lugar de asumir tanque lleno. El BME280 proporciona además medición barométrica real (hPa) y humedad relativa (% HR).
2. **Poda de Hardware de Sensor de Flujo (Challenge #3)**: Eliminación completa del sensor de caudal `YF-S201` en hardware y software, respaldada por analítica cuantitativa en Gateway ($Z$-score de $dH/dt$, regresión lineal OLS y $VPD$). Se preserva bandera condicional `#ifdef ENABLE_FLOW_SENSOR` para retrocompatibilidad con Challenge #2.
3. **Cálculo astronómico de Evapotranspiración ($ET_0$)**: Modelo de Hargreaves-Samani parametrizado para la radiación extraterrestre ($R_a$) y latitud de Sabana Centro.
4. **Matriz de Fusión Multisensorial de Riesgo con Histéresis**: Clasificación autónoma en 4 estados operativos (0: Normal, 1: Estrés Moderado, 2: Alerta Severa, 3: Emergencia Crítica) filtrados por `RiskStateMachine` (3 muestras consecutivas para escalar, 5 para des-escalar).
5. **Buffer Circular determinístico en SRAM (`SramRingBuffer`)**: 360 registros empacados de 33 bytes (`TelemetryRecord` sin caudal, o 37 bytes con caudal) sincronizados sin contención.
6. **Módulo de IA Hidroclimática en Gateway (Raspberry Pi — 100% C++17)**: Ingesta MQTT con `libmosquitto`, base local `sqlite3` en modo WAL, analítica OLS/$Z$-score compartida con `WaterMonitorCore.cpp`, cliente OpenRouter Free con `libcurl` y fallback determinista local (< 1 ms). por FreeRTOS Mutex, evitando el desgaste de memoria Flash.
6. **Visualización In-Situ Autónoma**: Display TFT ILI9341 de 2.4 pulgadas (SPI, RGB565, 240x320, sin táctil) con página densa de alta visibilidad (IP, riesgo, buzzer, nivel, caudal, clima, ETo y estado de memoria), LEDs tricolor y buzzer piezoeléctrico de 85 dB con **perro guardián (*watchdog*) de silenciamiento remoto**.
7. **Servidor Web Embebido lwIP (Core 0)**: Single Page Application (SPA) en `PROGMEM` con gráficas interactivas nativas en **HTML5 Canvas** (cero dependencias CDN o librerías externas), REST APIs (`/api/data`, `/api/history`, `/api/cmd`), filtro estricto de subred `(clientIP & subnetMask) == (localIP & subnetMask)` y control de acceso mediante formulario web, tokens de sesión en Cookie (`HttpOnly; SameSite=Strict`) y verificación SHA-256 con salt.

---

## 📁 Estructura del Repositorio
* `src/`:
  * `WaterMonitorCore.h` y `WaterMonitorCore.cpp`: Definición y lógica matemática pura portable (sin dependencias Arduino): `TelemetryRecord` (37 bytes empacados), clase `SramRingBuffer` (360 muestras), `RiskStateMachine`, filtro de subred `isSubnetAllowed` y algoritmos hidrometeorológicos.
  * `esp32_water_monitor/`: Código del nodo embebido para Arduino IDE / PlatformIO / Wokwi.
    * `esp32_water_monitor.ino`: Firmware principal para ESP32 DevKit v1 con FreeRTOS dual-core (Core 0: Servidor HTTP lwIP y CLI; Core 1: Adquisición, Analítica y HMI local).
    * `WaterMonitorCore.h` y `WaterMonitorCore.cpp`: Copia sincronizada del núcleo portable para compilación directa en el entorno Arduino. *(Nota de mantenimiento: se recomienda mantener ambas copias sincronizadas o unificar mediante symlink/ruta de inclusión en CI)*.
  * `test_water_monitor.cpp`: Suite de pruebas unitarias exhaustiva (12 bloques temáticos, 81 aserciones cubriendo verificación matemática, empaquetado de estructuras, wrap-around del ring buffer, histéresis y filtros de seguridad).
  * `Makefile`: Automatización de compilación con `g++` (`-Wall -Wextra -pedantic`) y ejecución local de tests.
* `wokwi/`:
  * `diagram.json`: Configuración del circuito en simulador Wokwi (ESP32 DevKit v1, HC-SR04, BME280 I2C, ILI9341 SPI, microSD, LEDs, Buzzer y Pulsador Hall).
  * `wokwi.toml`: Configuración de enlace a binarios de firmware.
* `Challenge.wiki/`: Documentación técnica formal completa con Registros de Decisiones de Arquitectura (ADR), Business Model Canvas, diagramas UML y normas IEEE.

---

## 🚀 Cómo Empezar

### 1. Validación de Algoritmos Locales (C++ Unit Tests)
Para compilar y ejecutar la suite completa de pruebas unitarias en Windows/Linux (81 aserciones en 12 bloques temáticos):
```powershell
cd src
mingw32-make run
```
*O mediante compilación directa con g++:*
```powershell
g++ -std=c++17 -Wall -Wextra -pedantic WaterMonitorCore.cpp test_water_monitor.cpp -o water_monitor_test.exe
.\water_monitor_test.exe
```

### 2. Simulación Interactiva en Wokwi y Operación In-Situ
1. Abrir [Wokwi](https://wokwi.com/) e importar la carpeta `wokwi/` junto con el código del firmware `src/esp32_water_monitor/`.
2. Iniciar la simulación interactiva:
   * Variar el sensor **BME280** para alterar temperatura, presión (hPa) o humedad relativa (% HR).
   * Mover la distancia del sensor ultrasónico HC-SR04 para simular fluctuaciones en el nivel del agua.
   * Pulsar el botón azul (*Flow Pulse*) en GPIO 14 para inyectar pulsos de caudal volumétrico.
   * **Lectura del Display ILI9341 (Página Única Fija)**: IP/AP, riesgo, buzzer, nivel, porcentaje, caudal, descenso, temperatura, humedad, presión, ETo, estado BME280/SD y uptime.
   * Comprobar la respuesta óptica y acústica en los LEDs tricolor y buzzer piezoeléctrico.

### 3. Conexión Inalámbrica Resiliente (Default STA con Fallback Dinámico a SoftAP)
El nodo opera bajo una política de resiliencia de red de dos niveles, optimizada para infraestructura o despliegue aislado:
* **Modo Predeterminado (STA - Estación)**: Al arrancar, el ESP32 intenta conectarse a la red local (`WIFI_STA_SSID="Wokwi-GUEST"` o red institucional configurada).
* **Falla Estricta y Fallback a SoftAP (10 s)**: Si tras 10 segundos no se establece enlace con la red STA, conmuta automáticamente a Punto de Acceso local (`AguaSabana-Nodo` / `sabana2026` en `192.168.4.1`), garantizando acceso ininterrumpido sin intervención manual.
* **Supervisión Dinámica en Tiempo de Ejecución (Core 0)**: Si en operación normal se pierde el enlace STA, la rutina `manageWiFiConnectivity` levanta de inmediato el SoftAP y reintenta asociarse a la infraestructura cada 15 segundos sin bloquear el servidor web ni las tareas de medición en Core 1.
* **Acceso desde Celular o PC**: Abrir el navegador en `http://192.168.4.1` (en modo AP) o en la IP asignada por DHCP (en modo STA, reportada en la pantalla ILI9341).
* **Características del SPA Dashboard**:
  * **Gráfica Interactiva en HTML5 Canvas Puro**: Dibuja la serie temporal de nivel y humedad relativa a partir de los 360 registros en SRAM de `/api/history` sin depender de CDNs externas.
  * **Telemetría en Vivo (1 Hz vía AJAX `/api/data`)**: Nivel, porcentaje, caudal instantáneo, descenso, temperatura, humedad relativa, presión barométrica BME280 (hPa), $ET_0$ y estado de red (`wifi_sta`, `ap_fallback`).
  * **Seguridad Perimetral y Autenticación**:
    * Filtro estricto de subred: únicamente se aceptan clientes pertenecientes a la subred de la interfaz activa (`192.168.4.0/24` en SoftAP o subred local autorizada en STA); peticiones de otras redes reciben `403 Forbidden`.
    * Formulario de inicio de sesión (`/login`): autenticación con usuario `admin` y contraseña `sabana2026`, validada mediante SHA-256 con salt (`mbedtls`).
    * Manejo de sesiones: Cookie `session=<token>` (`HttpOnly; SameSite=Strict; Path=/; Max-Age=900`) con soporte para hasta 4 sesiones activas y cierre de sesión (`/logout`).
  * **Watchdog de Silenciamiento de Alarma**: Botón para silenciar el buzzer durante 5 minutos (`silence 300`), reactivándose automáticamente si la emergencia persiste.

### 4. Consola de Servicio Embebida (UART0 @ 115200 baud)
Comandos implementados en el monitor serie (`SabanaNode>`):
* `help`: Menú interactivo de comandos disponibles.
* `status`: Diagnóstico completo de telemetría, memoria FreeRTOS, estado del BME280/SD y buffer histórico en SRAM.
* `wifi`: Diagnóstico detallado de red (modo de operación, IP, máscara, gateway, RSSI y estado de fallback SoftAP).
* `test <0-3>`: Forzado manual de estados de riesgo (0: Normal, 1: Estrés, 2: Alerta, 3: Emergencia).
* `auto`: Retorno al modo automático de fusión sensorial.
* `silence [seg]`: Silenciamiento temporal del buzzer con perro guardián (valor por defecto: 300 s).
* `unsilence`: Reactivación inmediata del buzzer físico.
* `flow <factor>`: Ajuste dinámico del factor $K$ del caudalímetro Hall (rango válido: 0.5 a 50.0).
* `ntp`: Diagnóstico del estado de sincronización NTP, día juliano y temperaturas extremas en 24 h.
* `hysteresis`: Inspección de la máquina de estados de histéresis temporal (`RiskStateMachine`).

*(Nota técnica: comandos de diagnóstico avanzado como escaneo I2C y calibración no volátil se encuentran planificados en el backlog del firmware para futuras iteraciones).*

---

## 📊 Lista Maestra de Materiales (BOM) y Mapeo de Pines

### Bill of Materials (BOM) Optimizado — Total: $20.50 USD (Challenge #3)
| Componente | Modelo / Especificación | Función en el Sistema | Costo Unitario |
| :--- | :--- | :--- | :--- |
| **ESP32-WROOM-32 DevKit v1** | Dual Core Xtensa LX6 @ 240 MHz | **Unidad Central y Componente Más Costoso** | **$4.50 USD** (21.95% BOM) |
| **Sensor de Caudal Hall** | YF-S201 (Rotor de turbina) | *Podado en Ch3 por feedback de diseño* | **$0.00 USD** (Ch3) / $3.50 (Ch2) |
| **Sensor Termo-Higro-Barométrico** | **BME280 I2C (0x76/0x77)** | **Temperatura, Humedad Relativa y Presión Barométrica** | **$2.80 USD** |
| **Display TFT** | **ILI9341 SPI, 240x320, RGB565, sin táctil** | Interfaz HMI local autónoma in-situ | **$6.00 USD** |
| **Sensor Ultrasónico de Nivel** | HC-SR04 con laca antihumedad | Medición de nivel con corrección acústica | **$2.20 USD** |
| **Fuente de Alimentación** | Adaptador USB 5V 2A para caseta | Alimentación continua 5V DC | **$2.00 USD** |
| **Gabinete de Paso** | Caja PVC/ABS estanca IP65 | Envolvente protectora para intemperie | **$2.00 USD** |
| **Alarma Acústica + Óptica** | Buzzer activo 85 dB + 3 LEDs | Alerta sonora y semáforo cromático de riesgo | **$1.80 USD** |
| **Acondicionamiento Pasivo** | Resistencias pull-up y divisores | Adaptación de niveles 5V a 3.3V | **$0.70 USD** |
| **TOTAL BOM** | — | **Costo Integral del Nodo de Borde** | **$20.50 USD** |

*Nota de Cumplimiento*: El microcontrolador ESP32 DevKit v1 ($4.50 USD) se conserva con el 21.95% del presupuesto como el componente individual de mayor valor en el BOM.

### Tabla Maestra de Asignación de Pines GPIO
| Dispositivo | Pin Dispositivo | Pin ESP32 | Tipo de Señal | Nivel Lógico | Notas de Acondicionamiento |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **HC-SR04** | Trigger | **GPIO 4** | Salida Digital | 3.3V CMOS | Disparo pulso 10 $\mu$s; GPIO18 queda para VSPI SCK |
| **HC-SR04** | Echo | **GPIO 34** | Entrada Digital solo-entrada | 5V $\rightarrow$ 3.3V | Divisor resistivo obligatorio |
| **Sensor Hall YF-S201** | Señal | **GPIO 14** | Interrupción Externa | 5V $\rightarrow$ 3.3V | *Deshabilitado en Ch3 bajo `#if ENABLE_FLOW_SENSOR`* |
| **BME280** | SDA / SCL | **GPIO 21 / 22** | Bus I2C (0x76 / 0x77) | 3.3V I2C | Detección dual y recuperación de bus |
| **ILI9341** | SCK / MISO / MOSI | **GPIO 18 / 19 / 23** | VSPI compartido | 3.3V | RGB565; MISO también alimenta SD |
| **ILI9341** | CS / DC / RST / BL | **GPIO 5 / 16 / 17 / 32** | Control SPI | 3.3V | Sin panel táctil |
| **microSD** | CS | **GPIO 13** | VSPI compartido | 3.3V | Persistencia CSV; SRAM conserva tiempo real |
| **Buzzer Activo** | Ánodo (+) | **GPIO 27** | Salida PWM / Tono | 3.3V / 5V | Señal acústica 85 dB con watchdog |
| **LEDs Estado** | Rojo / Verde / Azul | **GPIO 25, 26, 33** | Salidas Push-Pull | 3.3V CMOS | Resistencias limitadoras de $220\ \Omega$ |

---

## 🤖 Challenge #3: Módulo de IA Hidroclimática en Gateway IoT (Raspberry Pi — 100% C++17)

### Arquitectura Unificada y Ventajas Industriales
El Gateway IoT opera sobre Raspberry Pi OS en **100% C++17 nativo** (`Challenge/gateway/`):
- **Reutilización Binaria Total**: `WaterMonitorCore.cpp` se enlaza directamente en el binario `sabana_gateway`, compartiendo idénticos cálculos astronómicos, térmicos y de histéresis.
- **Persistencia en SQLite Local**: Motor embebido en modo WAL (`telemetry` e `ia_recommendations`).
- **Analítica Cuantitativa Local**: Regresión lineal OLS sobre 60 minutos (`computeOlsSlope`) y puntaje $Z$ histórico (`computeZScore`).
- **Inferencia LLM con OpenRouter Free**: Invocación HTTPS mediante `libcurl` con rotación en modelos gratuitos (`gemini-2.0-flash-exp:free`, `llama-3.3-70b-instruct:free`, `qwen-2.5-72b-instruct:free`, `openrouter/free`).
- **Fallback Determinista Local**: Tiempo de respuesta $< 1\text{ ms}$ ante caídas de WAN o agotamiento de cuota.
- **Huella de Memoria Reducida**: Consumo $< 4\text{ MB}$ de RAM (frente a $> 45\text{ MB}$ en runtime Python).

### Compilación y Ejecución

#### 1. En Raspberry Pi OS (Gateway C++17)
```bash
sudo apt update && sudo apt install -y g++ libmosquitto-dev libsqlite3-dev libcurl4-openssl-dev nlohmann-json3-dev
cd Challenge/gateway
make
./sabana_gateway
```

#### 2. Pruebas Unitarias Unificadas (C++17)
```bash
cd Challenge/src
# Pruebas Challenge #3 (Sensor de caudal podado, TC-01 a TC-05)
mingw32-make clean; mingw32-make test

# Pruebas Challenge #2 (Retrocompatibilidad con sensor de caudal habilitado)
mingw32-make clean; mingw32-make test-flow
```
