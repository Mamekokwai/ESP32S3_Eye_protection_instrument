#ifndef __LED_H
#define __LED_H

#include "driver/gpio.h"


/* 引脚定义: GPIO2 实际是功放 MUTE/使能 (V1.4), 不是心跳 LED; 勿作普通 LED 用 */
#define LED0_GPIO_PIN    GPIO_NUM_2

/* LED0端口定义 */
#define LED0(x)          do { x ?                                \
                              gpio_set_level(LED0_GPIO_PIN, 1):  \
                              gpio_set_level(LED0_GPIO_PIN, 0);  \
                            } while(0)  /* LED0翻转 */

/* LED取反定义 */
#define LED0_TOGGLE()    do { gpio_set_level(LED0_GPIO_PIN, !gpio_get_level(LED0_GPIO_PIN)); } while(0)  /* LED0翻转 */

/* 函数声明*/
void led_init(void);     /* 初始化LED */

#endif
