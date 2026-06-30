#include "myiic.h"


i2c_master_bus_handle_t bus_handle;     /* 总线句柄 */
i2c_master_bus_handle_t bus_handle1 = NULL;
/**
 * @brief       初始化MYIIC
 * @param       无
 * @retval      ESP_OK:初始化成功
 */
esp_err_t myiic_init(void)
{
    /* 如果已经初始化，直接返回 */
    if (bus_handle != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source                     = I2C_CLK_SRC_DEFAULT,  /* 时钟源 */
        .i2c_port                       = IIC_NUM_PORT,         /* I2C端口 */
        .scl_io_num                     = IIC_SCL_GPIO_PIN,     /* SCL管脚 */
        .sda_io_num                     = IIC_SDA_GPIO_PIN,     /* SDA管脚 */
        .glitch_ignore_cnt              = 7,                    /* 故障周期 */
        .flags.enable_internal_pullup   = true,                 /* 内部上拉 */
    };
    /* 新建I2C总线 */
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &bus_handle));

    return ESP_OK;
}

esp_err_t myiic_init1(void)
{
    /* 如果已经初始化，直接返回 */
    if (bus_handle1 != NULL) {
        return ESP_OK;
    }

    // 初始化 I2C1，用于 ES8311
    i2c_master_bus_config_t bus_cfg1 = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = IIC1_NUM_PORT,
        .scl_io_num = IIC1_SCL_GPIO_PIN,
        .sda_io_num = IIC1_SDA_GPIO_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg1, &bus_handle1);
    if (ret != ESP_OK) {
        ESP_LOGE("myiic", "i2c_new_master_bus1 failed: %d", ret);
        return ret;
    }
    return ESP_OK;
}
