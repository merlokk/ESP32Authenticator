# Store files: the header (format, version, modification time; store_header.cpp
# is pure, host-tested, store_header_c.cpp its C view) and loading / saving
# with the rollback check (store_io.cpp).
idf_component_register(SRCS "store_header.cpp" "store_header_c.cpp" "store_io.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES nvs_flash)
