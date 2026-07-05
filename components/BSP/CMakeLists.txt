set(src_dirs    
            KEY
            LED
            MYSPI
            SPI_SD
            SPILCD
            MYIIC
            XL9555)

set(include_dirs
            KEY
            LED
            MYSPI
            SPI_SD
            SPILCD
            MYIIC
            XL9555)

set(requires
            driver
            fatfs
            esp_lcd
            sdmmc)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
