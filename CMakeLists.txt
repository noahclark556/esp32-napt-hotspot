idf_component_register(
    SRCS "src/napt_interface.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_wifi lwip
)
