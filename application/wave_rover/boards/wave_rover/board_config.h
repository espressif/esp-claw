#pragma once
/* WAVE ROVER GPIO and hardware constants (confirmed from ugv_base_general) */

/* Motor A (left) */
#define WR_MOTOR_PWMA    25
#define WR_MOTOR_AIN1    21
#define WR_MOTOR_AIN2    17
#define WR_LEDC_CH_A      5

/* Motor B (right) */
#define WR_MOTOR_PWMB    26
#define WR_MOTOR_BIN1    22
#define WR_MOTOR_BIN2    23
#define WR_LEDC_CH_B      6

/* LEDC config */
#define WR_LEDC_FREQ_HZ  100000
#define WR_LEDC_BITS     LEDC_TIMER_8_BIT   /* 0-255 */
#define WR_LEDC_TIMER    LEDC_TIMER_0
#define WR_LEDC_SPEED    LEDC_LOW_SPEED_MODE

/* I2C bus */
#define WR_I2C_PORT      I2C_NUM_0
#define WR_I2C_SDA       32
#define WR_I2C_SCL       33
#define WR_I2C_FREQ_HZ   100000

/* INA219 */
#define WR_INA219_ADDR   0x42

/* OLED SSD1306 */
#define WR_OLED_ADDR     0x3C
#define WR_OLED_WIDTH    128
#define WR_OLED_HEIGHT    32

/* IMU */
#define WR_QMI8658_ADDR  0x6B
#define WR_AK09918_ADDR  0x0C

/* Safety */
#define WR_LOW_BATT_V    10.5f  /* 3S LiPo approx 3.5V/cell */
