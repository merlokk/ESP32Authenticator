# Windows 11: BLE FIDO2 not usable from Chrome

Fixed 2026-10-13. Verified: Chrome on webauthn.io (register + authenticate,
cross-platform), python-fido2 `WindowsClient`, full CTAP2 suite over BLE.

## Symptom

Chrome (any browser: on Windows all go through `webauthn.dll`) never reaches the
board. `Microsoft-Windows-WebAuthN/Operational` shows the key found, then a loop
every ~250 ms until timeout:

```
2260 Ctap Ble device thread started. DevicePath: ble:ac276ea4584a_002b PairedName: ESP32 Auth
2270 _GetFidoGattService  ErrorCode     0x8000000E (E_ILLEGAL_METHOD_CALL)
2270 _GetFidoGattService  AsyncComplete 0x800710DF (ERROR_DEVICE_NOT_AVAILABLE)
2270 _BleCreateContext                  0x800710DF
```

## Cause

NimBLE reported two different handle ranges for the last GATT service (FIDO 0xFFFD):

| ATT request | Used by | End Group Handle |
|---|---|---|
| Read By Group Type (all primary services) | Windows PnP, builds the service devnode | `0xFFFF` (NimBLE: last group) |
| Find By Type Value (service by UUID) | WebAuthn (CryptSvc, NetworkService) | `0x0034` (real last handle) |

Both forms are legal (Core spec Vol 3 Part G 4.4), but WebAuthn rejects the
mismatch: after a correct discovery it fails without sending Read By Type 0x2803.

```
before  → 06 0100 FFFF 0028 FDFF   ← 07 2B00 3400     → 06 3500 ...  ← 01 06 3500 0A
        (no more traffic, _GetFidoGattService 0x800710DF)
after   → 06 0100 FFFF 0028 FDFF   ← 07 2B00 FFFF     → 08 2B00 FFFF 0328 (characteristics) → CTAP
```

## Fix

The mismatch exists only for the **last** service in the table: NimBLE uses
0xFFFF there in Read By Group Type and the real last handle in Find By Type Value;
for any other service both give the handle before the next service. So a small
tail service is registered after all profile services (`drivers/ble/ble.cpp`,
`ble::On()`), and the FIDO range is 0x002B–0x0034 in both responses:

```cpp
// 40e2955e-e36d-4cca-9827-9890bace65f7: tail service, 40e2955f-...: layout version.
const ble_gatt_svc_def kTailServices[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kTailUuid.u, .characteristics = kTailChrs},
    {},
};
...
    for (size_t i = 0; rc == 0 && i < service_table_count; ++i) {
        rc = ble_gatts_count_cfg(service_tables[i]);
        rc = rc == 0 ? ble_gatts_add_svcs(service_tables[i]) : rc;
    }
    rc = rc == 0 ? ble_gatts_count_cfg(kTailServices) : rc;
    rc = rc == 0 ? ble_gatts_add_svcs(kTailServices) : rc;  // must stay last
```

Windows caches the GATT table of a bonded device: after a layout change the host
must re-read it (remove the device and pair again), otherwise WebAuthn still
compares against the old cached range and fails the same way.

Alternative (used first, commit 60ed7c3, removed): patch NimBLE
`ble_att_svr_fill_type_value()` to report 0xFFFF for the last group in Find By
Type Value too (`fill_type_value_entry(txom, first, 0xFFFF, ...)` instead of `prev`).

## How it was found

1. Repro without Chrome: python-fido2 `WindowsClient.make_credential()` with
   `authenticatorAttachment: cross-platform` gives the same log.
2. ETW, elevated: `logman create trace wa -ets -o wa.etl -p {8A1F9517-3A8C-4A9E-A018-4F17A200F277} 0xffffffffffffffff 0xff`
   (BTHPORT; event 402 = raw HCI, `BIP_Type` 4 = TX ACL, 3 = RX ACL), plus
   WebAuthN `{3AE1EA61-C002-47FB-B06C-4022A8C98929}`; `logman stop wa -ets`,
   `tracerpt wa.etl -o wa.xml -of XML`.
3. ATT dump showed discovery answered correctly and nothing after it.
4. Misleading: an elevated WinRT probe (`GattDeviceService.FromIdAsync` +
   `GetCharacteristicsAsync(Uncached)`) did the same discovery and succeeded, so it
   is not a stand-in for WebAuthn. Non-elevated apps cannot open FFFD at all
   (protected service; `GetGattServicesForUuidAsync` throws 0x80070016).

Ruled out on the way: GATT layout vs CTAP spec, `fidoControlPointLength` (now
MTU − 3), bonding / re-encryption, HID on the same device, stale bond.

## Notes

- Intermediate WebAuthN events 2270 with 0x773 / 0x52E in `_IsBleAutoSelectedDevice`
  during device selection precede a successful retry; they are not failures.
- Public reports of commercial BLE keys (Feitian K41, SpearID) failing on Win11 but
  working on Win10 may share this cause (unconfirmed).
