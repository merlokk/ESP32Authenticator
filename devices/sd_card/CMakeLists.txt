# microSD (SDMMC, 1-bit) and FAT; sd_policy.h/.cpp are pure (host-tested).
idf_component_register(SRCS "sd_card.cpp" "sd_policy.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES boards esp_driver_sdmmc esp_driver_gpio sdmmc fatfs freertos)
