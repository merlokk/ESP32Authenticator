# Windows 11: BLE FIDO2 not usable from Chrome

Fixed 2026-10-13 (tail service 6a400d6, Service Changed 86a64f2). Verified on that
firmware: Chrome on webauthn.io (register + authenticate, cross-platform, with PIN),
python-fido2 `WindowsClient`, full CTAP2 suite over BLE (59 passed, 8 skipped:
`--destructive` / `--reset` only).

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
after   (same discovery, Windows cache re-read)  → 08 2B00 3400 0328 (characteristics) → CTAP
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

Windows caches the GATT table of a bonded device and would keep comparing against
the old range. The firmware therefore indicates Service Changed on layout changes
(`CheckGattLayout()` in `drivers/ble/ble.cpp`): an FNV-1a hash of all service
tables is kept in NVS (`ble/gatt_hash`); when it differs at stack sync,
`ble_svc_gatt_changed(0x0001, 0xFFFF)` marks every bonded host subscribed to
Service Changed, and NimBLE sends the indication after re-encryption. Verified:
Windows confirms and re-runs Read By Group Type; added/removed services get/lose
their devnodes without re-pairing:

```
← 1D 0800 0100 FFFF   (Handle Value Indication: Service Changed, 0x0001–0xFFFF)
→ 1E                  (confirmation)
→ 10 0100 FFFF 0028   (Read By Group Type: Windows rediscovers)
```

 The range lives in RAM only, so it is set again
at boot while an indication is still pending.

## Workaround: Windows still fails after a firmware update

Symptom: same `_GetFidoGattService` 0x800710DF loop although the firmware has the
tail service. Cause: Windows uses its cached GATT table of the bonded device
(firmware without Service Changed, or the host never subscribed to it).

1. Windows: Settings → Bluetooth & devices → ESP32 Auth → Remove device.
2. `python utils/ble.py -p COM6 pair 90` (prints the passkey).
3. Windows: Add device → Bluetooth → ESP32 Auth, type the passkey.
4. Check that Windows re-read the table: the tail service has a devnode.

```powershell
Get-PnpDevice | ? InstanceId -match 'ac276ea4584a' | ? InstanceId -match '40E2955E'
```

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

- WebAuthN 2270 errors in `_IsBleAutoSelectedDevice` during device selection are
  followed by a successful retry; they are not failures: 0x52E on every request,
  0x773 (ERROR_PASSWORD_MUST_CHANGE) when the key had no PIN (Windows then has the
  user set one; inferred, not verified).
- 0x8009000F (NTE_EXISTS) on MakeCredential: the key already holds a credential
  from the RP's `excludeCredentials` (CTAP2_ERR_CREDENTIAL_EXCLUDED), e.g. the
  same webauthn.io user name registered twice. Expected.
- Tests: Windows hides FFFD from non-elevated processes, run the BLE suite from an
  elevated shell.
- Public reports of commercial BLE keys (Feitian K41, SpearID) failing on Win11 but
  working on Win10 may share this cause (unconfirmed).
