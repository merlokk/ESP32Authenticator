#include <cinttypes>
#include <cstdio>

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "hardware.h"
#include "soc/efuse_reg.h"
#include "soc/soc.h"

namespace hardware {

namespace {

struct EfuseField {
    const char *name;
    const esp_efuse_desc_t **desc;
    int block;
    const char *comment;
};

#define EFUSE_FIELD(name, desc, block, comment) {name, desc, block, comment},
const EfuseField kFields[] = {
#include "efuse_fields.inc"
};
#undef EFUSE_FIELD

// Registers per block on ESP32-S3: BLK0 and BLK1 have 6, the rest 8.
constexpr int kBlockCount = 11;
constexpr int kRegsPerBlock[kBlockCount] = {6, 6, 8, 8, 8, 8, 8, 8, 8, 8, 8};

}  // namespace

void PrintEfuseFields() {
    printf("[efuse fields] %u fields\n", static_cast<unsigned>(sizeof kFields / sizeof kFields[0]));
    for (const EfuseField &f : kFields) {
        const int bits = esp_efuse_get_field_size(f.desc);
        printf("BLK%-2d %-40s ", f.block, f.name);
        if (bits <= 32) {
            uint32_t value = 0;
            esp_efuse_read_field_blob(f.desc, &value, bits);
            printf("%-10" PRIu32, value);
            if (bits > 1) {
                printf(" 0x%" PRIx32, value);
            }
        } else {
            uint8_t data[32] = {};
            const int bytes = (bits + 7) / 8;
            esp_efuse_read_field_blob(f.desc, data, bits);
            for (int i = 0; i < bytes && i < static_cast<int>(sizeof data); ++i) {
                printf("%02x", data[i]);
            }
        }
        printf("  %.60s\n", f.comment);
    }
}

void PrintEfuseRaw() {
    printf("[efuse raw]\n");
    for (int blk = 0; blk < kBlockCount; ++blk) {
        printf("BLK%-2d", blk);
        for (int reg = 0; reg < kRegsPerBlock[blk]; ++reg) {
            // BLK0 is not readable through the block API: read its registers
            // (WR_DIS, REPEAT_DATA0..4) directly.
            if (blk == 0) {
                printf(" %08" PRIx32, REG_READ(EFUSE_RD_WR_DIS_REG + reg * 4));
                continue;
            }
            // esp_efuse_read_reg() asserts on error; read_block reports it instead.
            uint32_t value = 0;
            if (esp_efuse_read_block(static_cast<esp_efuse_block_t>(blk), &value, reg * 32, 32) ==
                ESP_OK) {
                printf(" %08" PRIx32, value);
            } else {
                printf(" --------");
            }
        }
        printf("\n");
    }
}

}  // namespace hardware
