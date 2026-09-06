#include "atarist_disk.h"
#include "atarist_os.h"
#include "atarist_audio.h"
#include "atarist_probe.h"

/* The largest stream tools/atarist/gen_atarist_audio.py emits is a shade under
 * 29 KB; the buffer is rounded up so a longer recording added later does not
 * silently fail to load. */
#define MUSIC_BUFFER_BYTES 32768

static uint8_t *g_music_buf;
static int8_t   g_music_loaded = -1;

/* In atarist_audio.h's enum order.  A track with no file is silence, not an
 * error: the floppy is allowed to ship without the whole soundtrack. */
static const char *const g_music_path[ATARIST_MUSIC_COUNT] = {
    0,
    "MUS\\TITLE.YMS",
    "MUS\\OVERWRLD.YMS",
    "MUS\\BATTLE.YMS",
    "MUS\\BOSS.YMS",
    "MUS\\FINALBOS.YMS",
    "MUS\\VICTORY.YMS",
    "MUS\\FAIL.YMS"
};

int32_t Atarist_DiskLoad(const char *path, void *buf, int32_t max)
{
    int16_t handle = st_fopen(path, 0);
    int32_t got;

    /* The last disk result is left in the probe on purpose: a blind run that
     * comes up silent needs to say whether the open failed or the read did. */
    g_atarist_probe.spare = (uint16_t)handle;
    if (handle < 0) return -1;
    got = st_fread(handle, max, buf);
    g_atarist_probe.spare = (uint16_t)got;
    st_fclose(handle);
    if (got < 0) return -1;
    return got;
}

int16_t Atarist_DiskOpen(const char *path)
{
    int16_t handle = st_fopen(path, 0);
    g_atarist_probe.spare = (uint16_t)handle;
    return handle;
}

int32_t Atarist_DiskReadAt(int16_t handle, int32_t offset, void *buf,
                           int32_t len)
{
    int32_t got;
    if (handle < 0) return -1;
    /* Mode 0 is "from the start of the file", which is the only seek this
     * needs: BIG.CRD's records are fixed size, so a face is offset * record. */
    if (st_fseek(offset, handle, 0) < 0) return -1;
    got = st_fread(handle, len, buf);
    g_atarist_probe.spare = (uint16_t)got;
    return got;
}

void Atarist_DiskClose(int16_t handle)
{
    if (handle >= 0) st_fclose(handle);
}

int32_t Atarist_DiskLoadAt(const char *path, int32_t offset, void *buf,
                           int32_t len)
{
    int16_t handle = Atarist_DiskOpen(path);
    int32_t got;

    if (handle < 0) return -1;
    got = Atarist_DiskReadAt(handle, offset, buf, len);
    Atarist_DiskClose(handle);
    return got;
}

int Atarist_DiskInit(void)
{
    if (!g_music_buf)
        g_music_buf = (uint8_t *)st_malloc(MUSIC_BUFFER_BYTES);
    return g_music_buf != 0;
}

int Atarist_MusicLoadTrack(int track)
{
    int32_t got;

    if (track <= 0 || track >= ATARIST_MUSIC_COUNT) return 0;
    if (!g_music_buf || !g_music_path[track]) return 0;
    if (g_music_loaded == (int8_t)track) {
        Atarist_MusicPlay(track);
        return 1;
    }

    /* THE PLAYER IS READING THIS BUFFER FROM THE VBL.  Reading a new track
     * over it while the old one is playing hands the interrupt a record that
     * is half one song and half another, and the register writes that come
     * out of that are exactly the ones that make a YM screech. */
    Atarist_MusicStop();
    g_music_loaded = -1;

    got = Atarist_DiskLoad(g_music_path[track], g_music_buf,
                           MUSIC_BUFFER_BYTES);
    if (got <= 0) {
        g_atarist_probe.status = ATARIST_PROBE_NOFILE;
        return 0;
    }
    Atarist_MusicSetStream(track, g_music_buf, got);
    g_music_loaded = (int8_t)track;
    Atarist_MusicPlay(track);
    return 1;
}
