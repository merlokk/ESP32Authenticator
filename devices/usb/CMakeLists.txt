# The USB port: Serial/JTAG or TinyUSB (CDC-ACM, FIDO2 HID), switched at run
# time; stdin / stdout follow it. CTAPHID framing from LionKey (lib/lionkey).
set(lk ${CMAKE_CURRENT_LIST_DIR}/../../lib/lionkey)

idf_component_register(SRCS "usb.cpp" "usb_fido.cpp" ${lk}/core/ctaphid.c
                    INCLUDE_DIRS "."
                    PRIV_INCLUDE_DIRS ${lk}/core
                    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_hal_usb esp_hw_support esp_system
                                  esp_timer vfs heap)

# Errors only, asserts off: as in components/fido_ctap.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LIONKEY_DEBUG_LEVEL=1)
set_source_files_properties(${lk}/core/ctaphid.c PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-unused-parameter")
# The stdio device's VFS table sets only the calls it serves.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
