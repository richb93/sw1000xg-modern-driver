CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror
BUILD_DIR := build
TEST_BIN := $(BUILD_DIR)/test_sw1000xg_hw

.PHONY: all test clean
all: test

$(TEST_BIN): src/hardware/sw1000xg_hw.c tests/test_sw1000xg_hw.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)
	python3 -m json.tool docs/startup-recipe.json >/dev/null
	python3 -m py_compile tools/extract_yswds.py tools/generate_assets.py

clean:
	rm -f $(TEST_BIN)
