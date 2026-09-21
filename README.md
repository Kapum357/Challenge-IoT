# Challenge: Sistema IoT Integrado de Borde para el Monitoreo Hidrometeorológico y Alerta Temprana en Sabana Centro

---

## 🌊 Visión del Proyecto
Nodo IoT de procesamiento en el borde (*Edge Computing*) concebido como una solución de ultra bajo costo (**BOM optimizado de $24.00 USD**, donde el ESP32 a **$4.50 USD** se preserva rigurosamente como el componente individual más costoso del sistema) para la detección temprana de riesgo hídrico, sequía y balance hidrometeorológico en reservorios y acueductos veredales de la provincia de Sabana Centro, Cundinamarca (Latitud 4.81° N, 2,600 m.s.n.m.), en el marco de la crisis climática por el Fenómeno de El Niño.

El sistema opera con **100% de autonomía in-situ (cero costos mensuales de conectividad o infraestructura cloud)** mediante una **arquitectura concurrente dual-core sobre FreeRTOS**:
1. **Compensación termo-acústica dinámica**: Corrección del tiempo de vuelo ultrasónico (HC-SR04) en función de la temperatura y humedad ambiental leídas por el sensor **BME280** vía I2C. Modo *fail-safe* que emite centinela de error en eco inválido en lugar de asumir tanque lleno.
2. **Cuantificación volumétrica de caudal**: Detección de pulsos de turbina Hall (YF-S201) mediante interrupciones de hardware (`IRAM_ATTR flowPulseISR`) protegidas por spinlock dual-core (`portMUX_TYPE`).
3. **Cálculo astronómico de Evapotranspiración ($ET_0$)**: Modelo de Hargreaves-Samani parametrizado para la radiación extraterrestre ($R_a$) y latitud de Sabana Centro.
4. **Matriz de Fusión Multisensorial de Riesgo con Histéresis**: Clasificación autónoma en 4 estados operativos (0: Normal, 1: Estrés Moderado, 2: Alerta Severa, 3: Emergencia Crítica) filtrados por `RiskStateMachine` (3 muestras consecutivas para escalar, 5 para des-escalar).
5. **Buffer Circular determinístico en SRAM (`SramRingBuffer`)**: 360 registros empacados de 37 bytes (`TelemetryRecord`) sincronizados por FreeRTOS Mutex, evitando el desgaste de memoria Flash.
6. **Visualización In-Situ Autónoma**: Display LCD 1602A (interfaz paralelo 4 bits) con página fija de alta visibilidad (IP de enlace + Estado de riesgo y buzzer con actualización diferencial anti-parpadeo), LEDs tricolor y buzzer piezoeléctrico de 85 dB con **perro guardián (*watchdog*) de silenciamiento remoto**.
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
  * `diagram.json`: Configuración del circuito en simulador Wokwi (ESP32 DevKit v1, HC-SR04, chip BME280 I2C, LCD 1602A 4-bit, LEDs, Buzzer, Pulsador Hall).
  * `bme280.chip.json`: Descriptor del chip personalizado Wokwi para BME280 con controles interactivos para dirección I2C (0x76/0x77), temperatura (-10 a 60 °C), presión (600 a 1100 hPa) y humedad relativa (0 a 100 % HR).
  * `bme280.chip.c`: Implementación en C del transductor BME280 para la API Wokwi, incluyendo emulación horaria acelerada (`accel=1440`).
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
   * Variar el deslizador del sensor **BME280** para alterar temperatura, presión o humedad relativa (% HR).
   * Mover la distancia del sensor ultrasónico HC-SR04 para simular fluctuaciones en el nivel del agua.
   * Pulsar el botón azul (*Flow Pulse*) en GPIO 14 para inyectar pulsos de caudal volumétrico.
   * **Lectura del Display LCD 1602A (Página Única Fija)**:
     * **Fila 0**: `IP:192.168.4.1` (o IP asignada en red STA de la alcaldía), permitiendo a las autoridades conocer de inmediato la URL del servidor web embebido.
     * **Fila 1**: `R:NORMAL BUZ:OFF` (Estado de riesgo actual y condición acústica; indica `BUZ:MUT` si fue silenciado o `*TEST*` en modo de prueba manual).
   * Comprobar la respuesta óptica y acústica en los LEDs tricolor y buzzer piezoeléctrico.

### 3. Conexión Inalámbrica Directa Punto a Punto (SoftAP + Web SPA Dashboard)
El nodo no requiere router externo ni conexión a internet para operar en campo:
* **SSID Wi-Fi del ESP32**: `AguaSabana-Nodo` (Contraseña: `sabana2026`).
* **Acceso desde Celular o PC**: Abrir el navegador en `http://192.168.4.1` (o la IP reportada en la Fila 0 del LCD).
* **Características del SPA Dashboard**:
  * **Gráfica Interactiva en HTML5 Canvas Puro**: Dibuja la serie temporal de nivel y humedad relativa a partir de los 360 registros en SRAM de `/api/history` sin depender de CDNs externas.
  * **Telemetría en Vivo (1 Hz vía AJAX `/api/data`)**: Nivel, porcentaje, caudal instantáneo, descenso, temperatura, humedad relativa BME280, presión y $ET_0$.
  * **Seguridad Perimetral y Autenticación**:
    * Filtro estricto de subred: únicamente se aceptan clientes pertenecientes a la subred de la interfaz activa (`192.168.4.0/24` en SoftAP o subred local autorizada en STA); peticiones de otras redes reciben `403 Forbidden`.
    * Formulario de inicio de sesión (`/login`): autenticación con usuario `admin` y contraseña `sabana2026`, validada mediante SHA-256 con salt (`mbedtls`).
    * Manejo de sesiones: Cookie `session=<token>` (`HttpOnly; SameSite=Strict; Path=/; Max-Age=900`) con soporte para hasta 4 sesiones activas y cierre de sesión (`/logout`).
  * **Watchdog de Silenciamiento de Alarma**: Botón para silenciar el buzzer durante 5 minutos (`silence 300`), reactivándose automáticamente si la emergencia persiste.

### 4. Consola de Servicio Embebida (UART0 @ 115200 baud)
Comandos implementados en el monitor serie (`SabanaNode>`):
* `help`: Menú interactivo de comandos disponibles.
* `status`: Diagnóstico completo de telemetría, memoria FreeRTOS, estado del BME280 y buffer histórico en SRAM.
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

### Bill of Materials (BOM) Optimizado — Total: $24.00 USD
| Componente | Modelo / Especificación | Función en el Sistema | Costo Unitario |
| :--- | :--- | :--- | :--- |
| **ESP32-WROOM-32 DevKit v1** | Dual Core Xtensa LX6 @ 240 MHz | **Unidad Central y Componente Más Costoso** | **$4.50 USD** (18.75% BOM) |
| **Sensor de Caudal Hall** | YF-S201 (Rotor de turbina) | Cuantificación volumétrica de descarga | **$3.50 USD** |
| **Sensor Termo-Higro-Barométrico** | **Bosch BME280 I2C (0x76/0x77)** | **Temperatura, Presión y Humedad Relativa** | **$2.80 USD** |
| **Display LCD 1602A** | 16x2 Paralelo 4 bits | Interfaz HMI local autónoma in-situ | **$2.50 USD** |
| **Sensor Ultrasónico de Nivel** | HC-SR04 con laca antihumedad | Medición de nivel con corrección acústica | **$2.20 USD** |
| **Fuente de Alimentación** | Adaptador USB 5V 2A para caseta | Alimentación continua 5V DC | **$2.00 USD** |
| **Gabinete de Paso** | Caja PVC/ABS estanca IP65 | Envolvente protectora para intemperie | **$2.00 USD** |
| **Alarma Acústica + Óptica** | Buzzer activo 85 dB + 3 LEDs | Alerta sonora y semáforo cromático de riesgo | **$1.80 USD** |
| **Acondicionamiento Pasivo** | Resistencias pull-up y divisores | Adaptación de niveles 5V a 3.3V | **$0.70 USD** |
| **TOTAL BOM** | — | **Costo Integral del Nodo de Borde** | **$24.00 USD** |

*Nota de Cumplimiento*: El microcontrolador ESP32 DevKit v1 ($4.50 USD) supera en costo a todos los demás componentes individuales ($3.50 USD caudalímetro, $2.80 USD BME280, etc.), satisfaciendo la directriz estricta del reto.

### Tabla Maestra de Asignación de Pines GPIO
| Dispositivo | Pin Dispositivo | Pin ESP32 | Tipo de Señal | Nivel Lógico | Notas de Acondicionamiento |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **HC-SR04** | Trigger | **GPIO 18** | Salida Digital | 3.3V CMOS | Disparo pulso 10 $\mu$s |
| **HC-SR04** | Echo | **GPIO 19** | Entrada Digital | 5V $\rightarrow$ 3.3V | Divisor resistivo ($R_1=1.0\text{ k}\Omega, R_2=2.0\text{ k}\Omega$) |
| **Sensor Hall YF-S201** | Señal | **GPIO 14** | Interrupción Externa | 5V $\rightarrow$ 3.3V | Pull-up 10k $\Omega$ + divisor $R_3=1.0\text{ k}\Omega, R_4=2.0\text{ k}\Omega$ |
| **BME280** | SDA / SCL | **GPIO 21 / 22** | Bus I2C (0x76 / 0x77) | 3.3V I2C | Detección dual y bus clock recovery |
| **LCD 1602A** | RS | **GPIO 23** | Salida Digital | 3.3V CMOS | Register Select |
| **LCD 1602A** | E | **GPIO 17** | Salida Digital | 3.3V CMOS | Enable Strobe |
| **LCD 1602A** | D4, D5, D6, D7 | **GPIO 16, 4, 5, 13** | Salidas Digitales | 3.3V CMOS | Bus paralelo de 4 bits |
| **LCD 1602A** | RW, VSS, V0, K | **GND** | Alimentación / Control | 0V | Modo escritura, contraste máximo, cátodo |
| **LCD 1602A** | VDD, A | **5V** | Alimentación | 5V DC | Panel y ánodo de retroiluminación |
| **Buzzer Activo** | Ánodo (+) | **GPIO 27** | Salida PWM / Tono | 3.3V / 5V | Señal acústica 85 dB con watchdog |
| **LEDs Estado** | Rojo / Verde / Azul | **GPIO 25, 26, 33** | Salidas Push-Pull | 3.3V CMOS | Resistencias limitadoras de $220\ \Omega$ |
