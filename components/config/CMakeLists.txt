idf_component_register(SRCS "config.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES passgen
                    PRIV_REQUIRES espressif__cjson timezone)
