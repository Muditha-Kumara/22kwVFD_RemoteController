idf_component_register(
    SRCS "sys_logger.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer log
    PRIV_REQUIRES esp_common
)
