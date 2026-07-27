set(src_dirs
    SD_CARD
    SPILCD
    MYIIC)

set(include_dirs
    SD_CARD
    SPI_SD
    SPILCD
    MYIIC)

set(requires
    driver
    fatfs
    esp_lcd
    sdmmc
    esp_timer
    esp_lcd_jd9855)

idf_component_register(
    SRC_DIRS ${src_dirs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
