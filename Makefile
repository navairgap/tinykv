# tinykv — build library, tests, and tools.
# Override CC/CFLAGS on the command line, e.g. make CC=clang.
CC ?= cc
AR ?= ar
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L
CPPFLAGS ?= -Iinclude
BUILD := build

LIB_SRCS := src/record_codec.c src/tkv_index.c src/tkv_io.c src/tkv_log.c \
            src/tkv_db.c
LIB_OBJS := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRCS))

TESTS := test_record_codec test_index test_io test_log test_db
TEST_BINS := $(patsubst %,$(BUILD)/%,$(TESTS))
TOOL := $(BUILD)/tinykv

.PHONY: all test clean

all: $(BUILD)/libtinykv.a $(TEST_BINS) $(TOOL)

$(BUILD) $(BUILD)/obj:
	mkdir -p $@

$(BUILD)/obj/%.o: src/%.c | $(BUILD)/obj
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/libtinykv.a: $(LIB_OBJS) | $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/%: tests/%.c $(BUILD)/libtinykv.a | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Isrc $< $(BUILD)/libtinykv.a -o $@

$(TOOL): src/tkv_tool.c $(BUILD)/libtinykv.a | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $< $(BUILD)/libtinykv.a -o $@

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== $$t"; ./$$t || exit 1; done

clean:
	rm -rf $(BUILD)
