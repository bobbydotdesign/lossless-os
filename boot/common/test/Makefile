# Host test for the gzip decompressor: make -C boot/common/test
# (test vectors from Python's gzip, and the original firmware's kernel)
# SPDX-License-Identifier: Apache-2.0
D ?= /tmp/lossless-inflate-test
test: test_inflate.c ../inflate.c ../inflate.h make_vectors.py
	mkdir -p $(D)
	python3 -I make_vectors.py $(D)
	$(CC) -O2 -Wall -Wextra -o $(D)/test_inflate test_inflate.c ../inflate.c
	cd $(D) && ./test_inflate $$(ls | grep -v -E '\.gz$$|test_inflate|uimage.bin')
.PHONY: test
