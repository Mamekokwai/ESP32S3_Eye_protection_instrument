#ifndef __XL9555_H
#define __XL9555_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "myiic.h"
#include "string.h"


/* 引脚与相关参数定义 */
#define XL9555_INT_IO               GPIO_NUM_46                     /* XL9555_INT引脚 */

/* XL9555寄存器宏 */
#define XL9555_INPUT_PORT0_REG      0                               /* 输入寄存器0地址 */
#define XL9555_INPUT_PORT1_REG      1                               /* 输入寄存器1地址 */
#define XL9555_OUTPUT_PORT0_REG     2                               /* 输出寄存器0地址 */
#define XL9555_OUTPUT_PORT1_REG     3                               /* 输出寄存器1地址 */
#define XL9555_INVERSION_PORT0_REG  4                               /* 极性反转寄存器0地址 */
#define XL9555_INVERSION_PORT1_REG  5                               /* 极性反转寄存器1地址 */
#define XL9555_CONFIG_PORT0_REG     6                               /* 方向配置寄存器0地址 */
#define XL9555_CONFIG_PORT1_REG     7                               /* 方向配置寄存器1地址 */

#define XL9555_ADDR                 0X20

/* XL9555各个IO的功能 */
#define CAMERA_LED_IO                   0x0001
#define IMU_INT1_IO                  0x0002
// #define SPK_EN_IO                   0x0004
#define TF_CS_IO                     0x0008
#define KEY1_IO                  0x0010
#define KEY2_IO                 0x0020
#define KEY3_IO                  0x0040
#define KEY4_IO                  0x0080
#define AP_INT_IO                   0x0100
#define TP_INT                   0x0200
// #define SLCD_RST_IO                 0x0400
// #define SLCD_PWR_IO                 0x0800
// #define KEY3_IO                     0x1000
#define LCD_CS_IO                     0x2000
#define BACKLIGHT_IO                     0x4000
#define MUTE                     0x8000

#define KEY1                        xl9555_pin_read(KEY1_IO)        /* 读取KEY1引脚 */
#define KEY2                        xl9555_pin_read(KEY2_IO)        /* 读取KEY2引脚 */
#define KEY3                        xl9555_pin_read(KEY3_IO)        /* 读取KEY3引脚 */
#define KEY4                        xl9555_pin_read(KEY4_IO)        /* 读取KEY4引脚 */

#define KEY1_PRES                   2                               /* KEY1按下 */
#define KEY2_PRES                   3                               /* KEY2按下 */
#define KEY3_PRES                   4                               /* KEY3按下 */
#define KEY4_PRES                   5                               /* KEY4按下 */

/* 函数声明 */
esp_err_t xl9555_init(void);                                            /* 初始化XL9555 */
int xl9555_pin_read(uint16_t pin);                                      /* 获取某个IO状态 */
uint16_t xl9555_pin_write(uint16_t pin, int val);                       /* 控制某个IO的电平 */
esp_err_t xl9555_read_byte(uint8_t* data, size_t len);                  /* 读取XL9555的IO值 */
esp_err_t xl9555_write_byte(uint8_t reg, uint8_t *data, size_t len);    /* 向XL9555寄存器写入数据 */
uint8_t xl9555_key_scan(uint8_t mode);                                  /* 扫描扩展按键 */
void xl9555_int_init(void);                                             /* 初始化XL9555的中断引脚 */

#endif
