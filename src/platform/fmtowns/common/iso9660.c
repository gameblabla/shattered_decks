#include "iso9660.h"
#include "cdrom.h"

#define ISO_SECTOR_SIZE   2048u
#define ISO_PVD_LBA         16u

static uint8_t g_sector[ISO_SECTOR_SIZE];

static int chr_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
}

/* Case-insensitive compare of a requested name against an ISO9660
 * directory-record name, ignoring a trailing ";<version>" on either
 * side (fmt_iso9660_find()'s caller may or may not pass ";1", and the
 * record almost always has it). */
static int name_matches(const char *want, const uint8_t *rec_name, int rec_len)
{
    int wi = 0;

    for (int ri = 0; ri < rec_len; ri++, wi++) {
        if (rec_name[ri] == ';') {
            break;
        }
        if (want[wi] == '\0' || want[wi] == ';') {
            return 0;
        }
        if (chr_upper(want[wi]) != chr_upper(rec_name[ri])) {
            return 0;
        }
    }
    return (want[wi] == '\0' || want[wi] == ';');
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fmt_iso9660_find(const char *name, uint32_t *lba, uint32_t *size)
{
    if (fmt_cdrom_read(ISO_PVD_LBA, 1, g_sector) != 0) {
        return -1;
    }
    /* Primary Volume Descriptor: type 1, "CD001" id at offset 1. */
    if (g_sector[0] != 1 || g_sector[1] != 'C' || g_sector[2] != 'D' ||
        g_sector[3] != '0' || g_sector[4] != '0' || g_sector[5] != '1') {
        return -1;
    }

    const uint8_t *root_rec = &g_sector[156];
    uint32_t dir_lba = read_le32(&root_rec[2]);
    uint32_t dir_size = read_le32(&root_rec[10]);
    uint32_t dir_sectors = (dir_size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;

    for (uint32_t s = 0; s < dir_sectors; s++) {
        if (fmt_cdrom_read(dir_lba + s, 1, g_sector) != 0) {
            return -1;
        }

        uint32_t off = 0;
        while (off < ISO_SECTOR_SIZE) {
            uint8_t rec_len = g_sector[off];
            if (rec_len == 0) {
                break; /* Padding to the next sector. */
            }

            uint8_t name_len = g_sector[off + 32];
            const uint8_t *rec_name = &g_sector[off + 33];

            if (name_matches(name, rec_name, name_len)) {
                if (lba) {
                    *lba = read_le32(&g_sector[off + 2]);
                }
                if (size) {
                    *size = read_le32(&g_sector[off + 10]);
                }
                return 0;
            }

            off += rec_len;
        }
    }

    return -1;
}

int32_t fmt_iso9660_load(const char *name, void *buf, uint32_t bufsize)
{
    uint32_t lba, size;

    if (fmt_iso9660_find(name, &lba, &size) != 0) {
        return -1;
    }
    if (size > bufsize) {
        return -1;
    }

    uint32_t full_sectors = size / ISO_SECTOR_SIZE;
    uint32_t tail = size % ISO_SECTOR_SIZE;
    uint8_t *dst = (uint8_t *)buf;

    if (full_sectors > 0) {
        if (fmt_cdrom_read(lba, (uint16_t)full_sectors, dst) != 0) {
            return -1;
        }
        dst += full_sectors * ISO_SECTOR_SIZE;
    }

    if (tail > 0) {
        if (fmt_cdrom_read(lba + full_sectors, 1, g_sector) != 0) {
            return -1;
        }
        for (uint32_t i = 0; i < tail; i++) {
            dst[i] = g_sector[i];
        }
    }

    return (int32_t)size;
}
