# ESP32Authenticator

Hardware authenticator built on the **XTEINK X4 Pro Pocket eReader** (ESP32-S3).
Firmware: C/C++ on **ESP-IDF**.

## Overview

- Stores an encrypted file with device settings and credentials.
- Credential entry: site (name) / login / password / TOTP / FIDO2.
- Credentials are shown on the e-ink screen and sent to the host via:
  - **BLE keyboard** (HID): types login/password/TOTP code;
  - **BLE FIDO2** authenticator.

## Sources

Reference projects (local paths + git): [sources.md](sources.md).

## Rules

- All docs and code comments are in English.
- Docs are as compact as possible.
- Commit directly to `main` and push right after committing.
- Commits and MRs: append ` +ai` to the subject/title; no `Co-Authored-By` or similar trailers.
- Reply in the language of the request.
