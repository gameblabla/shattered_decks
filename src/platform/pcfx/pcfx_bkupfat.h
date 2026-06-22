#ifndef PCFX_BKUPFAT_H
#define PCFX_BKUPFAT_H

#include <eris/types.h>

#define BKUPFAT_VOL_SIZE      0x8000U
#define BKUPFAT_SRM_SIZE      0x10000U
#define BKUPFAT_SRM_INTERNAL_OFFSET 0x0000U
#define BKUPFAT_SRM_EXTERNAL_OFFSET 0x8000U
#define BKUPFAT_SECTOR_SIZE   128U
#define BKUPFAT_CLUSTER_SIZE  128U

#define BKUPFAT_WRITE_OVERWRITE 0x00000001U

typedef enum bkupfat_result {
    BKUPFAT_OK = 0,
    BKUPFAT_ERR_INVALID_ARGUMENT = -1,
    BKUPFAT_ERR_INVALID_VOLUME = -2,
    BKUPFAT_ERR_UNSUPPORTED_VOLUME = -3,
    BKUPFAT_ERR_INVALID_NAME = -4,
    BKUPFAT_ERR_PATH_TOO_LONG = -5,
    BKUPFAT_ERR_NOT_FOUND = -6,
    BKUPFAT_ERR_ALREADY_EXISTS = -7,
    BKUPFAT_ERR_NOT_DIRECTORY = -8,
    BKUPFAT_ERR_IS_DIRECTORY = -9,
    BKUPFAT_ERR_DIR_FULL = -10,
    BKUPFAT_ERR_NO_SPACE = -11,
    BKUPFAT_ERR_FILE_TOO_LARGE = -12,
    BKUPFAT_ERR_BUFFER_TOO_SMALL = -13,
    BKUPFAT_ERR_CORRUPT_CHAIN = -14,
    BKUPFAT_ERR_NOT_EMPTY = -15
} bkupfat_result_t;

typedef struct bkupfat {
    u8 *image;
    u32 size;
} bkupfat_t;

typedef enum bkupfat_device {
    BKUPFAT_DEVICE_INTERNAL = 0,
    BKUPFAT_DEVICE_EXTERNAL = 1
} bkupfat_device_t;

const char *bkupfat_strerror(int rc);
int bkupfat_mount(bkupfat_t *fs, u8 *image, u32 size);
int bkupfat_is_valid_internal(const u8 *image, u32 size);
int bkupfat_is_valid_external(const u8 *image, u32 size);
int bkupfat_is_valid_device(const u8 *image, u32 size, bkupfat_device_t device);
int bkupfat_format_internal(u8 *image, u32 size);
int bkupfat_format_external(u8 *image, u32 size);
int bkupfat_format_device(u8 *image, u32 size, bkupfat_device_t device);
int bkupfat_mount_device(bkupfat_t *fs, u8 *image, u32 size, bkupfat_device_t device);
u8 *bkupfat_srm_volume(u8 *srm, u32 size, bkupfat_device_t device);
const u8 *bkupfat_srm_const_volume(const u8 *srm, u32 size, bkupfat_device_t device);
int bkupfat_srm_mount(bkupfat_t *fs, u8 *srm, u32 size, bkupfat_device_t device);

u32 bkupfat_free_clusters(const bkupfat_t *fs);
u32 bkupfat_required_clusters(u32 byte_count);

int bkupfat_exists(bkupfat_t *fs, const char *path, u8 *attr_out, u32 *size_out);
int bkupfat_create_folder(bkupfat_t *fs, const char *path);
int bkupfat_create_file(bkupfat_t *fs, const char *path, const void *data, u32 len, u32 flags);
int bkupfat_read_file(bkupfat_t *fs, const char *path, void *out, u32 out_capacity, u32 *len_out);

#endif
