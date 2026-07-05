idf_component_register(
    SRCS "main.c" "beep.c" "audio.c"
    INCLUDE_DIRS "."
    EMBED_FILES "canon.pcm"
    REQUIRES BSP)