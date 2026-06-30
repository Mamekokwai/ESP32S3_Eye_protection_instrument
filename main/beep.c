#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define BEEP_GPIO GPIO_NUM_46
#define BEEP_FREQ 2000 // 蜂鸣器频率，常用 2k~4kHz
#define LEDC_CH 0      // LEDC 通道

void beep_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BEEP_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num = BEEP_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CH,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&channel);
}

void beep_on(void)
{
    // 50% 占空比 = 2^(13-1) = 4096
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CH, 4096);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CH);
}

void beep_off(void)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CH, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CH);
}
