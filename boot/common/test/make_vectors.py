#!/usr/bin/env python3
"""Test vectors for the gzip decompressor: FILE and FILE.gz pairs in DIR.
   make_vectors.py DIR
The original firmware's kernel is added if HiFi Walker's V2.3 update is in
../../../../backup/firmware (private, not in the repository).
SPDX-License-Identifier: Apache-2.0"""
import gzip, os, random, subprocess, sys
d = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
random.seed(7)
text = open(os.path.join(here, '..', 'inflate.c'), 'rb').read() * 40
cases = {
    'text': text,
    'random': bytes(random.getrandbits(8) for _ in range(300000)),
    'zeros': bytes(1000000),
    'mixed': b''.join((bytes(random.getrandbits(8) for _ in range(random.randint(1, 2000)))
                       if random.random() < .3 else text[:random.randint(1, 5000)]) for _ in range(400)),
    'tiny': b'a',
    'empty': b'',
}
for name, data in cases.items():
    for lvl in (0, 1, 6, 9):
        p = os.path.join(d, f'{name}-{lvl}')
        open(p, 'wb').write(data)
        open(p + '.gz', 'wb').write(gzip.compress(data, compresslevel=lvl))
upt = os.path.join(here, '..', '..', '..', '..', 'backup', 'firmware', 'H2-Firmware-V2.3.upt')
if os.path.exists(upt):
    subprocess.run(['bsdtar', '-xf', upt, '-C', d, 'uimage.bin'], check=True)
    k = open(os.path.join(d, 'uimage.bin'), 'rb').read()[64:]
    open(os.path.join(d, 'kernel.gz'), 'wb').write(k)
    open(os.path.join(d, 'kernel'), 'wb').write(gzip.decompress(k))
