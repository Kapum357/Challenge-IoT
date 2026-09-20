"""
Simulador de Nodo Sensor IoT para Cisco Packet Tracer (MCU-PT / SBC-PT)
Actividad de Refuerzo #2.3 — Conectividad en IoT
Universidad de La Sabana - 2026

Este script puede cargarse en la pestaña 'Programming' de una placa MCU o SBC
en Cisco Packet Tracer para simular la publicación periódica de telemetría MQTT.
"""

from time import *
import json

# Constantes de Red y Broker
BROKER_IP = "203.0.113.1"
BROKER_PORT = 1883
NODE_ID = "Nodo_Microcuenca"
TOPIC_DATA = "sabanacentro/chia/microcuenca/data"
TOPIC_ALERT = "sabanacentro/chia/sistema/alerta"

def get_simulated_readings():
    # Simula lecturas de sensor ultrasonico JSN-SR04T y caudal YF-S201
    return {
        "ts": 1773446400,
        "node": NODE_ID,
        "level_m": 2.45,
        "flow_lmin": 18.2,
        "status": "NORMAL"
    }

def main():
    print("[INIT] Inicializando nodo sensor:", NODE_ID)
    print("[NET] Esperando concesión de DHCP...")
    sleep(2)
    print("[NET] Conectado a WLAN SSID 'SabanaCentro_IoT'")
    print("[MQTT] Conectando a Broker en", BROKER_IP, ":", BROKER_PORT)
    sleep(1)
    print("[MQTT] Conexión establecida exitosamente (Keep-Alive: 60s)")
    
    ciclo = 1
    while True:
        payload = get_simulated_readings()
        payload_str = json.dumps(payload)
        
        # Publicacion periodica QoS 0
        print("[PUB] Ciclo #" + str(ciclo) + " -> Topico: " + TOPIC_DATA + " [QoS 0]")
        print("      Payload: " + payload_str)
        
        # Evaluacion de condicion de alerta
        if payload["level_m"] < 1.0:
            alerta = {"ts": payload["ts"], "node": NODE_ID, "code": "ESTIAJE_CRITICO", "val": payload["level_m"]}
            print("[ALERT] Publicando evento crítico en " + TOPIC_ALERT + " [QoS 1]")
            print("        " + json.dumps(alerta))
            
        ciclo += 1
        sleep(30) # Envio cada 30 segundos

if __name__ == "__main__":
    main()
