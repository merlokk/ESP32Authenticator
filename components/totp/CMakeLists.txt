# totp.cpp is pure (Base32, windows; host-tested); codes.cpp uses the crypto
# driver's TOTP.
idf_component_register(SRCS "totp.cpp" "codes.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES crypto)
