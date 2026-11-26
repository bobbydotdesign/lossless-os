# lbt host tests: make -C lib/lbt/test
#
# The SBC encoder and decoder: ../sbc (Android's, Apache-2.0, from Zephyr's libsbc).
CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined
SRC = ../hci.c ../l2cap.c ../gap.c ../sdp.c ../avdtp.c ../a2dp.c ../avrcp.c ../coverart.c fake.c chan.c

LIBSBC ?= ../sbc
SBC_SRC = $(wildcard $(LIBSBC)/encoder/srce/*.c) $(filter-out %readsamplesjoint.inc,$(wildcard $(LIBSBC)/decoder/srce/*.c))
SBC_INC = -I$(LIBSBC)/encoder/include -I$(LIBSBC)/decoder/include -I$(LIBSBC)/decoder/srce

TESTS = test_hci test_l2cap test_gap test_sdp test_avdtp test_sbc test_avrcp test_coverart test_replay test_source
STACK = ../hci.c ../l2cap.c ../gap.c ../sdp.c ../avdtp.c ../a2dp.c ../avrcp.c ../coverart.c ../source.c

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

libsbc.a: $(SBC_SRC)
	rm -rf sbc-obj && mkdir sbc-obj
	cd sbc-obj && $(CC) -std=c11 -O2 -w $(addprefix -I../,$(LIBSBC)/encoder/include $(LIBSBC)/decoder/include $(LIBSBC)/decoder/srce) -c $(addprefix ../,$(SBC_SRC))
	ar rcs $@ sbc-obj/*.o

test_%: test_%.c $(SRC) ../*.h fake.h chan.h libsbc.a
	$(CC) $(CFLAGS) $(SBC_INC) -o $@ $< $(SRC) libsbc.a -lm

# the whole stack against simulated headphones (sim.c, not fake.c)
test_source: test_source.c sim.c sim.h $(STACK) ../*.h libsbc.a
	$(CC) $(CFLAGS) $(SBC_INC) -o $@ test_source.c sim.c $(STACK) libsbc.a -lm

clean:
	rm -rf $(TESTS) *.dSYM libsbc.a sbc-obj

.PHONY: all clean
