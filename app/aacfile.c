/*
 * AAC files (aacfile.h): the M4A's packets (mp4.c) handed to Android's
 * fixed-point decoder (lib/pvaac, Apache-2.0) one at a time.
 *
 * Every packet decodes to 1024 frames. The encoder puts some frames in
 * front (its delay: 2112 for Apple's) and pads the end to whole packets;
 * the file says how many (iTunSMPB, or the edit list), and both are cut so
 * albums play gapless. A packet's sound overlaps the one before it, so a
 * seek decodes the packet before the one wanted first.
 *
 * Sources: ISO/IEC 14496-3 (AudioSpecificConfig, 1.6.2.1; the 1024-frame
 * packets of AAC LC), Apple's iTunSMPB layout (mp4.c).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "aacfile.h"
#ifndef OSCL_IMPORT_REF
#define OSCL_IMPORT_REF                   /* what Android's build defined: nothing */
#endif
#include "pvmp4audiodecoder_api.h"
#include "los_sys.h"

#define AAC_FRAMES   1024
#define PACKET_MAX   8192             /* 6144 bits a channel at most: two channels, and room */

/* One file at a time: the decoder's state and buffers in decoder_mem */
static struct {
    tPVMP4AudioDecoderExternal ext;
    void *mem;                        /* the decoder's own */
    int16_t *pcm;                     /* a packet decoded: 1024 frames, 2 channels (and room) */
    uint8_t *packet;
    mp4_cursor cursor;
    uint32_t next;                    /* the next packet to decode */
    unsigned have, used;              /* frames in pcm, frames given */
    uint64_t skip;                    /* the delay still to drop */
    uint64_t left;                    /* music still to give */
    uint64_t music;                   /* all of it */
} a;

bool aac_config(const uint8_t *asc, unsigned len, unsigned *object, unsigned *rate,
                unsigned *channels)
{
    static const unsigned rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000, 7350 };
    if (len < 2)
        return false;
    uint64_t bits = 0;                /* the first 8 bytes, from the top */
    for (unsigned i = 0; i < 8; i++)
        bits = bits << 8 | (i < len ? asc[i] : 0);
    unsigned at = 0;
#define TAKE(n) (at += (n), (unsigned)((bits >> (64 - at)) & ((1u << (n)) - 1)))
    unsigned obj = TAKE(5);
    if (obj == 31)                    /* an escape: 6 more bits */
        obj = 32 + TAKE(6);
    unsigned index = TAKE(4);
    if (index >= 13)                  /* 15: the rate itself; not used by music */
        return false;
    unsigned ch = TAKE(4);
    /* plain AAC that says, after its own config (3 bits), that it's
     * HE-AAC after all: the sync word 0x2b7, object type 5, SBR present
     * (backward-compatible signalling) */
    if (obj == 2 && len >= 5) {
        (void)TAKE(3);                /* its own config: frame length, core coder, extension */
        if (TAKE(11) == 0x2b7 && TAKE(5) == 5 && TAKE(1))
            obj = 5;
    }
#undef TAKE
    *object = obj;
    *rate = rates[index];
    *channels = ch;
    return ch >= 1 && ch <= 2;        /* mono and stereo */
}

uint64_t aac_music_frames(const mp4_info *m)
{
    if (m->valid && m->valid + m->delay <= m->frames)
        return m->valid;
    return m->frames > m->delay ? m->frames - m->delay : 0;
}

/* The next packet decoded into a.pcm; false at the end. A damaged one is
 * silence, so the music keeps its place. */
static bool decode_next(los_source *s)
{
    uint64_t at;
    uint32_t size;
    if (!mp4_packet(s->file, &s->mp4, &a.cursor, a.next, &at, &size))
        return false;
    a.next++;
    int err = -1;
    if (size <= PACKET_MAX && los_file_seek(s->file, at) &&
        los_file_read(s->file, a.packet, size) == size) {
        a.ext.pInputBuffer = a.packet;
        a.ext.inputBufferCurrentLength = (Int)size;
        a.ext.inputBufferMaxLength = 0;
        a.ext.inputBufferUsedLength = 0;
        a.ext.remainderBits = 0;
        a.ext.pOutputBuffer = a.pcm;
        a.ext.pOutputBuffer_plus = a.pcm + 2 * AAC_FRAMES;
        a.ext.repositionFlag = false;
        err = PVMP4AudioDecodeFrame(&a.ext, a.mem);
    }
    if (err != MP4AUDEC_SUCCESS)
        memset(a.pcm, 0, 2 * AAC_FRAMES * sizeof(int16_t));
    a.have = AAC_FRAMES;
    a.used = 0;
    return true;
}

int aacfile_open(los_source *s)
{
    unsigned object, rate, channels;
    if (s->mp4.codec != MP4_AAC || !aac_config(s->mp4.asc, s->mp4.asc_len, &object, &rate, &channels))
        return -1;
    if (object != 2)
        return -2;                    /* AAC LC only (HE-AAC isn't played) */

    /* decoder_mem: the decoder's state, then a packet's sound, then the packet */
    uint32_t need = PVMP4AudioDecoderGetMemRequirements();
    size_t mem_bytes = (need + 15) & ~(size_t)15;
    size_t pcm_bytes = 4 * AAC_FRAMES * sizeof(int16_t);   /* two channels, and AAC+'s half */
    if (mem_bytes + pcm_bytes + PACKET_MAX > DECODER_MEM_BYTES)
        return -1;
    memset(&a, 0, sizeof(a));
    a.mem = decoder_mem;
    a.pcm = (int16_t *)((uint8_t *)decoder_mem + mem_bytes);
    a.packet = (uint8_t *)decoder_mem + mem_bytes + pcm_bytes;
    memset(a.mem, 0, need);

    a.ext.outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
    a.ext.aacPlusEnabled = 0;
    a.ext.desiredChannels = 2;        /* mono comes out on both */
    if (PVMP4AudioDecoderInitLibrary(&a.ext, a.mem) != 0)
        return -1;
    static uint8_t asc[16];
    memcpy(asc, s->mp4.asc, s->mp4.asc_len);
    a.ext.pInputBuffer = asc;
    a.ext.inputBufferCurrentLength = s->mp4.asc_len;
    a.ext.inputBufferMaxLength = 0;
    if (PVMP4AudioDecoderConfig(&a.ext, a.mem) != MP4AUDEC_SUCCESS)
        return -1;

    mp4_cursor_reset(&a.cursor);
    a.music = aac_music_frames(&s->mp4);
    a.left = a.music;
    a.skip = s->mp4.delay;
    s->rate = rate;
    s->bits = 16;
    s->channels = 2;
    s->frames = a.music;
    s->format = "AAC";
    return 0;
}

int aacfile_read(los_source *s, int32_t *out, int frames)
{
    int done = 0;
    while (done < frames && a.left) {
        if (a.used >= a.have && !decode_next(s))
            break;
        unsigned avail = a.have - a.used;
        if (a.skip) {                 /* the encoder's delay: not music */
            unsigned drop = a.skip < avail ? (unsigned)a.skip : avail;
            a.used += drop;
            a.skip -= drop;
            continue;
        }
        unsigned n = (unsigned)(frames - done) < avail ? (unsigned)(frames - done) : avail;
        if (n > a.left)
            n = (unsigned)a.left;
        const int16_t *p = a.pcm + 2 * a.used;
        for (unsigned i = 0; i < 2 * n; i++)
            out[2 * done + i] = p[i];
        a.used += n;
        a.left -= n;
        done += (int)n;
    }
    return done;
}

bool aacfile_seek(los_source *s, uint64_t frame)
{
    if (frame > a.music)
        return false;
    uint64_t target = frame + s->mp4.delay;
    uint32_t p = (uint32_t)(target / AAC_FRAMES);
    PVMP4AudioDecoderResetBuffer(a.mem);
    mp4_cursor_reset(&a.cursor);
    a.next = p ? p - 1 : 0;
    if (p && !decode_next(s))         /* the packet before: its overlap */
        return false;
    if (!decode_next(s))
        return frame == a.music;      /* the very end */
    a.used = (unsigned)(target - (uint64_t)p * AAC_FRAMES);
    a.skip = 0;
    a.left = a.music - frame;
    return true;
}

void aacfile_close(los_source *s)
{
    (void)s;
    a.left = 0;
}
