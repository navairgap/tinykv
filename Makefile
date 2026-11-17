# tinykv — build library, tests, and tools.
# Override CC/CFLAGS on the command line, e.g. make CC=clang.
CC ?= cc
AR ?= ar
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L
# make SAN=1 rebuilds everything under address+undefined sanitizers. A
# CFLAGS override on the command line disables the toggle, so pass
# sanitizer flags yourself in that case.
SAN ?= 0
ifeq ($(SAN),1)
SAN_CFLAGS := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
else
SAN_CFLAGS :=
endif
CPPFLAGS ?= -Iinclude
BUILD := build
PREFIX ?= /usr/local
BINDIR := $(DESTDIR)$(PREFIX)/bin
LIBDIR := $(DESTDIR)$(PREFIX)/lib
INCDIR := $(DESTDIR)$(PREFIX)/include/tinykv
PKGCFG := $(DESTDIR)$(PREFIX)/share/pkgconfig
VERSION ?= 1.0.0

PUBLIC_HEADERS := include/tinykv/record.h include/tinykv/db.h

LIB_SRCS := src/record_codec.c src/tkv_index.c src/tkv_io.c src/tkv_log.c \
            src/tkv_db.c
LIB_OBJS := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRCS))

TESTS := test_record_codec test_index test_io test_log test_db test_golden
TEST_BINS := $(patsubst %,$(BUILD)/%,$(TESTS))
TOOL := $(BUILD)/tinykv

.PHONY: all test fuzz bench install uninstall dist clean

all: $(BUILD)/libtinykv.a $(TEST_BINS) $(TOOL) $(BUILD)/example_basic

$(BUILD) $(BUILD)/obj:
	mkdir -p $@

$(BUILD)/obj/%.o: src/%.c | $(BUILD)/obj
	$(CC) $(CFLAGS) $(SAN_CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/libtinykv.a: $(LIB_OBJS) | $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/%: tests/%.c $(BUILD)/libtinykv.a | $(BUILD)
	$(CC) $(CFLAGS) $(SAN_CFLAGS) $(CPPFLAGS) -Isrc $< $(BUILD)/libtinykv.a -o $@

$(TOOL): src/tkv_tool.c $(BUILD)/libtinykv.a | $(BUILD)
	$(CC) $(CFLAGS) $(SAN_CFLAGS) $(CPPFLAGS) $< $(BUILD)/libtinykv.a -o $@

$(BUILD)/example_basic: examples/basic.c $(BUILD)/libtinykv.a | $(BUILD)
	$(CC) $(CFLAGS) $(SAN_CFLAGS) $(CPPFLAGS) $< $(BUILD)/libtinykv.a -o $@

$(BUILD)/fuzz_record_codec: fuzz/record_codec_fuzz.c $(BUILD)/libtinykv.a | $(BUILD)
	$(CC) $(CFLAGS) $(SAN_CFLAGS) $(CPPFLAGS) $< $(BUILD)/libtinykv.a -o $@

$(BUILD)/bench_tinykv: bench/bench.c $(BUILD)/libtinykv.a | $(BUILD)
	$(CC) $(CFLAGS) $(SAN_CFLAGS) $(CPPFLAGS) $< $(BUILD)/libtinykv.a -o $@

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== $$t"; ./$$t || exit 1; done

fuzz: $(BUILD)/fuzz_record_codec
	./$(BUILD)/fuzz_record_codec

bench: $(BUILD)/bench_tinykv
	./$(BUILD)/bench_tinykv

install: all
	install -d $(BINDIR) $(LIBDIR) $(INCDIR) $(PKGCFG)
	install -m 0755 $(TOOL) $(BINDIR)/tinykv
	install -m 0644 $(BUILD)/libtinykv.a $(LIBDIR)/libtinykv.a
	install -m 0644 $(PUBLIC_HEADERS) $(INCDIR)/
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' \
	    tinykv.pc.in > $(PKGCFG)/tinykv.pc

uninstall:
	rm -f $(BINDIR)/tinykv $(LIBDIR)/libtinykv.a \
	      $(PKGCFG)/tinykv.pc
	rmdir $(INCDIR) 2>/dev/null || true

dist:
	git archive --format=tar.gz --prefix=tinykv-$(VERSION)/ \
	    -o tinykv-$(VERSION).tar.gz HEAD

clean:
	rm -rf $(BUILD)
