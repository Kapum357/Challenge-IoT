# Actividad de Refuerzo #2.3 — Conectividad en IoT: Sistema Distribuido de Monitoreo Hídrico en Sabana Centro
## Diseño, Validación Experimental en Cisco Packet Tracer (`RA2.3.pkt`) y Guion de Presentación Técnica

---

## 1. Contexto, Motivación y Extensión del Challenge 1

### 1.1 El Reto Hídrico en Sabana Centro
La cuenca alta del río Bogotá y los sistemas de abastecimiento veredal de Sabana Centro enfrentan episodios críticos de estrés hídrico y estiaje severo exacerbados por el fenómeno de El Niño. En el **Challenge 1**, se diseñó y validó una estación hidrometeorológica de borde mononodal autónoma basada en ESP32 DevKit v1, orientada a medir nivel de agua, caudal y variables atmosféricas in-situ con una interfaz de usuario local (display TFT ILI9341 y SPA Web en WLAN local).

### 1.2 Extensión a una Red Distribuida de Nodos (WSN)
Para garantizar una toma de decisiones oportuna por parte de los Comités Municipales de Gestión del Riesgo (CMGRD) y las Juntas de Acción Comunal (JAC), una estación aislada resulta insuficiente. La **Actividad de Refuerzo #2.3** trasciende el diseño mononodal y lo extiende a una **Red Inalámbrica de Sensores (*Wireless Sensor Network - WSN*) distribuida**, donde múltiples nodos replican la sensórica en puntos críticos del acueducto veredal (microcuenca, tanque de almacenamiento y estación meteorológica), canalizando los datos a través de un gateway de borde hacia un servidor central en la nube mediante **MQTT**.

---

## 2. Justificación de Tecnologías y Protocolos de Red

### 2.1 Comparativa de Tecnologías de Red (PAN vs. WLAN vs. LPWAN)

| Criterio | WPAN (IEEE 802.15.4 / Zigbee) | WLAN (Wi-Fi 802.11 b/g/n) | LPWAN (LoRaWAN / NB-IoT) |
| :--- | :--- | :--- | :--- |
| **Alcance Típico** | 10 – 100 m | 50 – 150 m (hasta 500 m con antena sectorial) | 2 – 15 km |
| **Ancho de Banda** | 250 kbps | 11 – 150 Mbps | 0.3 – 50 kbps |
| **Consumo Energético** | Ultra bajo ($<10$ mA en TX) | Medio-Alto (mitigado con *Deep Sleep*) | Muy bajo ($<25$ mA en TX) |
| **Infraestructura de Borde** | Requiere coordinador Zigbee dedicado + puente serie | **Totalmente integrado en Home Gateway estándar** | Requiere Gateway concentrador LoRa multicanal |
| **Costo Adicional** | $15 – $25 USD por transceptor | **$0 USD extra (integrado en el SoC ESP32)** | $120 – $250 USD por gateway |
| **Soporte en Packet Tracer** | Nulo o experimental | **Nativo, robusto y verificable al 100%** | No soportado nativamente |

> **Decisión de Ingeniería**:
> Para el radio de cobertura del acueducto veredal (la caseta de bombeo, el tanque de almacenamiento y la microcuenca adyacente se ubican a menos de 100 metros del concentrador), **Wi-Fi 802.11 b/g/n a 2.4 GHz** es la tecnología óptima. Ofrece despliegue de campo con cero cables, costo marginal de hardware nulo y compatibilidad directa con el gateway simulado.

### 2.2 Protocolo de Capa de Aplicación: Adopción de MQTT

1. **Eficiencia de Ancho de Banda y Batería**: La cabecera fija de MQTT es de tan solo **2 bytes**. En comparación con peticiones HTTP repetitivas, reduce el volumen de datos en más de un 90%, minimizando el tiempo de radio encendido (*duty cycle*) de los nodos en campo.
2. **Arquitectura Publicador/Suscriptor Desacoplada**: Los nodos sensores envían sus datos al Broker sin requerir conocimiento de la dirección IP de los clientes finales ni depender de que el dashboard esté activo.
3. **Calidad de Servicio (QoS) Diferenciada**:
   - **QoS 0 (*At most once*)**: Para telemetría continua de rutina (nivel periódico, temperatura y presión cada 30 segundos).
   - **QoS 1 (*At least once*)**: Para eventos de alerta crítica (desabastecimiento inminente, desbordamiento o falla de flujo), asegurando confirmación obligatoria del Broker mediante `PUBACK`.
4. **Last Will and Testament (LWT)**: Permite notificar a los operadores si un nodo de campo se desconecta inesperadamente por corte de energía o daño físico, publicando un mensaje póstumo `{"status": "OFFLINE"}`.

### 2.3 Esquema de Tópicos MQTT y Estructura de Payloads JSON

Para optimizar el tráfico y seguir la premisa de **mínima sobrecarga**, se adopta un **esquema jerárquico estructurado por municipio/estación**, consolidando todas las variables en un objeto JSON compacto:

| Estación / Nodo | Tópico MQTT | Frecuencia / QoS | Payload JSON de Ejemplo |
| :--- | :--- | :--- | :--- |
| **Nodo Chía (Bocatoma Río Frío)** | `sabanacentro/chia/data` | 30 s / QoS 0 | `{"node":"Nodo_Chia","level_m":2.45,"flow_lmin":18.2,"status":"NORMAL"}` |
| **Nodo Cajicá (Reservorio Canelón)** | `sabanacentro/cajica/data` | 30 s / QoS 0 | `{"node":"Nodo_Cajica","level_m":3.10,"vol_pct":74.2,"flow_lmin":12.5}` |
| **Nodo Zipaquirá (Agro-Climatología)** | `sabanacentro/zipaquira/data` | 60 s / QoS 0 | `{"node":"Nodo_Zipaquira","temp_c":14.8,"press_hpa":748.0,"hr_pct":71,"eto_mm":3.6}` |
| **Canal Unificado de Alertas** | `sabanacentro/+/alerta` | Evento / QoS 1 | `{"ts":1773446400,"node":"Nodo_Chia","code":"ESTIAJE_CRITICO","val":0.95}` |

---

## 3. Implementación y Validación en Cisco Packet Tracer (`RA2.3.pkt`)

### 3.1 Arquitectura de Simulación y Despliegue Físico

Para lograr una validación reproducible y realista del ecosistema IoT:
- **Red de Sensores de Campo (LAN Inalámbrica)**: **Cero cables en campo**. Los 3 nodos sensores (`Nodo_Chia`, `Nodo_Cajica`, `Nodo_Zipaquira`) se asocian por radiofrecuencia Wi-Fi 2.4 GHz al SSID `SabanaCentro_IoT` proyectado por el gateway.
- **Gateway de Borde Consolidado (`GW_SabanaCentro`)**: Dispositivo `Home Gateway` (modelo `DLC-100`) que actúa como Punto de Acceso (AP) 802.11 b/g/n en su interfaz LAN (`192.168.25.1/24`) y como router de frontera con **traducción de direcciones NAT/PAT** en su puerto WAN `Internet` (`203.0.113.2/24`).
- **Enlace de Transporte WAN a Switch de Nube**: Un cable UTP directo (*Copper Straight-Through*) interconecta el puerto `Internet` del gateway con el puerto `FastEthernet0/1` del switch `SW_Nube` (Cisco Catalyst 2960).
- **Segmento Cloud / Plataforma IoT (Subred WAN `203.0.113.0/24`)**:
  - `Broker_MQTT` (Laptop-PT, IP `203.0.113.10`): Ejecuta el Broker MQTT v3.1.1 (puerto TCP 1883) instalado desde el *User Apps Manager*.
  - `Servidor_Web` (Server-PT, IP `203.0.113.20`): Aloja el Dashboard HTTP centralizado (puerto TCP 80) en su archivo `index.html`.
  - `Laptop_Observador` (Laptop-PT, IP `203.0.113.30`): Terminal del operador (JAC / CMGRD) conectada al switch, con navegador web y app cliente MQTT suscriptora.

### 3.2 Matriz Integral de Direccionamiento IP y Servicios

| Dispositivo | Interfaz | Modo | Dirección IP | Máscara | Gateway por Defecto | Servicios / Rol |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **GW_SabanaCentro** | `Wireless` | Estática | `192.168.25.1` | `255.255.255.0` | N/A | SSID: `SabanaCentro_IoT`, WPA2-PSK AES |
| **GW_SabanaCentro** | `Internet` | Estática | `203.0.113.2` | `255.255.255.0` | `203.0.113.1` | NAT/PAT Dinámico Habilitado hacia WAN |
| **SW_Nube** | `Vlan1` | N/A | Sin IP / Capa 2 | `255.255.255.0` | `203.0.113.1` | Switch 2960 de interconexión WAN |
| **Broker_MQTT** | `FastEthernet0` | Estática | `203.0.113.10` | `255.255.255.0` | `203.0.113.1` | Broker MQTT v3.1.1 (TCP 1883) |
| **Servidor_Web** | `FastEthernet0` | Estática | `203.0.113.20` | `255.255.255.0` | `203.0.113.1` | Servidor HTTP Dashboard (TCP 80) |
| **Laptop_Observador** | `FastEthernet0` | Estática | `203.0.113.30` | `255.255.255.0` | `203.0.113.1` | Navegador HTTP + Suscriptor MQTT Client |
| **Nodo_Chia** | `Wireless0` | Estática | `192.168.25.101` | `255.255.255.0` | `192.168.25.1` | Publicador MQTT (`sabanacentro/chia/data`) |
| **Nodo_Cajica** | `Wireless0` | Estática | `192.168.25.102` | `255.255.255.0` | `192.168.25.1` | Publicador MQTT (`sabanacentro/cajica/data`) |
| **Nodo_Zipaquira** | `Wireless0` | Estática | `192.168.25.103` | `255.255.255.0` | `192.168.25.1` | Publicador MQTT (`sabanacentro/zipaquira/data`) |

### 3.3 Guía de Montaje y Verificación Paso a Paso de `RA2.3.pkt`

1. **Apertura de la Topología**:
   * Abrir Cisco Packet Tracer y cargar [`RA2.3.pkt`].
2. **Verificación del Cableado Físico y Estado de Enlaces**:
   * Confirmar que el cable directo UTP conecte el puerto **Internet** de `GW_SabanaCentro` al puerto `FastEthernet0/1` de `SW_Nube`.
   * Verificar que `Broker_MQTT` (Fa0/2), `Servidor_Web` (Fa0/3) y `Laptop_Observador` (Fa0/4) tengan enlaces verdes activos con `SW_Nube`.
   * Confirmar que los tres nodos sensores (`Nodo_Chia`, `Nodo_Cajica`, `Nodo_Zipaquira`) muestren líneas punteadas de asociación inalámbrica con `GW_SabanaCentro`.
3. **Despliegue del Dashboard Web en el Servidor**:
   * En `Servidor_Web` -> pestaña **Services** -> **HTTP**.
   * Verificar que el servicio HTTP esté en **ON** y que el archivo `index.html` contenga el código del Dashboard de Sabana Centro provisto en [`dashboard_index.html`].
4. **Arranque del Broker MQTT**:
   * En `Broker_MQTT` -> pestaña **Desktop** -> **User Apps Manager** (o directamente el icono **MQTT Broker**).
   * Confirmar que el broker esté activo escuchando en el puerto estándar 1883.
5. **Verificación de Conectividad Capa 3 (Pruebas de Ping)**:
   * **Prueba 5.1 (LAN de Nodos al Gateway)**: Desde el Command Prompt de `Nodo_Chia`:
     ```cmd
     ping 192.168.25.1
     ```
     *Resultado*: 4/4 paquetes recibidos, confirmando enlace Wi-Fi local y capa 3 LAN.
   * **Prueba 5.2 (Nodo atravesando NAT hacia el Broker Cloud)**: Desde el Command Prompt de `Nodo_Chia`:
     ```cmd
     ping 203.0.113.10
     ```
     *Resultado*: 4/4 paquetes recibidos (0% de pérdida), confirmando que el gateway traduce y enruta tráfico LAN $\rightarrow$ WAN vía NAT/PAT. Repetir desde `Nodo_Cajica` y `Nodo_Zipaquira`.
   * **Prueba 5.3 (Interconexión Cloud)**: Desde `Laptop_Observador` en Command Prompt:
     ```cmd
     ping 203.0.113.10
     ping 203.0.113.20
     ```
     *Resultado*: Respuestas exitosas con $RTT < 1\text{ ms}$, validando la subred WAN `203.0.113.0/24`.
   * **Prueba 5.4 (Aislamiento de Seguridad WAN $\rightarrow$ LAN)**: Desde `Laptop_Observador` hacia un nodo interno:
     ```cmd
     ping 192.168.25.101
     ```
     *Resultado esperado*: **Request timed out (100% pérdida)**. Demuestra formalmente que el gateway oculta la topología privada de campo y bloquea conexiones entrantes no solicitadas desde Internet.
6. **Publicación y Suscripción MQTT en Vivo**:
   * En `Laptop_Observador` -> **Desktop** -> **MQTT Client**:
     * Broker Address: `203.0.113.10` -> Clic en **Connect**.
     * Pestaña **Subscribe**: Ingresar comodín `sabanacentro/#` (o `sabanacentro/+/data`) -> Clic en **Subscribe**.
   * En cada nodo sensor (`Nodo_Chia`, `Nodo_Cajica`, `Nodo_Zipaquira`) -> **Desktop** -> **MQTT Client**:
     * Broker Address: `203.0.113.10` -> Clic en **Connect**.
     * Ingresar el tópico municipal correspondiente (`sabanacentro/chia/data`, etc.), el payload JSON y QoS 0 -> Clic en **Publish**.
   * *Resultado en Observador*: Los mensajes de telemetría de los 3 municipios se reciben en tiempo real en la consola del cliente MQTT de la laptop.
7. **Acceso al Dashboard HTTP Centralizado**:
   * En `Laptop_Observador` -> **Desktop** -> **Web Browser**.
   * URL: `http://203.0.113.20` -> Presionar Enter.
   * *Resultado*: Despliegue visual del panel analítico oscuro con las 3 tarjetas de las subestaciones de Chía, Cajicá y Zipaquirá.
8. **Inspección en Modo Simulación (*Simulation Mode*)**:
   * Cambiar a modo **Simulation**, filtrar eventos por `ICMP`, `HTTP` y `TCP`.
   * Lanzar una solicitud web o ping desde un nodo hacia la nube y avanzar paso a paso (*Capture/Forward*).
   * Inspeccionar la PDU en el Gateway: evidenciar cómo la dirección IP origen cambia de `192.168.25.101` a la IP WAN pública `203.0.113.2`, validando el mecanismo PAT (Port Address Translation) en la interfaz de salida.

### 3.4 Troubleshooting: Retos Enfrentados y Soluciones de Ingeniería

Durante la implementación y validación experimental en Cisco Packet Tracer se presentaron y resolvieron cinco retos de ingeniería de red y de software:

#### Reto 1: Ausencia de Servidor DHCP Configurable en Gateway `DLC-100`
* **Síntoma**: Los nodos sensores SBC-PT (`Nodo_Chia`, `Nodo_Cajica`, `Nodo_Zipaquira`) se asociaban físicamente por radiofrecuencia al SSID `SabanaCentro_IoT`, pero reportaban direcciones IP vacías o del rango de autoconfiguración APIPA (`169.254.x.x/16`), provocando la desconexión total de capa 3.
* **Diagnóstico de Causa Raíz**: El dispositivo `Home Gateway` (modelo genérico `DLC-100`) en Packet Tracer cuenta con un microcódigo simplificado cuya pestaña **GUI** está restringida al servidor HTTP propio. Adicionalmente, en **Config $\rightarrow$ LAN** únicamente expone campos estáticos para IPv4 y máscara, sin daemon DHCP activo ni configurable para clientes de campo.
* **Solución de Ingeniería**: Migración a un esquema de **Direccionamiento IP Estático Determinístico** en cada nodo sensor (`192.168.25.101/24`, `.102/24` y `.103/24` con Gateway `192.168.25.1`). Esto garantiza predictibilidad absoluta en la red, elimina colisiones de asignación dinámica y suprime la sobrecarga temporal de negociación DHCP en cada ciclo de inicio.

#### Reto 2: Incompatibilidad de Máscara de Subred WAN (`/30` vs `/24`)
* **Síntoma**: Tiempo de espera agotado (*Request timed out*, 100% de pérdida) en todos los pings emitidos desde los nodos de campo hacia el Broker (`203.0.113.10`) y hacia el Servidor Web (`203.0.113.20`).
* **Diagnóstico de Causa Raíz**: La propuesta teórica inicial consideró un enlace WAN punto a punto `/30` (`255.255.255.252`) en el puerto Internet del gateway (`203.0.113.2`). Con dicha máscara, el único host válido en la subred era `203.0.113.1`. En consecuencia, los paquetes dirigidos a las direcciones `.10` y `.20` eran catalogados como fuera de subred y descartados en el gateway por ausencia de ruta.
* **Solución de Ingeniería**: Reconfiguración de la interfaz Internet del gateway y de los hosts de la nube a máscara `/24` (`255.255.255.0`). Esto permitió integrar el switch `SW_Nube` y alojar simultáneamente el Broker, el Servidor Web y la Laptop Observadora en un segmento WAN coherente y accesible.

#### Reto 3: Selección y Cableado del Puerto Físico en el Gateway
* **Síntoma**: Imposibilidad de enrutamiento o aislamiento nulo entre la red de campo y los servidores públicos.
* **Diagnóstico de Causa Raíz**: El chasis del gateway posee cuatro puertos Ethernet LAN y un puerto independiente etiquetado **Internet**. Conectar el switch de nube a uno de los puertos Ethernet sitúa los servidores en el dominio de difusión de campo, anulando el firewall de borde y la traducción NAT/PAT.
* **Solución de Ingeniería**: Cableado estricto con cable de cobre directo (*Copper Straight-Through*) desde el puerto exclusivo **Internet** del gateway hacia el puerto `FastEthernet0/1` del switch `SW_Nube`, garantizando la frontera de seguridad perimetral requerida por RFC 3022.

#### Reto 4: Limitación de Puertos Ethernet en Placa SBC-PT para el Broker
* **Síntoma**: Al intentar conectar una placa SBC-PT al switch de la nube para alojar el broker, el menú desplegable solo mostraba pines de E/S analógicos y digitales (D0–D9, USB0–1), sin tarjeta de red cableada.
* **Diagnóstico de Causa Raíz**: La placa SBC-PT estándar en el simulador está configurada por omisión como interfaz física de sensores de bajo nivel y carece de puerto RJ-45 FastEthernet en su chasis frontal.
* **Solución de Ingeniería**: Empleo de un equipo `Laptop-PT` dedicado (`Broker_MQTT`) con puerto `FastEthernet0` nativo. En este host se instaló y ejecutó el servicio `MQTT Broker` a través del módulo *User Apps Manager*, garantizando conectividad física directa a 100 Mbps con el switch.

#### Reto 5: Desacoplamiento de Flujos de Datos Web y MQTT en el Simulador
* **Síntoma**: El servidor HTTP de Packet Tracer no soporta de forma nativa la recepción de publicaciones MQTT ni su conversión a WebSockets para visualización HTML dinámica.
* **Solución de Ingeniería**: Separación modular de roles en la plataforma:
  1. **Capa de Telemetría Dinámica**: Demostrada en tiempo real mediante la app nativa `MQTT Client` en la `Laptop_Observador`, suscrita al canal `sabanacentro/#` sobre TCP 1883.
  2. **Capa de Visualización Institucional**: Desplegada por el `Servidor_Web` en el puerto TCP 80 (`http://203.0.113.20`), sirviendo el dashboard centralizado de Sabana Centro como referencia del panel de control para operadores de acueducto.
