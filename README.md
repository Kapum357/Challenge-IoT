# Challenge-1
Sistema IoT Integrado de Borde para el Monitoreo Hidrometeorológico Local y Alerta Temprana de Desabastecimiento Hídrico en Sabana Centro

## Visión del proyecto
El nodo de borde está pensado como una solución local y ultra económica para detectar riesgo hídrico en reservorios de la Sabana Centro. El diseño prioriza la robustez del software y la compensación matemática frente a sensores de bajo costo, con la finalidad de ofrecer una alerta temprana útil sin depender de conectividad celular ni de infraestructura en la nube.

## Estructura del repositorio
- `src/`: lógica portabilizada para ESP32 y validación de algoritmos
- `wokwi/diagram.json`: configuración base para simulación Wokwi

## Cómo empezar
1. Explorar la lógica del firmware en `src/`.
2. Importar `wokwi/diagram.json` en Wokwi para construir una prueba virtual del hardware.
3. Usar la arquitectura propuesta como base para calibración y despliegue real.

## Validación local
- El módulo `firmware/WaterMonitorCore.cpp` se valida con una pequeña suite de pruebas C++.
- Para ejecutar la validación local:
  - `cd firmware`
  - `make run`

## Componentes principales
- ESP32 DevKit v1 como unidad central
- HC-SR04 para medición de nivel
- Sensor Hall para medición de caudal
- BME280 para clima local
- Display LCD 1602 + LED RGB + buzzer para alerta in situ

## Estado actual
La base de la documentación y la lógica central del firmware ya quedaron organizadas en una estructura reproducible para continuar con la validación experimental y la puesta en marcha del proyecto.
