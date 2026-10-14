# zones.cpp is pure (host-tested); timezone.cpp sets TZ in libc.
idf_component_register(SRCS "zones.cpp" "timezone.cpp"
                    INCLUDE_DIRS ".")
