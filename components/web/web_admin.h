#pragma once

// The write path of the password and FIDO2 pages. Pure: host-tested.
//
// - POST /api/vault/<verb> and /api/fido/delete take a JSON object; a
//   whitelist: `id` (record, 1..2^32-1), `position` (list place, 0..65535),
//   `site`, `user`, `password`, `otp`, `pin` (strings), `credential` (64
//   hex digits). Anything else is refused by
//   name. Which fields a verb needs is the server's business.
// - Every string in the parsed tree is overwritten before it is freed; the
//   caller wipes the request (vault::Wipe).
// - Only `Content-Type: application/json`: a cross-site form cannot send it
//   without a CORS preflight, which this server never answers.

#include <cstddef>
#include <cstdint>

#include "records.h"

namespace web {

// kSeedNew: a random seed phrase (vault::SetSeed), its words in the answer.
enum class VaultVerb : uint8_t {
    kNone, kAdd, kSet, kDelete, kUnlock, kLock, kMove, kSort, kSeedNew
};

// "/api/vault/<verb>", query ignored.
VaultVerb VaultVerbFromUri(const char *uri);
const char *VaultVerbName(VaultVerb verb);

constexpr size_t kCredentialIdSize = 32;
// Base32 as typed: spaces and '-' allowed, normalized by the vault.
constexpr size_t kOtpInputMax = 2 * vault::kOtpMax;

struct AdminRequest {
    bool has_id;
    uint32_t id;
    bool has_position;
    uint16_t position;
    bool has[4];  // by vault::Field: site, user, password, otp
    char site[vault::kSiteMax + 1];
    char user[vault::kUserMax + 1];
    char password[vault::kPasswordMax + 1];
    char otp[kOtpInputMax + 1];
    bool has_pin;
    char pin[vault::kPinLength + 1];
    bool has_credential;
    uint8_t credential[kCredentialIdSize];
};

enum class BodyResult : uint8_t { kOk, kNotJson, kNotObject, kUnknownField, kBadValue, kTooLong };

struct BodyOutcome {
    BodyResult result = BodyResult::kOk;
    char detail[24] = {};  // the field
};

// Fills `out` (cleared first); on a refusal it is cleared again.
BodyOutcome ParseAdminRequest(const char *body, size_t length, AdminRequest *out);
const char *BodyResultText(BodyResult result);

// The text of field `field` in `request` (site, user, password, otp).
const char *FieldValue(const AdminRequest &request, vault::Field field);

// `type` (lower case), optionally with parameters (`; charset=utf-8`).
bool IsType(const char *content_type, const char *type);
// "application/json", the same way.
bool IsJsonType(const char *content_type);

// `id=<1..2^32-1>` in the query of `uri`, as a whole parameter.
bool IdFromQuery(const char *uri, uint32_t *id);

// 64 hex digits (either case) into 32 bytes.
bool ParseCredentialId(const char *hex, uint8_t out[kCredentialIdSize]);

}  // namespace web
