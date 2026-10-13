# Patches NimBLE in $IDF_PATH (idempotent) so the last GATT service reports
# end group handle 0xFFFF in Find By Type Value responses, as NimBLE already
# does for Read By Group Type. Windows 11 WebAuthn (BLE FIDO) looks the FIDO
# service up by Find By Type Value and fails with ERROR_DEVICE_NOT_AVAILABLE
# when that range differs from the one its PnP service node was built from.

set(_file "$ENV{IDF_PATH}/components/bt/host/nimble/nimble/nimble/host/src/ble_att_svr.c")
set(_marker "(ESP32Authenticator patch)")
set(_old "    if (first) {
        rc = ble_att_svr_fill_type_value_entry(txom, first, prev, mtu,
                                               out_att_err);")
set(_new "    if (first) {
        /* Last group in the table: end handle 0xffff, as Read By Group Type
         * does ${_marker}. */
        rc = ble_att_svr_fill_type_value_entry(txom, first, 0xFFFF, mtu,
                                               out_att_err);")

file(READ "${_file}" _src)
string(FIND "${_src}" "${_marker}" _patched)
if(_patched EQUAL -1)
    string(FIND "${_src}" "${_old}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "nimble_patch: code to patch not found in ${_file}")
    endif()
    string(REPLACE "${_old}" "${_new}" _src "${_src}")
    file(WRITE "${_file}" "${_src}")
    message(STATUS "nimble_patch: patched ${_file}")
endif()
