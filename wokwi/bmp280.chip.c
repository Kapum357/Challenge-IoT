// Wokwi Custom Chip: Bosch BMP280 Digital Pressure and Temperature Sensor
// Custom Chip by Luis Miguel Capacho Valbuena for Wokwi Simulation

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

typedef struct {
  uint32_t temp_attr;
  uint8_t reg_addr;
  bool is_first_byte;
  uint8_t registers[256];
} chip_state_t;

static void update_sensor_data(chip_state_t *chip) {
  // Read temperature attribute from Wokwi slider (default ~519000 corresponds to ~20.0 C)
  float raw_temp_float = attr_read_float(chip->temp_attr);
  uint32_t raw_temp = (uint32_t)raw_temp_float;
  if (raw_temp == 0) {
    raw_temp = 519000; // default ~20.0 C
  }

  // Default pressure ~415000 corresponds to ~1013.25 hPa
  uint32_t raw_press = 415000;

  // Update Burst Registers 0xF7..0xFC
  chip->registers[0xF7] = (raw_press >> 12) & 0xFF; // Press MSB
  chip->registers[0xF8] = (raw_press >> 4) & 0xFF;  // Press LSB
  chip->registers[0xF9] = (raw_press << 4) & 0xF0;  // Press XLSB

  chip->registers[0xFA] = (raw_temp >> 12) & 0xFF;  // Temp MSB
  chip->registers[0xFB] = (raw_temp >> 4) & 0xFF;   // Temp LSB
  chip->registers[0xFC] = (raw_temp << 4) & 0xF0;   // Temp XLSB
}

static bool on_i2c_connect(void *user_data, uint32_t address, bool read) {
  chip_state_t *chip = (chip_state_t *)user_data;
  chip->is_first_byte = true;
  update_sensor_data(chip);
  return true; // ACK
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
  return true; // ACK
}

void chip_init(void) {
  chip_state_t *chip = (chip_state_t *)calloc(1, sizeof(chip_state_t));
  chip->temp_attr = attr_init_float("temp", 519000.0f);

  // Initialize BMP280 Calibration Registers (dig_T1..T3, dig_P1..P9)
  // dig_T1 = 27504 (0x6B70)
  chip->registers[0x88] = 0x70; chip->registers[0x89] = 0x6B;
  // dig_T2 = 26435 (0x6743)
  chip->registers[0x8A] = 0x43; chip->registers[0x8B] = 0x67;
  // dig_T3 = -1000 (0xFC18)
  chip->registers[0x8C] = 0x18; chip->registers[0x8D] = 0xFC;
  // dig_P1 = 36477 (0x8E7D)
  chip->registers[0x8E] = 0x7D; chip->registers[0x8F] = 0x8E;
  // dig_P2 = -10685 (0xD643)
  chip->registers[0x90] = 0x43; chip->registers[0x91] = 0xD6;
  // dig_P3 = 3024 (0x0BD0)
  chip->registers[0x92] = 0xD0; chip->registers[0x93] = 0x0B;
  // dig_P4 = 2855 (0x0B27)
  chip->registers[0x94] = 0x27; chip->registers[0x95] = 0x0B;
  // dig_P5 = 140 (0x008C)
  chip->registers[0x96] = 0x8C; chip->registers[0x97] = 0x00;
  // dig_P6 = -7 (0xFFF9)
  chip->registers[0x98] = 0xF9; chip->registers[0x99] = 0xFF;
  // dig_P7 = 15500 (0x3C8C)
  chip->registers[0x9A] = 0x8C; chip->registers[0x9B] = 0x3C;
  // dig_P8 = -14600 (0xC6F8)
  chip->registers[0x9C] = 0xF8; chip->registers[0x9D] = 0xC6;
  // dig_P9 = 6000 (0x1770)
  chip->registers[0x9E] = 0x70; chip->registers[0x9F] = 0x17;

  // Chip ID: BMP280 = 0x58
  chip->registers[0xD0] = 0x58;

  // Status & Control default values
  chip->registers[0xF3] = 0x00;
  chip->registers[0xF4] = 0x27;
  chip->registers[0xF5] = 0x00;

  update_sensor_data(chip);

  // Configure I2C Slave on address 0x76 using pins SCK (SCL) and SDI (SDA)
  const i2c_config_t i2c_config = {
    .user_data = chip,
    .address = 0x76,
    .scl = pin_init("SCK", INPUT),
    .sda = pin_init("SDI", INPUT),
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
  };
  i2c_init(&i2c_config);
}
