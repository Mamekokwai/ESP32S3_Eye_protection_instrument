#include "xl9555.h"

const char *xl9555_tag = "xl9555";
i2c_master_dev_handle_t xl9555_handle = NULL;
static uint16_t s_output_cache = 0;

/**
 * @brief       读取XL9555的IO值
 * @param       data:读取数据的存储区
 * @param       len:读取数据的大小
 * @retval      ESP_OK:读取成功; 其他:读取失败
 */
esp_err_t xl9555_read_byte(uint8_t *data, size_t len)
{
    uint8_t reg_addr = XL9555_INPUT_PORT0_REG;

    return i2c_master_transmit_receive(xl9555_handle, &reg_addr, 1, data, len, -1);
}

/**
 * @brief       向XL9555寄存器写入数据
 * @param       reg:寄存器地址
 * @param       data:要写入数据的存储区
 * @param       len:要写入数据的大小
 * @retval      ESP_OK:读取成功; 其他:读取失败
 */
esp_err_t xl9555_write_byte(uint8_t reg, uint8_t *data, size_t len)
{
    esp_err_t ret;

    uint8_t *buf = malloc(1 + len);
    if (buf == NULL)
    {
        ESP_LOGE(xl9555_tag, "%s memory failed", __func__);
        return ESP_ERR_NO_MEM;      /* 分配内存失败 */
    }

    buf[0] = reg;                   /* 0号元素为寄存器数值 */
    memcpy(buf + 1, data, len);     /* 拷贝数据至存储区中 */

    ret = i2c_master_transmit(xl9555_handle, buf, len + 1, -1);

    free(buf);                      /* 发送完成释放内存 */

    return ret;
}

/**
 * @brief       控制某个IO的电平
 * @param       pin     : 控制的IO
 * @param       val     : 电平
 * @retval      返回所有IO状态
 */
uint16_t xl9555_pin_write(uint16_t pin, int val)
{
    if (val) {
        s_output_cache |= pin;
    } else {
        s_output_cache &= ~pin;
    }

    uint8_t w_data[2];
    w_data[0] = s_output_cache & 0xFF;
    w_data[1] = (s_output_cache >> 8) & 0xFF;

    xl9555_write_byte(XL9555_OUTPUT_PORT0_REG, w_data, 2);

    return s_output_cache;
}

/**
 * @brief       获取某个IO状态
 * @param       pin : 要获取状态的IO
 * @retval      此IO口的值(状态, 0/1)
 */
int xl9555_pin_read(uint16_t pin)
{
    uint16_t ret;
    uint8_t r_data[2];

    xl9555_read_byte(r_data, 2);

    ret = r_data[1] << 8 | r_data[0];

    return (ret & pin) ? 1 : 0;
}

/**
 * @brief       XL9555的IO配置
 * @param       config_value：IO配置输入或者输出
 * @retval      返回设置的数值
 */
void xl9555_ioconfig(uint16_t config_value)
{
    uint8_t data[2];
    esp_err_t err;

    data[0] = (uint8_t)(0xFF & config_value);
    data[1] = (uint8_t)(0xFF & (config_value >> 8));

    do
    {
        err = xl9555_write_byte(XL9555_CONFIG_PORT0_REG, data, 2);
        if (err != ESP_OK)
        {
            ESP_LOGE(xl9555_tag, "%s configure %X failed, ret: %d", __func__, config_value, err);
        }

        vTaskDelay(pdMS_TO_TICKS(100));

    } while (err != ESP_OK);
}

/**
 * @brief       外部中断服务函数（ISR）
 * @note        IRAM_ATTR 减少延迟
 */
static void IRAM_ATTR xl9555_exit_gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;

    if (gpio_num == XL9555_INT_IO)
    {
        esp_rom_delay_us(20000);

        if (gpio_get_level(XL9555_INT_IO) == 0)
        {
            /* 中断处理 */
        }
    }
}

/**
 * @brief       外部中断初始化程序
 * @note        GPIO46 与 beep 冲突，本项目不使用
 * @param       无
 * @retval      无
 */
void xl9555_int_init(void)
{
    gpio_config_t gpio_init_struct;

    gpio_init_struct.mode         = GPIO_MODE_INPUT;
    gpio_init_struct.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_init_struct.intr_type    = GPIO_INTR_NEGEDGE;
    gpio_init_struct.pin_bit_mask = 1ull << XL9555_INT_IO;
    gpio_config(&gpio_init_struct);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(XL9555_INT_IO, xl9555_exit_gpio_isr_handler, (void*)XL9555_INT_IO);
}

/**
 * @brief       初始化XL9555
 * @param       无
 * @retval      ESP_OK:初始化成功
 */
esp_err_t xl9555_init(void)
{
    uint8_t r_data[2];

    /* 未调用myiic_init初始化IIC */
    if (bus_handle == NULL)
    {
        ESP_ERROR_CHECK(myiic_init());
    }

    i2c_device_config_t xl9555_i2c_dev_conf = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz    = IIC_SPEED_CLK,
        .device_address  = XL9555_ADDR,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &xl9555_i2c_dev_conf, &xl9555_handle));

    /* 上电先读取一次清除中断标志 */
    xl9555_read_byte(r_data, 2);
    xl9555_ioconfig(0x03F2);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 同步 SPI 控制逻辑
    s_output_cache = 0;
    xl9555_pin_write(TF_CS_IO, 1);
    xl9555_pin_write(LCD_CS_IO, 1);
    xl9555_pin_write(BACKLIGHT_IO, 0);

    ESP_LOGI(xl9555_tag, "XL9555 init done.");
    return ESP_OK;
}

/**
 * @brief       按键扫描函数
 * @param       mode:0->不连续;1->连续
 * @retval      键值
 */
uint8_t xl9555_key_scan(uint8_t mode)
{
    uint8_t keyval = 0;
    static uint8_t key_up = 1;

    if (mode)
    {
        key_up = 1;
    }

    if (key_up && (KEY1 == 0 || KEY2 == 0 || KEY3 == 0  || KEY4 == 0 ))
    {
        esp_rom_delay_us(100000);
        key_up = 0;

        if (KEY1 == 0) keyval = KEY1_PRES;
        if (KEY2 == 0) keyval = KEY2_PRES;
        if (KEY3 == 0) keyval = KEY3_PRES;
        if (KEY4 == 0) keyval = KEY4_PRES;
    }
    else if (KEY1 == 1 && KEY2 == 1 && KEY3 == 1 && KEY4 == 1)
    {
        key_up = 1;
    }

    return keyval;
}
