# Board classes (board_spec.h, dev_board.h, x4_pro.h: pure, host-tested), the
# build-time choice (board.h <- drivers/config.h) and the board's devices.
idf_component_register(SRCS "board_devices.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES buttons clock)
