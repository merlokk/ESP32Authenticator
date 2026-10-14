#pragma once

// The header in front of every store file (fido.bin, vault.bin): format,
// version, modification time, and how the payload is encrypted. It stays plain
// (an encrypted payload uses it as AES-GCM associated data). Pure,
// host-tested; the C side uses store_header_c.h.
//
// Layout, little-endian, then `length` payload bytes (+ a 16-byte tag when
// encrypted). Format 2 (written), 64 bytes:
//   0  magic "ESAF" (4)
//   4  format (1) = 2
//   5  kind (1): which store (a file of one kind is never loaded as another)
//   6  cipher (1): 0 plain, 1 AES-256-GCM
//   7  reserved (1) = 0
//   8  version (4): +1 on every write; the highest accepted one is in NVS
//  12  length (4): payload bytes
//  16  modified (8): Unix seconds, int64; 0 = unknown (no believable time)
//  24  nonce (12): AES-GCM; zero when plain
//  36  KDF iterations (4): PBKDF2-HMAC-SHA256 from the PIN; 0 = no KDF
//  40  KDF salt (16)
//  56  reserved (8) = 0
// Format 1 (read only): the first 40 bytes, without the KDF fields.

#include <cstddef>
#include <cstdint>

namespace storefile {

inline constexpr size_t kHeaderSize = 64;    // format 2; the most a header takes
inline constexpr size_t kHeaderSizeV1 = 40;
inline constexpr size_t kTagSize = 16;
inline constexpr size_t kNonceSize = 12;
inline constexpr size_t kSaltSize = 16;
inline constexpr uint8_t kFormat = 2;

enum class Kind : uint8_t { kFido = 1, kRecords = 2 };
enum class Cipher : uint8_t { kPlain = 0, kAesGcm = 1 };

struct Header {
    Kind kind = Kind::kFido;
    Cipher cipher = Cipher::kPlain;
    uint32_t version = 0;
    uint32_t length = 0;
    int64_t modified_utc = 0;
    uint8_t nonce[kNonceSize] = {};
    uint32_t kdf_iterations = 0;
    uint8_t salt[kSaltSize] = {};
    uint8_t size = kHeaderSize;  // bytes the header takes in the file (Decode)
};

// Always format 2 (kHeaderSize bytes).
void Encode(const Header &header, uint8_t out[kHeaderSize]);

enum class Parse {
    kOk,
    kNoHeader,  // no magic: a file from before the header (fido.bin only)
    kBad,       // magic, but unknown format/cipher, other kind, or a wrong size
};

// `data`: the first `size` bytes of a file of `file_size` bytes.
Parse Decode(const uint8_t *data, size_t size, size_t file_size, Kind kind, Header *out);

// What to do with a file read at boot against the highest version this device
// has accepted (NVS; `nvs_has` false before the first one).
enum class Verdict {
    kLoad,      // load; remember the version if above NVS
    kMigrate,   // a file without a header, before any version: load, rewrite
    kRollback,  // older than what this device has seen: refuse, leave it
};
Verdict Check(Parse parse, uint32_t version, bool nvs_has, uint32_t nvs_version);

// The version of the next write: above both the file and NVS.
uint32_t NextVersion(uint32_t file_version, uint32_t nvs_version);

// Sync: is `a` the newer copy? A later known time wins (0 loses to any known
// time); equal times: the higher version.
bool Newer(const Header &a, const Header &b);

}  // namespace storefile
