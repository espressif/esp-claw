idf_component_register(
    SRCS "src/claw_http.c"
    INCLUDE_DIRS "include"
    REQUIRES claw_endpoint esp_http_client
    PRIV_REQUIRES freertos
)
