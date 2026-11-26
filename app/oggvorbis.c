/*
 * Ogg Vorbis files (oggvorbis.h): Ogg pages (ogg.c) of Vorbis packets,
 * decoded by Tremor (lib/tremor, Xiph's fixed-point decoder, BSD licence).
 *
 * Three header packets (identification, comments, setup), then audio.
 * Each audio packet finishes the samples its window overlaps with the one
 * before, so after a restart the first packet gives nothing. Positions
 * ("granule positions") count samples from the start; the last page's
 * says where the music ends.
 *
 * Tremor's samples are fixed point with 1.0 at 2^24: they go out as 24
 * bits. Its memory comes from an arena here, emptied for each file.
 *
 * Source: the Vorbis I specification (Xiph.Org), RFC 3533.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "oggvorbis.h"
#include "ogg.h"
#include "ivorbiscodec.h"
#include "los_sys.h"

/* ---- Tremor's memory ---- */

#define ARENA_BYTES (512 * 1024)
static size_t arena_used;
static bool arena_full;

/* One file plays at a time: its state (and Tremor's memory) laid over the
 * audio thread's decoder memory (source.h) */
struct vorbis_state {
    uint64_t arena[ARENA_BYTES / 8];
    ogg_reader rd;
    uint8_t packet[OGG_PACKET_MAX];
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    long packetno;
    ogg_int32_t **pcm;
    int pcm_n, pcm_used;
    int64_t at;                     /* granule position of pcm[.][0] */
    int64_t skip_until;
    int64_t end;
    uint64_t audio_start;
};
_Static_assert(sizeof(struct vorbis_state) <= DECODER_MEM_BYTES, "Vorbis fits");
#define ov (*(struct vorbis_state *)decoder_mem)

void *tremor_malloc(size_t n);
void *tremor_calloc(size_t count, size_t n);
void *tremor_realloc(void *p, size_t n);
void tremor_free(void *p);

/* Each block: its size, then the data, 8-byte aligned */
void *tremor_malloc(size_t n)
{
    size_t need = 8 + ((n + 7) & ~(size_t)7);
    if (arena_used + need > sizeof(ov.arena)) {
        arena_full = true;
        return NULL;
    }
    uint64_t *b = ov.arena + arena_used / 8;
    b[0] = n;
    arena_used += need;
    return b + 1;
}

void *tremor_calloc(size_t count, size_t n)
{
    if (n && count > SIZE_MAX / n)
        return NULL;
    void *p = tremor_malloc(count * n);
    if (p)
        memset(p, 0, count * n);
    return p;
}

void *tremor_realloc(void *p, size_t n)
{
    if (!p)
        return tremor_malloc(n);
    size_t old = (size_t)((uint64_t *)p)[-1];
    if (n <= old)
        return p;
    void *q = tremor_malloc(n);
    if (q)
        memcpy(q, p, old);
    return q;
}

void tremor_free(void *p)
{
    (void)p;                        /* all of it goes when the file closes */
}

/* ---- the file ---- */


static void as_packet(ogg_packet *op, bool first)
{
    memset(op, 0, sizeof(*op));
    op->packet = ov.packet;
    op->bytes = (long)ov.rd.len;
    op->b_o_s = first;
    op->granulepos = -1;
    op->packetno = ov.packetno++;
}

int oggvorbis_open(los_source *s)
{
    int64_t g = -1;
    ogg_packet op;
    arena_used = 0;
    arena_full = false;
    memset(&ov.vi, 0, sizeof(ov.vi));
    ogg_open(&ov.rd, s->file, ov.packet);
    vorbis_info_init(&ov.vi);
    vorbis_comment_init(&ov.vc);
    ov.packetno = 0;
    for (int i = 0; i < 3; i++) {
        if (!ogg_next(&ov.rd, &g))
            return -1;
        as_packet(&op, i == 0);
        if (vorbis_synthesis_headerin(&ov.vi, &ov.vc, &op))
            return i == 0 ? -2 : -1;       /* not Vorbis, or damaged */
    }
    if (ov.vi.channels < 1 || ov.vi.channels > 2 || ov.vi.rate <= 0)
        return -2;
    /* the setup header ends a page: audio starts on the next */
    while (g < 0 && ogg_next(&ov.rd, &g)) {
    }
    ov.audio_start = ov.rd.pos;
    if (vorbis_synthesis_init(&ov.vd, &ov.vi) || vorbis_block_init(&ov.vd, &ov.vb) || arena_full)
        return -1;
    ov.end = ogg_last_granule(&ov.rd);
    ov.rd.pos = ov.audio_start;
    ov.rd.nseg = ov.rd.seg = 0;
    ov.at = 0;
    ov.pcm_n = ov.pcm_used = 0;
    ov.skip_until = 0;

    s->rate = (unsigned)ov.vi.rate;
    s->bits = 24;
    s->channels = (unsigned)ov.vi.channels;
    s->frames = ov.end > 0 ? (uint64_t)ov.end : 0;
    s->format = "OGG";
    return 0;
}

/* More samples from the decoder, trimmed; false at the end */
static bool more(void)
{
    for (;;) {
        if (ov.pcm_n) {
            vorbis_synthesis_read(&ov.vd, ov.pcm_n);
            ov.at += ov.pcm_n;
            ov.pcm_n = ov.pcm_used = 0;
        }
        int n = vorbis_synthesis_pcmout(&ov.vd, &ov.pcm);
        if (n > 0) {
            int keep = n;
            if (ov.end >= 0 && ov.at + n > ov.end)
                keep = ov.at < ov.end ? (int)(ov.end - ov.at) : 0;
            if (keep <= 0) {
                vorbis_synthesis_read(&ov.vd, n);
                return false;               /* past the end */
            }
            /* at the end only 'keep' are music; the rest is never read */
            ov.pcm_n = keep;
            if (ov.at + keep <= ov.skip_until)
                continue;                   /* all before where we start */
            ov.pcm_used = ov.at < ov.skip_until ? (int)(ov.skip_until - ov.at) : 0;
            return true;
        }
        int64_t g;
        ogg_packet op;
        if (!ogg_next(&ov.rd, &g))
            return false;
        as_packet(&op, false);
        if (vorbis_synthesis(&ov.vb, &op) == 0)
            vorbis_synthesis_blockin(&ov.vd, &ov.vb);
    }
}

static inline int32_t to24(ogg_int32_t v)
{
    v >>= 1;                                /* 1.0 at 2^23 */
    return v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : v;
}

int oggvorbis_read(los_source *s, int32_t *out, int frames)
{
    int done = 0, ch = (int)s->channels;
    while (done < frames) {
        if (ov.pcm_used >= ov.pcm_n && !more())
            break;
        int take = ov.pcm_n - ov.pcm_used;
        if (take > frames - done)
            take = frames - done;
        for (int i = 0; i < take; i++)
            for (int c = 0; c < ch; c++)
                out[(done + i) * ch + c] = to24(ov.pcm[c][ov.pcm_used + i]);
        ov.pcm_used += take;
        done += take;
    }
    return done;
}

/* After a restart at 'from': the position of the first sample out. The
 * decoder runs to the next page that gives a position, counting what
 * comes out (the position is that of the last sample out). -1 if none. */
static int64_t first_out(uint64_t from)
{
    int64_t out = 0, g = -1;
    ogg_packet op;
    ogg_int32_t **pcm;
    if (from == ov.audio_start)
        return 0;
    ov.rd.pos = from;
    ov.rd.nseg = ov.rd.seg = 0;
    ov.rd.drop = true;
    vorbis_synthesis_restart(&ov.vd);
    while (ogg_next(&ov.rd, &g)) {
        as_packet(&op, false);
        if (vorbis_synthesis(&ov.vb, &op) == 0)
            vorbis_synthesis_blockin(&ov.vd, &ov.vb);
        int n;
        while ((n = vorbis_synthesis_pcmout(&ov.vd, &pcm)) > 0) {
            out += n;
            vorbis_synthesis_read(&ov.vd, n);
        }
        if (g >= 0)
            return g - out;
    }
    return -1;
}

bool oggvorbis_seek(los_source *s, uint64_t frame)
{
    (void)s;
    int64_t target = (int64_t)frame, pos = -1;
    /* the output starts a packet or two after a page boundary: start far
     * enough back that it starts before the target */
    int64_t margin = 2 * vorbis_info_blocksize(&ov.vi, 1);
    uint64_t from = ov.audio_start;
    for (int tries = 1; tries <= 4; tries++) {
        from = ogg_seek_from(&ov.rd, ov.audio_start, target - margin * tries);
        pos = first_out(from);
        if (pos >= 0 && pos <= target)
            break;
    }
    if (pos < 0 || pos > target) {
        from = ov.audio_start;                  /* from the top, then */
        pos = 0;
    }
    ov.rd.pos = from;
    ov.rd.nseg = ov.rd.seg = 0;
    ov.rd.drop = from != ov.audio_start;
    vorbis_synthesis_restart(&ov.vd);
    ov.pcm_n = ov.pcm_used = 0;
    ov.at = pos;
    ov.skip_until = target;
    return true;
}

void oggvorbis_close(los_source *s)
{
    (void)s;
    /* Tremor's own clearing only frees, and its memory is the arena:
     * emptying that is all (and safe after an open that failed part way) */
    arena_used = 0;
}

/* ---- for the library: our own header reading, no decoder ---- */

static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

bool oggvorbis_info(const char *path, unsigned *rate, unsigned *channels, uint64_t *frames,
                    void (*tag)(const char *comment, void *ctx), void *ctx)
{
    struct info_mem { ogg_reader rd; uint8_t packet[OGG_PACKET_MAX]; } *m = ogg_info_mem();
    ogg_reader *const rdp = &m->rd;
    uint8_t *const packet = m->packet;
#define rd (*rdp)
    void *f = los_file_open(path);
    if (!f)
        return false;
    int64_t g;
    bool ok = false;
    ogg_open(&rd, f, packet);
    /* identification: 1 "vorbis", version, channels, rate */
    if (ogg_next(&rd, &g) && rd.len >= 30 && packet[0] == 1 && !memcmp(packet + 1, "vorbis", 6)) {
        *channels = packet[11];
        *rate = le32(packet + 12);
        ok = *rate > 0;
        /* comments: 3 "vorbis", vendor, count, "KEY=value"s */
        if (ok && tag && ogg_next(&rd, &g) && rd.len >= 15 && packet[0] == 3 &&
            !memcmp(packet + 1, "vorbis", 6)) {
            const uint8_t *p = packet + 7, *e = packet + rd.len;
            uint32_t vendor = le32(p);
            p += 4;
            if (vendor <= (uint32_t)(e - p)) {
                p += vendor;
                uint32_t count = e - p >= 4 ? le32(p) : 0;
                p += 4;
                char c[256];
                for (uint32_t i = 0; i < count && i < 64 && e - p >= 4; i++) {
                    uint32_t n = le32(p);
                    p += 4;
                    if (n > (uint32_t)(e - p))
                        break;
                    if (n < sizeof(c)) {
                        memcpy(c, p, n);
                        c[n] = '\0';
                        tag(c, ctx);
                    }
                    p += n;
                }
            }
        }
        int64_t end = ogg_last_granule(&rd);
        *frames = end > 0 ? (uint64_t)end : 0;
    }
    los_file_close(f);
    return ok;
#undef rd
}
