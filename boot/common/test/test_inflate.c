/*
 * Host test for inflate.c: gunzip each FILE.gz and compare with FILE.
 *   test_inflate FILE ...
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../inflate.h"

static unsigned char *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	rewind(f);
	unsigned char *b = malloc(*len + 1);
	if (fread(b, 1, *len, f) != (size_t)*len) {
		fclose(f);
		return NULL;
	}
	fclose(f);
	return b;
}

int main(int argc, char **argv)
{
	int failed = 0;

	for (int i = 1; i < argc; i++) {
		char gz[1024];
		long glen, olen;
		snprintf(gz, sizeof(gz), "%s.gz", argv[i]);
		unsigned char *in = slurp(gz, &glen), *want = slurp(argv[i], &olen);
		if (!in || !want) {
			printf("%s: can't read\n", argv[i]);
			failed++;
			continue;
		}
		unsigned char *out = malloc(olen + 16);
		uint32_t got = 0;
		clock_t t0 = clock();
		int err = gunzip(in, (uint32_t)glen, out, (uint32_t)olen + 16, &got);
		double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
		int ok = !err && got == (uint32_t)olen && !memcmp(out, want, olen);
		printf("%-40s %s (err %d, %u of %ld bytes, %.1f ms)\n", argv[i], ok ? "ok" : "FAILED",
		       err, got, olen, ms);
		failed += !ok;
		/* and a damaged copy must be refused, never crash */
		if (glen > 64) {
			in[glen / 2] ^= 0x55;
			err = gunzip(in, (uint32_t)glen, out, (uint32_t)olen + 16, &got);
			/* accepted is fine only if the output is still right (e.g. a
			 * changed distance that copies the same bytes) */
			int same = !err && got == (uint32_t)olen && !memcmp(out, want, olen);
			printf("%-40s damaged copy: %s (err %d)\n", "",
			       err ? "refused" : same ? "same output" : "WRONG OUTPUT ACCEPTED", err);
			failed += !err && !same;
		}
		free(in); free(want); free(out);
	}
	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed != 0;
}
