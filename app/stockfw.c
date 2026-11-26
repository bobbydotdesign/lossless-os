/*
 * The player's original firmware, read only (stockfw.h).
 *
 * Its system partition is UBI on the flash with UBIFS inside. Only reads,
 * through los_stock_read (the platform: flash blocks of that partition).
 * Formats, as their maintainers publish them (the Linux MTD project's UBI
 * and UBIFS documentation), and as seen in HiFi Walker's V2.3 image:
 *
 * UBI, big-endian. Each flash block (a "physical eraseblock") opens with
 * an erase counter header ("UBI#": the offsets of the volume header and
 * of the data, at 16 and 20) and a volume header ("UBI!": volume 8,
 * logical block number 12, sequence number 40). Both end in a CRC-32 at
 * 60. The data from the data offset to the block's end is one "logical
 * eraseblock" of a volume; where two flash blocks hold the same one, the
 * higher sequence number is the newer.
 *
 * UBIFS, little-endian. Nodes start on 8-byte boundaries inside logical
 * blocks: magic 0x06101831, CRC-32 of the rest, sequence number, length,
 * type. Logical block 0 holds the superblock node, 1 and 2 the master
 * node (the newest says where the index's root is). The index is a tree
 * of index nodes, each a list of (block, offset, length, key) for its
 * children, keys in order; keys are (inode number, kind << 29 | a hash
 * or block number): kind 0 an inode, 1 data, 2 a directory entry. A
 * directory's entries are all the keys (its number, 2 << 29 | anything);
 * a file's data, (its number, 1 << 29 | block), 4096 bytes a block,
 * compressed alone (LZO, zlib's DEFLATE, or none).
 *
 * The CRC-32s are the usual polynomial started at all ones with no final
 * inversion, the complement of zlib's crc32 (seen: every node in V2.3's
 * image checks out that way, and none the other).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "stockfw.h"
#include "lzo1x.h"
#include "inflate.h"
#include "los_sys.h"

#define MAX_BLOCKS    1024          /* flash blocks, and logical blocks (the partition has 512) */
#define MAX_NODE      8192          /* the biggest node read: a 4 KB block of data and its header */
#define MAX_READS     6000          /* an index walk takes about a hundred */
#define MAX_FOUND     1024          /* keys a range may hold: a directory's entries, a file's blocks */
#define MAX_PENDING   512           /* index nodes waiting to be read */
#define DATA_BLOCK    4096

#define UBI_EC_MAGIC  0x55424923u   /* "UBI#" */
#define UBI_VID_MAGIC 0x55424921u   /* "UBI!" */
#define UBI_USER_VOLS 0x7fffef00u   /* volume numbers from here are UBI's own */
#define NODE_MAGIC    0x06101831u

enum { NODE_INO = 0, NODE_DATA = 1, NODE_DENT = 2, NODE_SB = 6, NODE_MST = 7, NODE_IDX = 9 };
enum { KEY_INO = 0, KEY_DATA = 1, KEY_DENT = 2 };
enum { ITYPE_REG = 0, ITYPE_DIR = 1 };
enum { COMPR_NONE = 0, COMPR_LZO = 1, COMPR_ZLIB = 2 };

#define HCD_WRITE_RAM  0xfc4cu
#define HCD_LAUNCH_RAM 0xfc4eu

typedef struct { uint32_t a, b; } key;
typedef struct { uint32_t lnum, offs; key k; } found_t;

static struct {
    uint32_t block_size, blocks, data_off, leb_size, min_io;
    uint16_t leb_block[MAX_BLOCKS];      /* flash block + 1 of each logical block; 0 none */
    uint32_t lebs;                       /* logical blocks: the highest mapped + 1 */
    uint32_t reads;
    uint32_t root_lnum, root_offs;
    uint8_t node[MAX_NODE];
    uint32_t node_len;
    found_t found[MAX_FOUND];
    uint32_t nfound;
    const char *why;
} fs;

/* scratch for the volume scan: what each flash block's headers say */
static uint32_t blk_vol[MAX_BLOCKS], blk_lnum[MAX_BLOCKS];
static uint64_t blk_sqnum[MAX_BLOCKS];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

/* the usual CRC-32, started at all ones, not inverted at the end */
static uint32_t crc_raw(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
            table[i] = c;
        }
    uint32_t c = 0xffffffffu;
    while (n--)
        c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c;
}

static int key_cmp(key x, key y)
{
    if (x.a != y.a)
        return x.a < y.a ? -1 : 1;
    if (x.b != y.b)
        return x.b < y.b ? -1 : 1;
    return 0;
}

static key key_of(const uint8_t *p) { return (key){ le32(p), le32(p + 4) }; }

/* ---- UBI: which flash block holds each logical block of the UBIFS volume ---------- */

static bool read_block(uint32_t block, uint32_t offs, void *buf, uint32_t len)
{
    if (++fs.reads > MAX_READS * 8)
        return false;
    return los_stock_read(block, offs, buf, len);
}

static bool ubi_scan(void)
{
    uint8_t h[64];
    uint32_t vid_off = 0, good = 0;

    for (uint32_t b = 0; b < fs.blocks; b++) {
        blk_vol[b] = UINT32_MAX;
        if (!read_block(b, 0, h, sizeof(h)) || be32(h) != UBI_EC_MAGIC || crc_raw(h, 60) != be32(h + 60))
            continue;                               /* bad, erased, or not UBI */
        uint32_t vid = be32(h + 16), data = be32(h + 20);
        if (vid < 64 || vid % 8 || data < vid + 64 || data >= fs.block_size)
            continue;
        if (!vid_off) {
            vid_off = vid;
            fs.data_off = data;
            fs.leb_size = fs.block_size - data;
        } else if (vid != vid_off || data != fs.data_off) {
            continue;                               /* not the same UBI as the rest */
        }
        if (!read_block(b, vid, h, sizeof(h)) || be32(h) != UBI_VID_MAGIC || crc_raw(h, 60) != be32(h + 60))
            continue;                               /* no volume: free */
        blk_vol[b] = be32(h + 8);
        blk_lnum[b] = be32(h + 12);
        blk_sqnum[b] = be64(h + 40);
        good++;
    }
    if (!good) {
        fs.why = "no UBI on the flash";
        return false;
    }

    /* the volume whose logical block 0 opens with a UBIFS superblock */
    for (uint32_t b = 0; b < fs.blocks; b++) {
        if (blk_vol[b] >= UBI_USER_VOLS || blk_lnum[b] != 0)
            continue;
        uint32_t vol = blk_vol[b];
        memset(fs.leb_block, 0, sizeof(fs.leb_block));
        fs.lebs = 0;
        static uint64_t sq[MAX_BLOCKS];
        for (uint32_t c = 0; c < fs.blocks; c++) {
            uint32_t l = blk_lnum[c];
            if (blk_vol[c] != vol || l >= MAX_BLOCKS)
                continue;
            if (!fs.leb_block[l] || blk_sqnum[c] > sq[l]) {
                fs.leb_block[l] = (uint16_t)(c + 1);
                sq[l] = blk_sqnum[c];
            }
            if (l + 1 > fs.lebs)
                fs.lebs = l + 1;
        }
        uint8_t n[24 + 16];
        if (read_block(fs.leb_block[0] - 1, fs.data_off, n, sizeof(n)) &&
            le32(n) == NODE_MAGIC && n[20] == NODE_SB) {
            fs.min_io = le32(n + 24 + 8);
            if (le32(n + 24 + 12) != fs.leb_size || !fs.min_io || fs.min_io > fs.leb_size) {
                fs.why = "the file system's block size isn't UBI's";
                return false;
            }
            return true;
        }
    }
    fs.why = "no UBIFS volume";
    return false;
}

/* ---- UBIFS ---------------------------------------------------------------------------- */

/* The node at (lnum, offs) into fs.node, checked: magic, length, CRC, and
 * the type asked for (-1: any). Nodes never cross a logical block. */
static const uint8_t *node_at(uint32_t lnum, uint32_t offs, int type)
{
    if (++fs.reads > MAX_READS) {
        fs.why = "too many reads";
        return NULL;
    }
    if (lnum >= fs.lebs || !fs.leb_block[lnum] || offs % 8 || offs + 24 > fs.leb_size)
        return NULL;
    uint32_t b = fs.leb_block[lnum] - 1, at = fs.data_off + offs;
    if (!los_stock_read(b, at, fs.node, 24) || le32(fs.node) != NODE_MAGIC)
        return NULL;
    uint32_t len = le32(fs.node + 16);
    if (len < 24 || len > MAX_NODE || offs + len > fs.leb_size)
        return NULL;
    if (!los_stock_read(b, at + 24, fs.node + 24, len - 24))
        return NULL;
    if (crc_raw(fs.node + 8, len - 8) != le32(fs.node + 4))
        return NULL;
    if (type >= 0 && fs.node[20] != type)
        return NULL;
    fs.node_len = len;
    return fs.node;
}

/* The newest master node: written one to a write unit (min_io) from the
 * start of logical block 1, its copy in 2 */
static bool find_root(void)
{
    uint64_t best = 0;
    for (uint32_t lnum = 1; lnum <= 2 && !best; lnum++) {
        for (uint32_t offs = 0; offs + 24 <= fs.leb_size; offs += fs.min_io) {
            uint8_t h[4];
            if (lnum >= fs.lebs || !fs.leb_block[lnum] ||
                !los_stock_read(fs.leb_block[lnum] - 1, fs.data_off + offs, h, 4))
                break;
            if (le32(h) == 0xffffffffu)
                break;                              /* the rest is unwritten */
            const uint8_t *n = node_at(lnum, offs, NODE_MST);
            if (n && fs.node_len >= 60 && le64(n + 8) > best) {
                best = le64(n + 8);
                fs.root_lnum = le32(n + 48);
                fs.root_offs = le32(n + 52);
            }
        }
    }
    if (!best)
        fs.why = "no master node";
    return best != 0;
}

/* Every leaf key in lo..hi, by walking the index from its root: into
 * fs.found. A child covers the keys from its own to the next child's,
 * which may repeat its last key (entries whose hashes collide). */
static bool range(key lo, key hi)
{
    static struct { uint32_t lnum, offs; } pending[MAX_PENDING];
    uint32_t npending = 0;

    fs.nfound = 0;
    pending[npending].lnum = fs.root_lnum;
    pending[npending++].offs = fs.root_offs;
    while (npending) {
        npending--;
        const uint8_t *n = node_at(pending[npending].lnum, pending[npending].offs, NODE_IDX);
        if (!n) {
            if (!fs.why)
                fs.why = "an index node didn't read";
            return false;
        }
        uint32_t count = le16(n + 24), level = le16(n + 26);
        if (28 + count * 20 > fs.node_len) {
            fs.why = "an index node is short";
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *br = n + 28 + i * 20;
            key k = key_of(br + 12);
            if (key_cmp(k, hi) > 0)
                break;
            if (level == 0) {
                if (key_cmp(k, lo) < 0)
                    continue;
                if (fs.nfound == MAX_FOUND) {
                    fs.why = "too many entries";
                    return false;
                }
                fs.found[fs.nfound++] = (found_t){ le32(br), le32(br + 4), k };
            } else {
                if (i + 1 < count && key_cmp(key_of(br + 20 + 12), lo) < 0)
                    continue;
                if (npending == MAX_PENDING) {
                    fs.why = "the index is too deep";
                    return false;
                }
                pending[npending].lnum = le32(br);
                pending[npending++].offs = le32(br + 4);
            }
        }
    }
    return true;
}

/* A directory entry's name, type and inode number, from the node in fs.node */
static bool dent_parts(char *name, size_t n, uint8_t *type, uint32_t *inum)
{
    uint32_t nlen = le16(fs.node + 50);
    if (fs.node_len < 56 + nlen || nlen + 1 > n || le64(fs.node + 40) >> 32)
        return false;
    memcpy(name, fs.node + 56, nlen);
    name[nlen] = '\0';
    *type = fs.node[49];
    *inum = (uint32_t)le64(fs.node + 40);
    return true;
}

static bool patch_name(const char *name)
{
    size_t n = strlen(name);
    return n > 13 && !strncasecmp(name, "BCM4343A1", 9) && !strcasecmp(name + n - 4, ".hcd");
}

/* In directory dir: the entry called name (want_patch: any patch file's
 * name instead); its inode number, or 0 */
static uint32_t lookup(uint32_t dir, const char *name, bool want_patch, uint8_t want_type,
                       char *found_name, size_t found_n)
{
    if (!range((key){ dir, KEY_DENT << 29 }, (key){ dir, KEY_DENT << 29 | 0x1fffffffu }))
        return 0;
    static found_t entries[MAX_FOUND];
    uint32_t count = fs.nfound;
    memcpy(entries, fs.found, count * sizeof(found_t));
    for (uint32_t i = 0; i < count; i++) {
        char nm[256];
        uint8_t type;
        uint32_t inum;
        if (!node_at(entries[i].lnum, entries[i].offs, NODE_DENT) || !dent_parts(nm, sizeof(nm), &type, &inum))
            continue;
        if (type != want_type || !inum)
            continue;
        if (want_patch ? patch_name(nm) : !strcmp(nm, name)) {
            if (found_name)
                snprintf(found_name, found_n, "%s", nm);
            return inum;
        }
    }
    return 0;
}

/* A file's data node (in fs.node) into buf, at its block's place */
static bool place_data(uint8_t *buf, size_t size)
{
    if (fs.node_len < 48)
        return false;
    uint32_t block = le32(fs.node + 28) & 0x1fffffffu, len = le32(fs.node + 40);
    uint32_t compr = le16(fs.node + 44), stored = fs.node_len - 48;
    size_t at = (size_t)block * DATA_BLOCK;
    if (len > DATA_BLOCK || at >= size || len > size - at)
        return false;
    const uint8_t *src = fs.node + 48;
    if (compr == COMPR_NONE) {
        if (stored != len)
            return false;
        memcpy(buf + at, src, len);
        return true;
    }
    if (compr == COMPR_LZO) {
        size_t got;
        return lzo1x_decode(src, stored, buf + at, len, &got) == 0 && got == len;
    }
    if (compr == COMPR_ZLIB) {
        uint32_t got, used;
        return inflate_raw(src, stored, buf + at, len, &got, &used) == 0 && got == len;
    }
    return false;
}

/* A file by its inode number, through the index, into buf */
static size_t read_file(uint32_t inum, uint8_t *buf, size_t cap)
{
    if (!range((key){ inum, KEY_INO << 29 }, (key){ inum, KEY_INO << 29 }) || fs.nfound != 1 ||
        !node_at(fs.found[0].lnum, fs.found[0].offs, NODE_INO) || fs.node_len < 56) {
        fs.why = "the file's inode didn't read";
        return 0;
    }
    uint64_t size = le64(fs.node + 48);
    if (size == 0 || size > cap) {
        fs.why = "the file's size is wrong";
        return 0;
    }
    memset(buf, 0, (size_t)size);                   /* blocks never written read as zeros */
    if (!range((key){ inum, KEY_DATA << 29 }, (key){ inum, KEY_DATA << 29 | 0x1fffffffu }))
        return 0;
    static found_t blocks[MAX_FOUND];
    uint32_t count = fs.nfound;
    memcpy(blocks, fs.found, count * sizeof(found_t));
    for (uint32_t i = 0; i < count; i++)
        if (!node_at(blocks[i].lnum, blocks[i].offs, NODE_DATA) || !place_data(buf, (size_t)size)) {
            fs.why = "a block of the file didn't read";
            return 0;
        }
    return (size_t)size;
}

/* Where the original firmwares keep it: /lib/firmware (H2 V2.3) */
static const char *const patch_dirs[][3] = {
    { "lib", "firmware", NULL },
    { "etc", "firmware", NULL },
    { "usr", "lib", "firmware" },
    { "system", "etc", "firmware" },
};

static size_t by_index(uint8_t *buf, size_t cap, char *note, size_t n)
{
    if (!find_root())
        return 0;
    for (size_t d = 0; d < sizeof(patch_dirs) / sizeof(patch_dirs[0]); d++) {
        uint32_t dir = 1;                           /* the root directory */
        for (int c = 0; c < 3 && patch_dirs[d][c] && dir; c++)
            dir = lookup(dir, patch_dirs[d][c], false, ITYPE_DIR, NULL, 0);
        if (!dir)
            continue;
        uint32_t inum = lookup(dir, NULL, true, ITYPE_REG, note, n);
        if (!inum)
            continue;
        size_t size = read_file(inum, buf, cap);
        if (size && stockfw_hcd_ok(buf, size))
            return size;
    }
    if (!fs.why)
        fs.why = "no patch file found";
    return 0;
}

/* ---- the whole partition, when the index doesn't lead to it ---------------------------- */

/* where a data node is: 16 bytes, so a whole partition's fit in a quarter megabyte */
typedef struct { uint32_t inum, offs, sqnum; uint16_t block, lnum; } data_ref;

static size_t by_scan(uint8_t *buf, size_t cap, char *note, size_t n)
{
    uint8_t *leb = malloc(fs.leb_size);
    size_t max_refs = 16384;
    data_ref *refs = malloc(max_refs * sizeof(data_ref));
    size_t nrefs = 0, size = 0;
    uint64_t dent_sq = 0, file_size = 0;
    uint32_t want = 0;

    if (!leb || !refs) {
        fs.why = "not enough memory";
        goto out;
    }
    /* one pass: the newest entry with a patch file's name, and where every
     * small file's first blocks are (the patch is under 64 KB) */
    for (uint32_t lnum = 0; lnum < fs.lebs; lnum++) {
        if (!fs.leb_block[lnum])
            continue;
        uint32_t b = fs.leb_block[lnum] - 1;
        bool ok = true;
        for (uint32_t at = 0; at < fs.leb_size && ok; at += 2048) {
            uint32_t len = fs.leb_size - at < 2048 ? fs.leb_size - at : 2048;
            ok = los_stock_read(b, fs.data_off + at, leb + at, len);
        }
        if (!ok)
            continue;
        for (uint32_t offs = 0; offs + 24 <= fs.leb_size;) {
            const uint8_t *p = leb + offs;
            uint32_t len = le32(p + 16);
            if (le32(p) != NODE_MAGIC || len < 24 || offs + len > fs.leb_size ||
                crc_raw(p + 8, len - 8) != le32(p + 4)) {
                offs += 8;
                continue;
            }
            uint64_t sq = le64(p + 8);
            if (p[20] == NODE_DENT && len >= 56) {
                uint32_t nlen = le16(p + 50);
                char nm[256];
                if (nlen < sizeof(nm) && 56 + nlen <= len && p[49] == ITYPE_REG && sq > dent_sq) {
                    memcpy(nm, p + 56, nlen);
                    nm[nlen] = '\0';
                    if (patch_name(nm) && !(le64(p + 40) >> 32)) {
                        dent_sq = sq;
                        want = (uint32_t)le64(p + 40);     /* 0: deleted */
                        snprintf(note, n, "%s", nm);
                    }
                }
            } else if (p[20] == NODE_DATA && len >= 48 && (le32(p + 28) & 0x1fffffffu) < 16 &&
                       nrefs < max_refs) {
                refs[nrefs++] = (data_ref){ le32(p + 24), offs, (uint32_t)sq,
                                            (uint16_t)(le32(p + 28) & 0x1fffffffu), (uint16_t)lnum };
            }
            offs += (len + 7) & ~7u;
        }
    }
    if (!want) {
        fs.why = "no patch file found";
        goto out;
    }
    /* each block's newest data node; the file ends where its last block
     * does (the patch check below catches anything cut short) */
    memset(buf, 0, cap);
    for (uint32_t blk = 0; blk < 16; blk++) {
        data_ref *best = NULL;
        for (size_t i = 0; i < nrefs; i++)
            if (refs[i].inum == want && refs[i].block == blk && (!best || refs[i].sqnum > best->sqnum))
                best = &refs[i];
        if (!best)
            continue;
        if (!node_at(best->lnum, best->offs, NODE_DATA) || !place_data(buf, cap)) {
            fs.why = "a block of the file didn't read";
            goto out;
        }
        file_size = (uint64_t)blk * DATA_BLOCK + le32(fs.node + 40);
    }
    if (file_size && stockfw_hcd_ok(buf, (size_t)file_size))
        size = (size_t)file_size;
    else
        fs.why = "the file isn't a patch";
out:
    free(leb);
    free(refs);
    return size;
}

/* ---- ------------------------------------------------------------------------------------ */

bool stockfw_hcd_ok(const uint8_t *p, size_t n)
{
    size_t at = 0, records = 0;
    uint16_t op = 0;
    if (n < 64)
        return false;
    while (at + 3 <= n) {
        op = le16(p + at);
        if (op != HCD_WRITE_RAM && op != HCD_LAUNCH_RAM)
            return false;
        at += 3u + p[at + 2];
        records++;
        if (op == HCD_LAUNCH_RAM)
            break;
    }
    return at == n && op == HCD_LAUNCH_RAM && records > 1;
}

size_t stockfw_bt_patch(uint8_t *buf, size_t cap, bool thorough, char *note, size_t n)
{
    memset(&fs, 0, sizeof(fs));
    note[0] = '\0';
    if (!los_stock_info(&fs.block_size, &fs.blocks) || !fs.blocks) {
        snprintf(note, n, "no original firmware on this player");
        return 0;
    }
    if (fs.blocks > MAX_BLOCKS)
        fs.blocks = MAX_BLOCKS;
    size_t size = 0;
    if (ubi_scan()) {
        size = by_index(buf, cap, note, n);
        if (!size && thorough) {
            const char *why = fs.why;
            fs.why = NULL;
            fs.reads = 0;
            size = by_scan(buf, cap, note, n);
            if (!size && why && fs.why) {
                char w[160];
                snprintf(w, sizeof(w), "%s; read through: %s", why, fs.why);
                snprintf(note, n, "%s", w);
                return 0;
            }
        }
    }
    if (!size)
        snprintf(note, n, "%s", fs.why ? fs.why : "not found");
    return size;
}
