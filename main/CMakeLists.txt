idf_component_register(
    SRCS "app_uart.c" "audio_player.c" "mp3_decoder_wrapper.cpp" "sd_browser.c" "image_viewer.c" "reset_to_dl.c" "main.c" "audio.c" "avi.c" "mjpeg.c" "video_player.c" "raw_player.c" "flash_player.c" "testimage_data.c"
    INCLUDE_DIRS "."
    EMBED_FILES "canon.pcm"
    REQUIRES BSP esp_timer spi_flash esp_adc esp_mm)
