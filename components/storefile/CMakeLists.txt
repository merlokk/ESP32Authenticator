# Store file header (format, version, modification time); store_header.cpp is
# pure (host-tested), store_header_c.cpp is its C view.
idf_component_register(SRCS "store_header.cpp" "store_header_c.cpp"
                    INCLUDE_DIRS ".")
