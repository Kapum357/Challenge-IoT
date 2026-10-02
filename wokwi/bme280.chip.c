// Wokwi Custom Chip: Bosch BME280 - Challenge #2
// Módulo sensor de presión, temperatura y humedad BME280
//
// Especificaciones técnicas del módulo:
//   - Voltaje de funcionamiento: 1.71 V a 3.6 V
//   - Interfaz: I2C (0x76 / 0x77), SPI
//   - Consumo de corriente: 3.6 µA
//   - Dimensiones: 1.4 × 1.7 cm
//   - Sensor de humedad:
//       * Rango: 0 a 100 %HR
//       * Precisión: ± 3 % HR
//       * Resolución: 0.008 % HR
//   - Sensor de presión:
//       * Rango: 300 a 1100 hPa
//       * Precisión: ± 1 hPa
//       * Resolución: 0.18 Pa
//   - Sensor de temperatura:
//       * Rango: -40 a 85 °C
//       * Precisión: ± 1 °C
//       * Resolución: 0.1 °C
//
// Serie temporal acelerada: comprime 72h de datos meteorológicos de Sabana Centro
// en N minutos de simulación (factor configurable vía atributo "accel").
//
// Perfil climatológico Sabana Centro (2600 msnm):
//   Temperatura: 6-26°C ciclo sinusoidal diurno/nocturno
//   Humedad: inversa a temperatura (45-90%)
//   Presión: ~750 hPa ± variación barométrica leve
//   Eventos: "ola de calor" con picos de hasta 30°C y HR < 40%

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
  uint32_t addr_attr;
  uint32_t temp_attr;
  uint32_t press_attr;
  uint32_t hum_attr;
  uint32_t accel_attr;    // Factor de aceleración temporal (default: 1440 = 1 día/min)
  uint32_t mode_attr;     // 0=manual (sliders), 1=auto (serie temporal acelerada)
  pin_t sdo_pin;
  uint8_t reg_addr;
  bool is_first_byte;
  uint8_t registers[256];
  uint32_t start_time_ms;  // Timestamp de inicio de simulación
} chip_state_t;

// Perfil climatológico: calcula valores meteorológicos para un momento dado
// sim_hours: horas transcurridas en tiempo simulado (0 a 72+)
static void compute_climate_profile(float sim_hours, float *temp_c, float *hum_pct, float *press_hpa) {
    // Hora del día (0-24)
    float hour_of_day = fmodf(sim_hours, 24.0f);
    // Día de simulación (0, 1, 2...)
    int day = (int)(sim_hours / 24.0f);

    // Temperatura base: sinusoidal 6°C (amanecer 6am) a 26°C (pico 14:00)
    // T(h) = T_mean + T_amp * sin(2π(h - 8)/24)
    // Mínima a las 5-6am, máxima a las 13-14h
    float t_mean = 16.0f;  // Media Sabana Centro
    float t_amp = 10.0f;   // Amplitud térmica diaria
    float t_phase = (hour_of_day - 8.0f) / 24.0f * 2.0f * M_PI;
    float base_temp = t_mean + t_amp * sinf(t_phase);

    // Evento de "ola de calor" en el día 1 (horas 24-48): +4°C extra
    if (day == 1) {
        float heat_factor = 4.0f * sinf((hour_of_day - 8.0f) / 24.0f * 2.0f * M_PI);
        if (heat_factor > 0) base_temp += heat_factor;
    }

    // Evento "noche fría" en día 2 (horas 48-72): -3°C en la madrugada
    if (day == 2 && hour_of_day < 7.0f) {
        base_temp -= 3.0f;
    }

    // Clamp temperatura
    if (base_temp < 2.0f) base_temp = 2.0f;
    if (base_temp > 32.0f) base_temp = 32.0f;

    // Humedad relativa: inversamente correlacionada con temperatura
    // HR alta de noche (85-90%), baja con calor (40-55%)
    float hr_base = 90.0f - (base_temp - 6.0f) * (50.0f / 26.0f);
    // Variación estocástica leve usando seno con frecuencia diferente
    hr_base += 3.0f * sinf(sim_hours * 0.7f);
    if (hr_base < 35.0f) hr_base = 35.0f;
    if (hr_base > 95.0f) hr_base = 95.0f;

    // Presión atmosférica: ~750 hPa (2600 msnm) con variación pequeña
    float press_base = 750.0f + 2.0f * sinf(sim_hours * 0.3f) + 1.5f * sinf(sim_hours * 0.07f);

    *temp_c = base_temp;
    *hum_pct = hr_base;
    *press_hpa = press_base;
}

static void update_sensor_data(chip_state_t *chip) {
    float temp_c, press_hpa, hum_pct;

    int mode = (int)attr_read_float(chip->mode_attr);

    if (mode == 1) {
        // Modo automático: serie temporal acelerada
        float accel = attr_read_float(chip->accel_attr);
        if (accel < 1.0f) accel = 1440.0f; // Default: 1 día/min

        // Calcular horas simuladas transcurridas
        uint32_t elapsed_ms = get_sim_nanos() / 1000000UL - chip->start_time_ms;
        float elapsed_hours = (elapsed_ms / 1000.0f / 3600.0f) * accel;

        compute_climate_profile(elapsed_hours, &temp_c, &hum_pct, &press_hpa);
    } else {
        // Modo manual: leer valores de los sliders
        temp_c = attr_read_float(chip->temp_attr);
        if (temp_c < -40.0f || temp_c > 85.0f) temp_c = 20.0f;

        press_hpa = attr_read_float(chip->press_attr);
        if (press_hpa < 300.0f || press_hpa > 1100.0f) press_hpa = 1013.25f;

        hum_pct = attr_read_float(chip->hum_attr);
        if (hum_pct < 0.0f || hum_pct > 100.0f) hum_pct = 65.0f;
    }

    // Codificar valores crudos para registros BME280
    uint32_t raw_temp = (uint32_t)(503680.0f + (temp_c - 20.0f) * 3192.0f);
    uint32_t raw_press = (uint32_t)(406600.0f - (press_hpa - 1013.25f) * 587.0f);
    uint32_t raw_hum = (uint32_t)(19250.0f + hum_pct * 182.5f);

    // Burst Registers 0xF7..0xFE
    chip->registers[0xF7] = (raw_press >> 12) & 0xFF;
    chip->registers[0xF8] = (raw_press >> 4) & 0xFF;
    chip->registers[0xF9] = (raw_press << 4) & 0xF0;
    chip->registers[0xFA] = (raw_temp >> 12) & 0xFF;
    chip->registers[0xFB] = (raw_temp >> 4) & 0xFF;
    chip->registers[0xFC] = (raw_temp << 4) & 0xF0;
    chip->registers[0xFD] = (raw_hum >> 8) & 0xFF;
    chip->registers[0xFE] = raw_hum & 0xFF;
}

static bool on_i2c_connect(void *user_data, uint32_t address, bool read) {
  chip_state_t *chip = (chip_state_t *)user_data;
  chip->is_first_byte = true;
  update_sensor_data(chip);
  return true;
}

static uint8_t on_i2c_read(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  update_sensor_data(chip);
  uint8_t value = chip->registers[chip->reg_addr];
  chip->reg_addr = (chip->reg_addr + 1) & 0xFF;
  return value;
}

static bool on_i2c_write(void *user_data, uint8_t data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  if (chip->is_first_byte) {
    chip->reg_addr = data;
    chip->is_first_byte = false;
  } else {
    chip->registers[chip->reg_addr] = data;
    chip->reg_addr = (chip->reg_addr + 1) & 0xFF;
  }
  return true;
}

void chip_init(void) {
  chip_state_t *chip = (chip_state_t *)calloc(1, sizeof(chip_state_t));
  chip->addr_attr = attr_init_float("address", 118.0f);
  chip->temp_attr = attr_init_float("temp", 20.0f);
  chip->press_attr = attr_init_float("pressure", 750.0f);  // 750 hPa para Sabana Centro
  chip->hum_attr = attr_init_float("humidity", 65.0f);
  chip->accel_attr = attr_init_float("accel", 1440.0f);    // 1 día/min por defecto
  chip->mode_attr = attr_init_float("mode", 0.0f);          // 0=manual, 1=auto
  chip->sdo_pin = pin_init("SDO", INPUT);
  chip->start_time_ms = get_sim_nanos() / 1000000UL;

  // Registros de calibración (idénticos al Ch1)
  chip->registers[0x88] = 0x70; chip->registers[0x89] = 0x6B;
  chip->registers[0x8A] = 0x43; chip->registers[0x8B] = 0x67;
  chip->registers[0x8C] = 0x18; chip->registers[0x8D] = 0xFC;
  chip->registers[0x8E] = 0x7D; chip->registers[0x8F] = 0x8E;
  chip->registers[0x90] = 0x43; chip->registers[0x91] = 0xD6;
  chip->registers[0x92] = 0xD0; chip->registers[0x93] = 0x0B;
  chip->registers[0x94] = 0x27; chip->registers[0x95] = 0x0B;
  chip->registers[0x96] = 0x8C; chip->registers[0x97] = 0x00;
  chip->registers[0x98] = 0xF9; chip->registers[0x99] = 0xFF;
  chip->registers[0x9A] = 0x8C; chip->registers[0x9B] = 0x3C;
  chip->registers[0x9C] = 0xF8; chip->registers[0x9D] = 0xC6;
  chip->registers[0x9E] = 0x70; chip->registers[0x9F] = 0x17;
  chip->registers[0xA1] = 0x4B;
  chip->registers[0xE1] = 0x68; chip->registers[0xE2] = 0x01;
  chip->registers[0xE3] = 0x00;
  chip->registers[0xE4] = 0x12; chip->registers[0xE5] = 0x0C;
  chip->registers[0xE6] = 0x03;
  chip->registers[0xE7] = 0x1E;

  // Chip ID BME280
  chip->registers[0xD0] = 0x60;

  // Control registers
  chip->registers[0xF2] = 0x01;
  chip->registers[0xF3] = 0x00;
  chip->registers[0xF4] = 0x27;
  chip->registers[0xF5] = 0x00;

  update_sensor_data(chip);

  uint32_t i2c_addr = 0x76;
  if (pin_read(chip->sdo_pin) == HIGH || attr_read_float(chip->addr_attr) >= 119.0f) {
    i2c_addr = 0x77;
  }

  const i2c_config_t i2c_config = {
    .user_data = chip,
    .address = i2c_addr,
    .scl = pin_init("SCK", INPUT),
    .sda = pin_init("SDI", INPUT),
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
  };
  i2c_init(&i2c_config);
}
