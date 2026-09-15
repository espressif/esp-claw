/*
 * board_defs.h — Arduino UNO (ATmega328P) Board Definitions
 *
 * Equivalent to ESP-Claw's board_info.yaml / board_peripherals.yaml
 * for the Arduino UNO platform.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* ───────────────────── Board Identity ───────────────────── */
#define BOARD_NAME          "arduino_uno"
#define BOARD_CHIP          "ATmega328P"
#define BOARD_MANUFACTURER  "Arduino"
#define BOARD_DESCRIPTION   "Arduino UNO R3"

/* ───────────────────── Clock ───────────────────── */
#define BOARD_CPU_FREQ_HZ   16000000UL

/* ───────────────────── Memory ───────────────────── */
#define BOARD_SRAM_BYTES    2048
#define BOARD_FLASH_BYTES   32768
#define BOARD_EEPROM_BYTES  1024

/* ───────────────────── Serial ───────────────────── */
#define BOARD_SERIAL_BAUD       115200
#define BOARD_SERIAL_RX_BUF     128    /* Max incoming command length */
#define BOARD_SERIAL_TX_BUF     128    /* Max outgoing response length */

/* ───────────────────── Pin Counts ───────────────────── */
#define BOARD_DIGITAL_PINS  14   /* D0–D13 (D0/D1 = Serial TX/RX) */
#define BOARD_ANALOG_PINS   6    /* A0–A5 */
#define BOARD_TOTAL_PINS    20   /* D0–D13 + A0–A5 */

/* ───────────────────── PWM Capable Pins ───────────────────── */
/* Arduino UNO PWM: D3, D5, D6, D9, D10, D11 (8-bit, 0–255) */
#define BOARD_PWM_PIN_COUNT 6
static const uint8_t BOARD_PWM_PINS[] = { 3, 5, 6, 9, 10, 11 };

/* ───────────────────── ADC Capable Pins ───────────────────── */
/* A0–A5 map to digital pins 14–19 */
#define BOARD_ADC_PIN_COUNT 6
#define BOARD_ADC_RESOLUTION 10  /* 10-bit: 0–1023 */
static const uint8_t BOARD_ADC_PINS[] = { A0, A1, A2, A3, A4, A5 };

/* ───────────────────── I2C ───────────────────── */
#define BOARD_I2C_SDA_PIN   A4   /* Pin 18 */
#define BOARD_I2C_SCL_PIN   A5   /* Pin 19 */

/* ───────────────────── SPI ───────────────────── */
#define BOARD_SPI_MOSI_PIN  11
#define BOARD_SPI_MISO_PIN  12
#define BOARD_SPI_SCK_PIN   13
#define BOARD_SPI_SS_PIN    10

/* ───────────────────── Built-in LED ───────────────────── */
#define BOARD_LED_BUILTIN   13

/* ───────────────────── Reserved Pins ───────────────────── */
/* D0 (RX) and D1 (TX) are used by Serial — avoid user GPIO on these */
#define BOARD_SERIAL_RX_PIN 0
#define BOARD_SERIAL_TX_PIN 1

/* ───────────────────── Pin Capability Helpers ───────────────────── */

/**
 * Check if a digital pin number supports PWM output.
 */
static inline bool board_pin_has_pwm(uint8_t pin)
{
    switch (pin) {
        case 3: case 5: case 6: case 9: case 10: case 11:
            return true;
        default:
            return false;
    }
}

/**
 * Check if a pin number is a valid analog input (A0–A5 or 14–19).
 */
static inline bool board_pin_has_adc(uint8_t pin)
{
    return (pin >= A0 && pin <= A5);
}

/**
 * Check if a digital pin is safe for user GPIO (not Serial TX/RX).
 */
static inline bool board_pin_is_user_safe(uint8_t pin)
{
    return (pin >= 2 && pin <= 13) || (pin >= A0 && pin <= A5);
}

/* ───────────────────── Servo Limits ───────────────────── */
#define BOARD_SERVO_MAX     4    /* Max simultaneous servos (RAM-limited) */
#define BOARD_SERVO_MIN_US  544
#define BOARD_SERVO_MAX_US  2400
