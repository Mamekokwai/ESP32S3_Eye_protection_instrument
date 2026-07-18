idf_component_register(
    SRCS "app_uart.c" "audio_player.c" "reset_to_dl.c" "main.c" "audio.c" "avi.c" "mjpeg.c" "video_player.c" "raw_player.c" "flash_player.c"
    INCLUDE_DIRS "."
    EMBED_FILES "canon.pcm"
    REQUIRES BSP esp_timer spi_flash)