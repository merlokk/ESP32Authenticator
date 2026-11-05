# The password generator: passgen.h is pure (host-tested); the word lists.
idf_component_register(SRCS "passgen_words.cpp" "words_eff.cpp" "words_slip39.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES vault)
