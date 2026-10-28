/*
 * Opus files (opusfile.h): Ogg pages (ogg.c) of Opus packets, decoded by
 * libopus (lib/opus, BSD licence) built in fixed point: the players have
 * no floating-point unit.
 *
 * The first packet is OpusHead (channels, how many samples at the start
 * are only there to prime the decoder, the output gain), the second
 * OpusTags (Vorbis-style comments). Positions ("granule positions") count
 * samples at 48 kHz from the start, the primer included; the last page's
 * says where the music ends. Seeking starts 80 ms early, which the format
 * asks for so the decoder has settled by the place wanted.
 *
 * Source: RFC 7845 (Ogg encapsulation for Opus), RFC 6716.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "opusfile.h"
#include "ogg.h"
#include "opus.h"
#include "los_sys.h"

#define PREROLL 3840                 /* 80 ms at 48 kHz */
#define MAX_FRAME 5760               /* 120 ms: the longest packet */
#ifndef OPUS_DECODER_BYTES
#define OPUS_DECODER_BYTES 26600     /* opus_decoder_get_size(2), fixed point */
#endif

/* One Opus file plays at a time: its state is laid over the audio
 * thread's decoder memory (source.h) */
struct opus_state {
    ogg_reader rd;
    uint8_t packet[OGG_PACKET_MAX];
    int64_t decoder[(OPUS_DECODER_BYTES + 7) / 8];   /* aligned */
    int16_t pcm[MAX_FRAME * 2];
    int pcm_n, pcm_used;
    unsigned channels;
    int64_t pre_skip;
    int64_t at;                      /* granule position of pcm[0] */
    int64_t skip_until;              /* nothing before this granule position comes out */
    int64_t end;                     /* where the music ends (granule), -1 unknown */
    uint64_t audio_start;            /* the first page after the headers */
};
_Static_assert(sizeof(struct opus_state) <= DECODER_MEM_BYTES, "Opus fits");
#define op (*(struct opus_state *)decoder_mem)

static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

static OpusDecoder *dec(void) { return (OpusDecoder *)op.decoder; }

/* OpusHead: channels and the primer; false if it's not mono or stereo Opus */
static bool read_head(const uint8_t *p, size_t n, unsigned *channels, int64_t *pre_skip, int *gain)
{
    if (n < 19 || memcmp(p, "OpusHead", 8) || (p[8] & 0xf0) != 0)
        return false;
    *channels = p[9];
    *pre_skip = le16(p + 10);
    *gain = (int16_t)le16(p + 16);
    return (*channels == 1 || *channels == 2) && p[18] == 0;   /* mapping family 0 */
}

int opusfile_open(los_source *s)
{
    int64_t g;
    int gain;
    ogg_open(&op.rd, s->file, op.packet);
    if (!ogg_next(&op.rd, &g) || !read_head(op.packet, op.rd.len, &op.channels, &op.pre_skip, &gain))
        return -2;
    if (opus_decoder_get_size((int)op.channels) > (int)sizeof(op.decoder) ||
        opus_decoder_init(dec(), 48000, (int)op.channels) != OPUS_OK)
        return -2;
    if (gain)
        opus_decoder_ctl(dec(), OPUS_SET_GAIN(gain));
    if (!ogg_next(&op.rd, &g) || op.rd.len < 8 || memcmp(op.packet, "OpusTags", 8))
        return -1;
    /* the tags may run over several pages; audio starts on a fresh page */
    while (g < 0 && ogg_next(&op.rd, &g)) {
    }
    op.audio_start = op.rd.pos;
    op.end = ogg_last_granule(&op.rd);
    op.rd.pos = op.audio_start;
    op.rd.nseg = op.rd.seg = 0;
    op.at = 0;
    op.pcm_n = op.pcm_used = 0;
    op.skip_until = op.pre_skip;

    s->rate = 48000;
    s->bits = 16;
    s->channels = op.channels;
    s->frames = op.end > op.pre_skip ? (uint64_t)(op.end - op.pre_skip) : 0;
    s->format = "OPUS";
    return 0;
}

/* The next packet decoded into pcm, trimmed; false at the end */
static bool next_packet(void)
{
    int64_t g;
    for (;;) {
        op.at += op.pcm_n;
        op.pcm_n = op.pcm_used = 0;
        if (!ogg_next(&op.rd, &g))
            return false;
        int n = opus_decode(dec(), op.packet, (opus_int32)op.rd.len, op.pcm, MAX_FRAME, 0);
        if (n <= 0)
            continue;                   /* damaged: skip it */
        op.pcm_n = n;
        /* the end: the last page says where the music stops */
        if (op.end >= 0 && op.at + n > op.end)
            op.pcm_n = op.at < op.end ? (int)(op.end - op.at) : 0;
        if (op.at + op.pcm_n <= op.skip_until)
            continue;                   /* all of it before where we start */
        op.pcm_used = op.at < op.skip_until ? (int)(op.skip_until - op.at) : 0;
        return true;
    }
}

int opusfile_read(los_source *s, int32_t *out, int frames)
{
    int done = 0;
    while (done < frames) {
        if (op.pcm_used >= op.pcm_n && !next_packet())
            break;
        int take = op.pcm_n - op.pcm_used;
        if (take > frames - done)
            take = frames - done;
        const int16_t *p = op.pcm + op.pcm_used * (int)s->channels;
        int32_t *o = out + done * (int)s->channels;
        for (int i = 0; i < take * (int)s->channels; i++)
            o[i] = p[i];
        op.pcm_used += take;
        done += take;
    }
    return done;
}

/* Reading from 'from' (a page boundary): the position of the first whole
 * packet (one that began on the page before is dropped): the next page's
 * position less what its packets hold. -1 if none. */
static int64_t first_packet(uint64_t from)
{
    int64_t g = -1, held = 0;
    if (from == op.audio_start)
        return 0;
    op.rd.pos = from;
    op.rd.nseg = op.rd.seg = 0;
    op.rd.drop = true;
    while (ogg_next(&op.rd, &g)) {
        int n = opus_packet_get_nb_samples(op.packet, (opus_int32)op.rd.len, 48000);
        held += n > 0 ? n : 0;
        if (g >= 0)
            return g - held;
    }
    return -1;
}

bool opusfile_seek(los_source *s, uint64_t frame)
{
    (void)s;
    int64_t target = (int64_t)frame + op.pre_skip, pos = -1;
    uint64_t from = op.audio_start;
    /* decoding starts 80 ms before the target; the first whole packet can
     * be up to 120 ms after the page boundary */
    for (int tries = 1; tries <= 4; tries++) {
        from = ogg_seek_from(&op.rd, op.audio_start, target - PREROLL - MAX_FRAME * tries);
        pos = first_packet(from);
        if (pos >= 0 && pos <= target - PREROLL)
            break;
    }
    if (pos < 0 || pos > target - PREROLL) {
        from = op.audio_start;
        pos = 0;
    }
    op.rd.pos = from;
    op.rd.nseg = op.rd.seg = 0;
    op.rd.drop = from != op.audio_start;
    opus_decoder_ctl(dec(), OPUS_RESET_STATE);
    op.at = pos;
    op.pcm_n = op.pcm_used = 0;
    op.skip_until = target;
    return true;
}

void opusfile_close(los_source *s)
{
    (void)s;
}

/* ---- for the library ---- */

bool opusfile_info(const char *path, unsigned *rate, unsigned *channels, uint64_t *frames,
                   void (*tag)(const char *comment, void *ctx), void *ctx)
{
    struct info_mem { ogg_reader rd; uint8_t packet[OGG_PACKET_MAX]; } *m = ogg_info_mem();
    ogg_reader *const rdp = &m->rd;
    uint8_t *const packet = m->packet;
#define rd (*rdp)
    void *f = los_file_open(path);
    if (!f)
        return false;
    int64_t g, pre_skip;
    int gain;
    bool ok = false;
    ogg_open(&rd, f, packet);
    if (ogg_next(&rd, &g) && read_head(packet, rd.len, channels, &pre_skip, &gain)) {
        ok = true;
        *rate = 48000;
        if (tag && ogg_next(&rd, &g) && rd.len >= 16 && !memcmp(packet, "OpusTags", 8)) {
            const uint8_t *p = packet + 8, *e = packet + rd.len;
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
        *frames = end > pre_skip ? (uint64_t)(end - pre_skip) : 0;
    }
    los_file_close(f);
    return ok;
#undef rd
}
