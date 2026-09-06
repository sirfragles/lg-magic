# Userspace tools: a single multi-call binary `lg-magic` + unit tests.
#
# The binary itself is Linux-only (evdev/hidraw/uinput); the `check` target
# builds and runs the portable module tests, which also pass on macOS.

CC      ?= cc
CFLAGS  += -O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE -Iinclude -I../include
LDLIBS  += -lm

BIN := lg-magic

SRC := \
	src/main.c src/config.c \
	src/cmd_imu.c src/cmd_analyze.c src/cmd_calibrate.c \
	src/cmd_calib2bin.c src/cmd_config.c src/cmd_setup.c \
	src/matrix.c src/json.c src/csv.c src/lm.c src/madgwick.c src/calib.c \
	src/evdev.c src/hidraw.c src/uinput.c src/modprobe_helpers.c src/cube.c

OBJ := $(SRC:.c=.o)

# Portable modules, shared with the unit tests (build on macOS too).
PORTABLE_SRC := src/matrix.c src/json.c src/csv.c src/calib.c \
	src/madgwick.c src/lm.c src/cube.c src/config.c

# Explicit rules (no pattern rules, to keep BSD make on macOS happy).
TEST_DIR := tests
TEST_BINS := $(TEST_DIR)/test_json $(TEST_DIR)/test_matrix $(TEST_DIR)/test_csv \
	$(TEST_DIR)/test_madgwick $(TEST_DIR)/test_lm $(TEST_DIR)/test_calib_blob \
	$(TEST_DIR)/test_config $(TEST_DIR)/test_cube_math

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(TEST_DIR)/test_json: $(TEST_DIR)/test_json.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_matrix: $(TEST_DIR)/test_matrix.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_csv: $(TEST_DIR)/test_csv.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_madgwick: $(TEST_DIR)/test_madgwick.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_lm: $(TEST_DIR)/test_lm.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_calib_blob: $(TEST_DIR)/test_calib_blob.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_config: $(TEST_DIR)/test_config.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

$(TEST_DIR)/test_cube_math: $(TEST_DIR)/test_cube_math.c $(PORTABLE_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(PORTABLE_SRC) $(LDLIBS)

check: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
		echo "== $$t"; \
		if ./$$t; then echo "PASS $$t"; else echo "FAIL $$t"; fail=1; fi; \
	done; \
	if [ -x $(BIN) ]; then sh tests/cli_smoke.sh ./$(BIN) || fail=1; fi; \
	exit $$fail

clean:
	rm -f $(BIN) $(OBJ) $(TEST_BINS)

.PHONY: all check clean
