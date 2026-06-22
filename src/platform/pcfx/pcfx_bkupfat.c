#include "pcfx_bkupfat.h"

#define ATTR_DIR     0x10U
#define ATTR_ARCHIVE 0x20U
#define FAT12_EOC    0xFFFU
#define FAT12_BAD    0xFF7U

typedef struct bkupfat_name {
    char name[9];
    char ext[4];
} bkupfat_name_t;

typedef struct bkupfat_dir {
    u8 is_root;
    u16 start_cluster;
    u32 parent_cluster;
} bkupfat_dir_t;

static void mem_zero(u8 *p, u32 n) { u32 i; for(i = 0; i < n; i++) p[i] = 0; }
static void mem_copy(u8 *d, const u8 *s, u32 n) { u32 i; for(i = 0; i < n; i++) d[i] = s[i]; }
static int mem_equal(const u8 *a, const u8 *b, u32 n) { u32 i; for(i = 0; i < n; i++) if(a[i] != b[i]) return 0; return 1; }

static const u8 bkupfat_magic_internal[8] = { 'P','C','F','X','S','r','a','m' };
static const u8 bkupfat_magic_external[8] = { 'P','C','F','X','C','a','r','d' };

static u16 rd16(const u8 *b, u32 o) { return (u16)b[o] | ((u16)b[o + 1] << 8); }
static u32 rd32(const u8 *b, u32 o) { return (u32)rd16(b, o) | ((u32)rd16(b, o + 2) << 16); }
static void wr16(u8 *b, u32 o, u16 v) { b[o] = (u8)v; b[o + 1] = (u8)(v >> 8); }
static void wr32(u8 *b, u32 o, u32 v) { wr16(b, o, (u16)v); wr16(b, o + 2, (u16)(v >> 16)); }

static u16 bps(const bkupfat_t *fs) { return rd16(fs->image, 0x0B); }
static u8 spc(const bkupfat_t *fs) { return fs->image[0x0D]; }
static u16 reserved_sectors(const bkupfat_t *fs) { return rd16(fs->image, 0x0E); }
static u8 fat_count(const bkupfat_t *fs) { return fs->image[0x10]; }
static u16 root_entries(const bkupfat_t *fs) { return rd16(fs->image, 0x11); }
static u16 total_sectors(const bkupfat_t *fs) { return rd16(fs->image, 0x13); }
static u16 sectors_per_fat(const bkupfat_t *fs) { return rd16(fs->image, 0x16); }
static u32 fat_off(const bkupfat_t *fs) { return (u32)reserved_sectors(fs) * bps(fs); }
static u32 root_off(const bkupfat_t *fs) { return ((u32)reserved_sectors(fs) + ((u32)fat_count(fs) * sectors_per_fat(fs))) * bps(fs); }
static u32 root_size(const bkupfat_t *fs) { return (u32)root_entries(fs) * 32U; }
static u32 root_sectors(const bkupfat_t *fs) { return (root_size(fs) + bps(fs) - 1U) / bps(fs); }
static u32 data_off(const bkupfat_t *fs) { return root_off(fs) + root_sectors(fs) * bps(fs); }
static u32 cluster_size(const bkupfat_t *fs) { return (u32)spc(fs) * bps(fs); }
static u16 max_cluster(const bkupfat_t *fs) { return (u16)(2U + ((total_sectors(fs) - (data_off(fs) / bps(fs))) / spc(fs)) - 1U); }
static u32 cluster_off(const bkupfat_t *fs, u16 c) { return data_off(fs) + ((u32)c - 2U) * cluster_size(fs); }

static int valid_cluster(const bkupfat_t *fs, u16 c) { return c >= 2U && c <= max_cluster(fs); }
static int is_eoc(u16 v) { return v >= 0xFF8U && v <= 0xFFFU; }

const char *bkupfat_strerror(int rc)
{
    switch(rc) {
    case BKUPFAT_OK: return "OK";
    case BKUPFAT_ERR_INVALID_ARGUMENT: return "invalid argument";
    case BKUPFAT_ERR_INVALID_VOLUME: return "invalid BackupRAM volume";
    case BKUPFAT_ERR_UNSUPPORTED_VOLUME: return "unsupported FAT12 volume layout";
    case BKUPFAT_ERR_INVALID_NAME: return "invalid 8.3 path/name";
    case BKUPFAT_ERR_PATH_TOO_LONG: return "path too long";
    case BKUPFAT_ERR_NOT_FOUND: return "not found";
    case BKUPFAT_ERR_ALREADY_EXISTS: return "already exists";
    case BKUPFAT_ERR_NOT_DIRECTORY: return "not a directory";
    case BKUPFAT_ERR_IS_DIRECTORY: return "is a directory";
    case BKUPFAT_ERR_DIR_FULL: return "directory has no free entry";
    case BKUPFAT_ERR_NO_SPACE: return "not enough BackupRAM space";
    case BKUPFAT_ERR_FILE_TOO_LARGE: return "file too large";
    case BKUPFAT_ERR_BUFFER_TOO_SMALL: return "read buffer too small";
    case BKUPFAT_ERR_CORRUPT_CHAIN: return "corrupt FAT chain";
    case BKUPFAT_ERR_NOT_EMPTY: return "directory not empty";
    default: return "unknown error";
    }
}

static int bkupfat_is_valid_layout(const u8 *image, u32 size)
{
    if(!image || size != BKUPFAT_VOL_SIZE) return 0;
    if(rd16(image, 0x0B) != 128U) return 0;
    if(image[0x0D] != 1U) return 0;
    if(rd16(image, 0x0E) != 1U) return 0;
    if(image[0x10] != 1U) return 0;
    if(rd16(image, 0x11) != 64U) return 0;
    if(rd16(image, 0x13) != 256U) return 0;
    if(image[0x15] != 0xF9U) return 0;
    if(rd16(image, 0x16) != 3U) return 0;
    return 1;
}

static int bkupfat_has_magic(const u8 *image, bkupfat_device_t device)
{
    const u8 *magic = (device == BKUPFAT_DEVICE_EXTERNAL) ? bkupfat_magic_external : bkupfat_magic_internal;
    return mem_equal(&image[0x03], magic, 8U);
}

int bkupfat_is_valid_device(const u8 *image, u32 size, bkupfat_device_t device)
{
    if(device != BKUPFAT_DEVICE_INTERNAL && device != BKUPFAT_DEVICE_EXTERNAL) return 0;
    if(!bkupfat_is_valid_layout(image, size)) return 0;
    if(!bkupfat_has_magic(image, device)) return 0;
    return 1;
}

int bkupfat_is_valid_internal(const u8 *image, u32 size)
{
    return bkupfat_is_valid_device(image, size, BKUPFAT_DEVICE_INTERNAL);
}

int bkupfat_is_valid_external(const u8 *image, u32 size)
{
    return bkupfat_is_valid_device(image, size, BKUPFAT_DEVICE_EXTERNAL);
}

int bkupfat_format_device(u8 *image, u32 size, bkupfat_device_t device)
{
    static const u8 boot[32] = {
        0x24,0x8A,0xDF,'P','C','F','X','S','r','a','m',0x80,
        0x00,0x01,0x01,0x00,0x01,0x40,0x00,0x00,0x01,0xF9,0x03,0x00,
        0x01,0x00,0x01,0x00,0x00,0x00,0x00,0x00
    };
    const u8 *magic;
    if(!image || size != BKUPFAT_VOL_SIZE) return BKUPFAT_ERR_INVALID_ARGUMENT;
    if(device != BKUPFAT_DEVICE_INTERNAL && device != BKUPFAT_DEVICE_EXTERNAL) return BKUPFAT_ERR_INVALID_ARGUMENT;
    mem_zero(image, size);
    mem_copy(image, boot, 32U);
    magic = (device == BKUPFAT_DEVICE_EXTERNAL) ? bkupfat_magic_external : bkupfat_magic_internal;
    mem_copy(&image[0x03], magic, 8U);
    image[0x80] = 0xF9;
    image[0x81] = 0xFF;
    image[0x82] = 0xFF;
    return BKUPFAT_OK;
}

int bkupfat_format_internal(u8 *image, u32 size)
{
    return bkupfat_format_device(image, size, BKUPFAT_DEVICE_INTERNAL);
}

int bkupfat_format_external(u8 *image, u32 size)
{
    return bkupfat_format_device(image, size, BKUPFAT_DEVICE_EXTERNAL);
}

int bkupfat_mount_device(bkupfat_t *fs, u8 *image, u32 size, bkupfat_device_t device)
{
    if(!fs || !image || size != BKUPFAT_VOL_SIZE) return BKUPFAT_ERR_INVALID_ARGUMENT;
    if(device != BKUPFAT_DEVICE_INTERNAL && device != BKUPFAT_DEVICE_EXTERNAL) return BKUPFAT_ERR_INVALID_ARGUMENT;
    fs->image = image;
    fs->size = size;
    if(!bkupfat_is_valid_device(image, size, device)) return BKUPFAT_ERR_INVALID_VOLUME;
    if(bps(fs) != 128U || spc(fs) != 1U || fat_count(fs) != 1U || root_entries(fs) != 64U || total_sectors(fs) != 256U || sectors_per_fat(fs) != 3U) {
        return BKUPFAT_ERR_UNSUPPORTED_VOLUME;
    }
    return BKUPFAT_OK;
}

int bkupfat_mount(bkupfat_t *fs, u8 *image, u32 size)
{
    if(!fs || !image || size != BKUPFAT_VOL_SIZE) return BKUPFAT_ERR_INVALID_ARGUMENT;
    fs->image = image;
    fs->size = size;
    if(!bkupfat_is_valid_layout(image, size)) return BKUPFAT_ERR_INVALID_VOLUME;
    if(!mem_equal(&image[0x03], bkupfat_magic_internal, 8U) && !mem_equal(&image[0x03], bkupfat_magic_external, 8U)) return BKUPFAT_ERR_INVALID_VOLUME;
    if(bps(fs) != 128U || spc(fs) != 1U || fat_count(fs) != 1U || root_entries(fs) != 64U || total_sectors(fs) != 256U || sectors_per_fat(fs) != 3U) {
        return BKUPFAT_ERR_UNSUPPORTED_VOLUME;
    }
    return BKUPFAT_OK;
}

u8 *bkupfat_srm_volume(u8 *srm, u32 size, bkupfat_device_t device)
{
    if(!srm || size < BKUPFAT_SRM_SIZE) return 0;
    if(device == BKUPFAT_DEVICE_INTERNAL) return srm + BKUPFAT_SRM_INTERNAL_OFFSET;
    if(device == BKUPFAT_DEVICE_EXTERNAL) return srm + BKUPFAT_SRM_EXTERNAL_OFFSET;
    return 0;
}

const u8 *bkupfat_srm_const_volume(const u8 *srm, u32 size, bkupfat_device_t device)
{
    if(!srm || size < BKUPFAT_SRM_SIZE) return 0;
    if(device == BKUPFAT_DEVICE_INTERNAL) return srm + BKUPFAT_SRM_INTERNAL_OFFSET;
    if(device == BKUPFAT_DEVICE_EXTERNAL) return srm + BKUPFAT_SRM_EXTERNAL_OFFSET;
    return 0;
}

int bkupfat_srm_mount(bkupfat_t *fs, u8 *srm, u32 size, bkupfat_device_t device)
{
    u8 *vol = bkupfat_srm_volume(srm, size, device);
    if(!vol) return BKUPFAT_ERR_INVALID_ARGUMENT;
    return bkupfat_mount_device(fs, vol, BKUPFAT_VOL_SIZE, device);
}

static u16 fat_get(const bkupfat_t *fs, u16 c)
{
    u32 fo = fat_off(fs) + c + (c >> 1);
    u16 val;
    if(c & 1U) val = (u16)((fs->image[fo] >> 4) | ((u16)fs->image[fo + 1] << 4));
    else val = (u16)(fs->image[fo] | (((u16)fs->image[fo + 1] & 0x000F) << 8));
    return (u16)(val & 0x0FFFU);
}

static void fat_set(bkupfat_t *fs, u16 c, u16 value)
{
    u32 fo = fat_off(fs) + c + (c >> 1);
    value &= 0x0FFFU;
    if(c & 1U) {
        fs->image[fo] = (u8)((fs->image[fo] & 0x0FU) | ((value & 0x000FU) << 4));
        fs->image[fo + 1] = (u8)(value >> 4);
    } else {
        fs->image[fo] = (u8)value;
        fs->image[fo + 1] = (u8)((fs->image[fo + 1] & 0xF0U) | (value >> 8));
    }
}

u32 bkupfat_required_clusters(u32 byte_count)
{
    if(byte_count == 0) return 0;
    return (byte_count + BKUPFAT_CLUSTER_SIZE - 1U) / BKUPFAT_CLUSTER_SIZE;
}

u32 bkupfat_free_clusters(const bkupfat_t *fs)
{
    u16 c;
    u32 n = 0;
    if(!fs || !fs->image) return 0;
    for(c = 2; c <= max_cluster(fs); c++) if(fat_get(fs, c) == 0) n++;
    return n;
}

static int chain_count(const bkupfat_t *fs, u16 start, u32 *count_out)
{
    u16 c = start;
    u32 count = 0;
    u32 guard = 0;
    if(start == 0) { *count_out = 0; return BKUPFAT_OK; }
    if(!valid_cluster(fs, start)) return BKUPFAT_ERR_CORRUPT_CHAIN;
    while(valid_cluster(fs, c)) {
        u16 next;
        count++;
        guard++;
        if(guard > (u32)(max_cluster(fs) - 1U)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        next = fat_get(fs, c);
        if(next == 0 || next == FAT12_BAD) return BKUPFAT_ERR_CORRUPT_CHAIN;
        if(is_eoc(next)) { *count_out = count; return BKUPFAT_OK; }
        if(!valid_cluster(fs, next)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        c = next;
    }
    return BKUPFAT_ERR_CORRUPT_CHAIN;
}

static void chain_free(bkupfat_t *fs, u16 start)
{
    u16 c = start;
    u32 guard = 0;
    if(!valid_cluster(fs, start)) return;
    while(valid_cluster(fs, c) && guard <= (u32)(max_cluster(fs) - 1U)) {
        u16 next = fat_get(fs, c);
        fat_set(fs, c, 0);
        if(is_eoc(next) || !valid_cluster(fs, next)) return;
        c = next;
        guard++;
    }
}

static int alloc_chain(bkupfat_t *fs, u32 clusters, u16 *first_out)
{
    u16 first = 0;
    u16 prev = 0;
    u32 made = 0;
    u16 c;
    if(clusters == 0) { *first_out = 0; return BKUPFAT_OK; }
    for(c = 2; c <= max_cluster(fs) && made < clusters; c++) {
        if(fat_get(fs, c) == 0) {
            if(first == 0) first = c;
            if(prev != 0) fat_set(fs, prev, c);
            fat_set(fs, c, FAT12_EOC);
            mem_zero(&fs->image[cluster_off(fs, c)], cluster_size(fs));
            prev = c;
            made++;
        }
    }
    if(made != clusters) {
        if(first != 0) chain_free(fs, first);
        return BKUPFAT_ERR_NO_SPACE;
    }
    *first_out = first;
    return BKUPFAT_OK;
}

static u8 to_upper(u8 c)
{
    if(c >= 'a' && c <= 'z') return (u8)(c - 'a' + 'A');
    return c;
}

static int valid_name_char(u8 c)
{
    c = to_upper(c);
    if(c >= 'A' && c <= 'Z') return 1;
    if(c >= '0' && c <= '9') return 1;
    if(c == '_' || c == '-' || c == '$' || c == '~') return 1;
    return 0;
}

static int parse_component(const char *start, u32 len, bkupfat_name_t *out)
{
    u32 i;
    u32 nlen = 0;
    u32 elen = 0;
    u8 saw_dot = 0;
    if(len == 0 || len > 12U || !out) return BKUPFAT_ERR_INVALID_NAME;
    for(i = 0; i < 9U; i++) out->name[i] = 0;
    for(i = 0; i < 4U; i++) out->ext[i] = 0;
    if((len == 1U && start[0] == '.') || (len == 2U && start[0] == '.' && start[1] == '.')) return BKUPFAT_ERR_INVALID_NAME;
    for(i = 0; i < len; i++) {
        u8 c = (u8)start[i];
        if(c == '.') {
            if(saw_dot || nlen == 0) return BKUPFAT_ERR_INVALID_NAME;
            saw_dot = 1;
            continue;
        }
        if(!valid_name_char(c)) return BKUPFAT_ERR_INVALID_NAME;
        c = to_upper(c);
        if(!saw_dot) {
            if(nlen >= 8U) return BKUPFAT_ERR_INVALID_NAME;
            out->name[nlen++] = (char)c;
        } else {
            if(elen >= 3U) return BKUPFAT_ERR_INVALID_NAME;
            out->ext[elen++] = (char)c;
        }
    }
    if(nlen == 0 || (saw_dot && elen == 0)) return BKUPFAT_ERR_INVALID_NAME;
    return BKUPFAT_OK;
}

static int next_component(const char **path, bkupfat_name_t *out, int *last)
{
    const char *p;
    const char *q;
    u32 len = 0;
    int rc;
    if(!path || !*path || !out || !last) return BKUPFAT_ERR_INVALID_ARGUMENT;
    p = *path;
    while(*p == '/') p++;
    if(*p == 0) return BKUPFAT_ERR_INVALID_NAME;
    q = p;
    while(*q && *q != '/') { q++; len++; if(len > 12U) return BKUPFAT_ERR_INVALID_NAME; }
    rc = parse_component(p, len, out);
    if(rc != BKUPFAT_OK) return rc;
    while(*q == '/') q++;
    *last = (*q == 0);
    *path = q;
    return BKUPFAT_OK;
}

static void name83(u8 *dst, const bkupfat_name_t *n)
{
    u32 i;
    for(i = 0; i < 11U; i++) dst[i] = ' ';
    for(i = 0; i < 8U && n->name[i]; i++) dst[i] = (u8)n->name[i];
    for(i = 0; i < 3U && n->ext[i]; i++) dst[8U + i] = (u8)n->ext[i];
}

static int entry_matches(bkupfat_t *fs, u32 off, const bkupfat_name_t *n)
{
    u8 tmp[11];
    (void)fs;
    name83(tmp, n);
    return mem_equal(&fs->image[off], tmp, 11U);
}

static void set_entry(bkupfat_t *fs, u32 off, const bkupfat_name_t *n, u8 attr, u16 cluster, u32 size)
{
    mem_zero(&fs->image[off], 32U);
    name83(&fs->image[off], n);
    fs->image[off + 11U] = attr;
    wr16(fs->image, off + 26U, cluster);
    wr32(fs->image, off + 28U, size);
}

static void make_special_name(bkupfat_name_t *n, const char *s)
{
    u32 i;
    for(i = 0; i < 9U; i++) n->name[i] = 0;
    for(i = 0; i < 4U; i++) n->ext[i] = 0;
    for(i = 0; i < 8U && s[i]; i++) n->name[i] = s[i];
}

static void init_dir_cluster(bkupfat_t *fs, u16 cluster, u16 parent_cluster)
{
    bkupfat_name_t dot;
    bkupfat_name_t dotdot;
    u32 off = cluster_off(fs, cluster);
    mem_zero(&fs->image[off], cluster_size(fs));
    make_special_name(&dot, ".");
    make_special_name(&dotdot, "..");
    set_entry(fs, off, &dot, ATTR_DIR, cluster, 0);
    set_entry(fs, off + 32U, &dotdot, ATTR_DIR, parent_cluster, 0);
}

static int dir_find_entry(bkupfat_t *fs, const bkupfat_dir_t *dir, const bkupfat_name_t *n, u32 *entry_out)
{
    if(dir->is_root) {
        u32 p;
        for(p = root_off(fs); p < root_off(fs) + root_size(fs); p += 32U) {
            if(fs->image[p] == 0x00U || fs->image[p] == 0xE5U) continue;
            if(entry_matches(fs, p, n)) { *entry_out = p; return BKUPFAT_OK; }
        }
        return BKUPFAT_ERR_NOT_FOUND;
    } else {
        u16 c = dir->start_cluster;
        u32 guard = 0;
        if(!valid_cluster(fs, c)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        while(valid_cluster(fs, c)) {
            u32 off = cluster_off(fs, c);
            u32 p;
            for(p = off; p < off + cluster_size(fs); p += 32U) {
                if(fs->image[p] == 0x00U || fs->image[p] == 0xE5U) continue;
                if(entry_matches(fs, p, n)) { *entry_out = p; return BKUPFAT_OK; }
            }
            guard++;
            if(guard > (u32)(max_cluster(fs) - 1U)) return BKUPFAT_ERR_CORRUPT_CHAIN;
            c = fat_get(fs, c);
            if(is_eoc(c)) return BKUPFAT_ERR_NOT_FOUND;
            if(!valid_cluster(fs, c)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        }
    }
    return BKUPFAT_ERR_NOT_FOUND;
}

static int dir_has_free_entry(bkupfat_t *fs, const bkupfat_dir_t *dir, int *has_free, int *needs_expand)
{
    *has_free = 0;
    *needs_expand = 0;
    if(dir->is_root) {
        u32 p;
        for(p = root_off(fs); p < root_off(fs) + root_size(fs); p += 32U) {
            if(fs->image[p] == 0x00U || fs->image[p] == 0xE5U) { *has_free = 1; return BKUPFAT_OK; }
        }
        return BKUPFAT_ERR_DIR_FULL;
    } else {
        u16 c = dir->start_cluster;
        u32 guard = 0;
        if(!valid_cluster(fs, c)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        while(valid_cluster(fs, c)) {
            u32 off = cluster_off(fs, c);
            u32 p;
            for(p = off; p < off + cluster_size(fs); p += 32U) {
                if(fs->image[p] == 0x00U || fs->image[p] == 0xE5U) { *has_free = 1; return BKUPFAT_OK; }
            }
            guard++;
            if(guard > (u32)(max_cluster(fs) - 1U)) return BKUPFAT_ERR_CORRUPT_CHAIN;
            c = fat_get(fs, c);
            if(is_eoc(c)) { *needs_expand = 1; return BKUPFAT_OK; }
            if(!valid_cluster(fs, c)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        }
    }
    return BKUPFAT_ERR_CORRUPT_CHAIN;
}

static int append_dir_cluster(bkupfat_t *fs, bkupfat_dir_t *dir)
{
    u16 tail;
    u16 newc;
    int rc;
    if(dir->is_root) return BKUPFAT_ERR_DIR_FULL;
    tail = dir->start_cluster;
    if(!valid_cluster(fs, tail)) return BKUPFAT_ERR_CORRUPT_CHAIN;
    while(valid_cluster(fs, fat_get(fs, tail)) && !is_eoc(fat_get(fs, tail))) tail = fat_get(fs, tail);
    if(!is_eoc(fat_get(fs, tail))) return BKUPFAT_ERR_CORRUPT_CHAIN;
    rc = alloc_chain(fs, 1U, &newc);
    if(rc != BKUPFAT_OK) return rc;
    fat_set(fs, tail, newc);
    fat_set(fs, newc, FAT12_EOC);
    mem_zero(&fs->image[cluster_off(fs, newc)], cluster_size(fs));
    return BKUPFAT_OK;
}

static int dir_find_free_entry_mut(bkupfat_t *fs, bkupfat_dir_t *dir, u32 *entry_out)
{
    if(dir->is_root) {
        u32 p;
        for(p = root_off(fs); p < root_off(fs) + root_size(fs); p += 32U) {
            if(fs->image[p] == 0x00U || fs->image[p] == 0xE5U) { *entry_out = p; return BKUPFAT_OK; }
        }
        return BKUPFAT_ERR_DIR_FULL;
    } else {
        u16 c = dir->start_cluster;
        u32 guard = 0;
        while(valid_cluster(fs, c)) {
            u32 off = cluster_off(fs, c);
            u32 p;
            for(p = off; p < off + cluster_size(fs); p += 32U) {
                if(fs->image[p] == 0x00U || fs->image[p] == 0xE5U) { *entry_out = p; return BKUPFAT_OK; }
            }
            guard++;
            if(guard > (u32)(max_cluster(fs) - 1U)) return BKUPFAT_ERR_CORRUPT_CHAIN;
            if(is_eoc(fat_get(fs, c))) break;
            c = fat_get(fs, c);
        }
        {
            int rc = append_dir_cluster(fs, dir);
            if(rc != BKUPFAT_OK) return rc;
        }
        c = dir->start_cluster;
        while(valid_cluster(fs, fat_get(fs, c)) && !is_eoc(fat_get(fs, c))) c = fat_get(fs, c);
        if(!valid_cluster(fs, c)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        *entry_out = cluster_off(fs, c);
        return BKUPFAT_OK;
    }
}

static int resolve_parent(bkupfat_t *fs, const char *path, bkupfat_dir_t *parent, bkupfat_name_t *leaf)
{
    const char *p;
    bkupfat_dir_t cur;
    int last;
    int rc;
    bkupfat_name_t n;
    if(!fs || !path || !parent || !leaf) return BKUPFAT_ERR_INVALID_ARGUMENT;
    if(path[0] == 0) return BKUPFAT_ERR_INVALID_NAME;
    p = path;
    cur.is_root = 1;
    cur.start_cluster = 0;
    cur.parent_cluster = 0;
    while(*p == '/') p++;
    if(*p == 0) return BKUPFAT_ERR_INVALID_NAME;
    p = path;
    while(1) {
        u32 entry;
        u8 attr;
        u16 c;
        rc = next_component(&p, &n, &last);
        if(rc != BKUPFAT_OK) return rc;
        if(last) {
            *parent = cur;
            *leaf = n;
            return BKUPFAT_OK;
        }
        rc = dir_find_entry(fs, &cur, &n, &entry);
        if(rc != BKUPFAT_OK) return rc;
        attr = fs->image[entry + 11U];
        if(!(attr & ATTR_DIR)) return BKUPFAT_ERR_NOT_DIRECTORY;
        c = rd16(fs->image, entry + 26U);
        if(!valid_cluster(fs, c)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        cur.is_root = 0;
        cur.parent_cluster = 0;
        cur.start_cluster = c;
    }
}

static int resolve_entry(bkupfat_t *fs, const char *path, u32 *entry_out, bkupfat_dir_t *parent_out)
{
    bkupfat_dir_t parent;
    bkupfat_name_t leaf;
    int rc = resolve_parent(fs, path, &parent, &leaf);
    if(rc != BKUPFAT_OK) return rc;
    rc = dir_find_entry(fs, &parent, &leaf, entry_out);
    if(rc != BKUPFAT_OK) return rc;
    if(parent_out) *parent_out = parent;
    return BKUPFAT_OK;
}

int bkupfat_exists(bkupfat_t *fs, const char *path, u8 *attr_out, u32 *size_out)
{
    u32 entry;
    int rc;
    if(!fs || !fs->image || !path) return BKUPFAT_ERR_INVALID_ARGUMENT;
    if(path[0] == '/' && path[1] == 0) {
        if(attr_out) *attr_out = ATTR_DIR;
        if(size_out) *size_out = 0;
        return BKUPFAT_OK;
    }
    rc = resolve_entry(fs, path, &entry, 0);
    if(rc != BKUPFAT_OK) return rc;
    if(attr_out) *attr_out = fs->image[entry + 11U];
    if(size_out) *size_out = rd32(fs->image, entry + 28U);
    return BKUPFAT_OK;
}

int bkupfat_create_folder(bkupfat_t *fs, const char *path)
{
    bkupfat_dir_t parent;
    bkupfat_name_t leaf;
    u32 existing;
    int rc;
    int has_free;
    int needs_expand;
    u32 free_clusters;
    u16 dir_cluster;
    u32 entry;
    if(!fs || !fs->image || !path) return BKUPFAT_ERR_INVALID_ARGUMENT;
    rc = resolve_parent(fs, path, &parent, &leaf);
    if(rc != BKUPFAT_OK) return rc;
    rc = dir_find_entry(fs, &parent, &leaf, &existing);
    if(rc == BKUPFAT_OK) {
        if(fs->image[existing + 11U] & ATTR_DIR) return BKUPFAT_ERR_ALREADY_EXISTS;
        return BKUPFAT_ERR_ALREADY_EXISTS;
    }
    if(rc != BKUPFAT_ERR_NOT_FOUND) return rc;
    rc = dir_has_free_entry(fs, &parent, &has_free, &needs_expand);
    if(rc != BKUPFAT_OK) return rc;
    free_clusters = bkupfat_free_clusters(fs);
    if(free_clusters < (1U + (needs_expand ? 1U : 0U))) return BKUPFAT_ERR_NO_SPACE;
    rc = alloc_chain(fs, 1U, &dir_cluster);
    if(rc != BKUPFAT_OK) return rc;
    if(needs_expand) {
        rc = append_dir_cluster(fs, &parent);
        if(rc != BKUPFAT_OK) { chain_free(fs, dir_cluster); return rc; }
    }
    rc = dir_find_free_entry_mut(fs, &parent, &entry);
    if(rc != BKUPFAT_OK) { chain_free(fs, dir_cluster); return rc; }
    init_dir_cluster(fs, dir_cluster, parent.is_root ? 0U : parent.start_cluster);
    set_entry(fs, entry, &leaf, ATTR_DIR, dir_cluster, 0);
    return BKUPFAT_OK;
}

int bkupfat_create_file(bkupfat_t *fs, const char *path, const void *data, u32 len, u32 flags)
{
    bkupfat_dir_t parent;
    bkupfat_name_t leaf;
    u32 existing = 0;
    int have_existing = 0;
    int rc;
    int has_free = 0;
    int needs_expand = 0;
    u32 old_clusters = 0;
    u16 old_start = 0;
    u32 new_clusters;
    u32 required_clusters;
    u32 free_clusters;
    u16 first;
    u32 entry;
    if(!fs || !fs->image || !path || (len && !data)) return BKUPFAT_ERR_INVALID_ARGUMENT;
    if(len > (u32)(max_cluster(fs) - 1U) * cluster_size(fs)) return BKUPFAT_ERR_FILE_TOO_LARGE;
    rc = resolve_parent(fs, path, &parent, &leaf);
    if(rc != BKUPFAT_OK) return rc;
    rc = dir_find_entry(fs, &parent, &leaf, &existing);
    if(rc == BKUPFAT_OK) {
        if(fs->image[existing + 11U] & ATTR_DIR) return BKUPFAT_ERR_IS_DIRECTORY;
        if(!(flags & BKUPFAT_WRITE_OVERWRITE)) return BKUPFAT_ERR_ALREADY_EXISTS;
        have_existing = 1;
        old_start = rd16(fs->image, existing + 26U);
        rc = chain_count(fs, old_start, &old_clusters);
        if(rc != BKUPFAT_OK) return rc;
    } else if(rc == BKUPFAT_ERR_NOT_FOUND) {
        rc = dir_has_free_entry(fs, &parent, &has_free, &needs_expand);
        if(rc != BKUPFAT_OK) return rc;
    } else return rc;

    new_clusters = bkupfat_required_clusters(len);
    required_clusters = new_clusters + ((!have_existing && needs_expand) ? 1U : 0U);
    free_clusters = bkupfat_free_clusters(fs) + old_clusters;
    if(free_clusters < required_clusters) return BKUPFAT_ERR_NO_SPACE;

    if(have_existing) {
        chain_free(fs, old_start);
        entry = existing;
    } else {
        if(needs_expand) {
            rc = append_dir_cluster(fs, &parent);
            if(rc != BKUPFAT_OK) return rc;
        }
        rc = dir_find_free_entry_mut(fs, &parent, &entry);
        if(rc != BKUPFAT_OK) return rc;
    }
    rc = alloc_chain(fs, new_clusters, &first);
    if(rc != BKUPFAT_OK) return rc;
    if(len != 0) {
        const u8 *src = (const u8 *)data;
        u32 left = len;
        u32 pos = 0;
        u16 c = first;
        while(left && valid_cluster(fs, c)) {
            u32 chunk = left < cluster_size(fs) ? left : cluster_size(fs);
            mem_copy(&fs->image[cluster_off(fs, c)], src + pos, chunk);
            pos += chunk;
            left -= chunk;
            if(left == 0) break;
            c = fat_get(fs, c);
        }
    }
    set_entry(fs, entry, &leaf, ATTR_ARCHIVE, first, len);
    return BKUPFAT_OK;
}

int bkupfat_read_file(bkupfat_t *fs, const char *path, void *out, u32 out_capacity, u32 *len_out)
{
    u32 entry;
    u8 attr;
    u16 c;
    u32 size;
    u32 copied = 0;
    u32 guard = 0;
    u8 *dst = (u8 *)out;
    int rc;
    if(!fs || !fs->image || !path || !len_out) return BKUPFAT_ERR_INVALID_ARGUMENT;
    rc = resolve_entry(fs, path, &entry, 0);
    if(rc != BKUPFAT_OK) return rc;
    attr = fs->image[entry + 11U];
    if(attr & ATTR_DIR) return BKUPFAT_ERR_IS_DIRECTORY;
    size = rd32(fs->image, entry + 28U);
    *len_out = size;
    if(size > out_capacity) return BKUPFAT_ERR_BUFFER_TOO_SMALL;
    if(size != 0 && !out) return BKUPFAT_ERR_INVALID_ARGUMENT;
    c = rd16(fs->image, entry + 26U);
    if(size == 0) return BKUPFAT_OK;
    if(!valid_cluster(fs, c)) return BKUPFAT_ERR_CORRUPT_CHAIN;
    while(copied < size) {
        u32 chunk;
        if(!valid_cluster(fs, c)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        chunk = size - copied;
        if(chunk > cluster_size(fs)) chunk = cluster_size(fs);
        mem_copy(dst + copied, &fs->image[cluster_off(fs, c)], chunk);
        copied += chunk;
        if(copied >= size) break;
        guard++;
        if(guard > (u32)(max_cluster(fs) - 1U)) return BKUPFAT_ERR_CORRUPT_CHAIN;
        c = fat_get(fs, c);
        if(is_eoc(c)) return BKUPFAT_ERR_CORRUPT_CHAIN;
    }
    return BKUPFAT_OK;
}
