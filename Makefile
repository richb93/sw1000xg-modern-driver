CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror
BUILD_DIR := build
TEST_BIN := $(BUILD_DIR)/test_sw1000xg_hw
TRACE_BIN := $(BUILD_DIR)/trace_startup
TRACE_OUT := $(BUILD_DIR)/startup-trace.txt
CORE := src/hardware/sw1000xg_hw.c src/hardware/sw1000xg_trace.c

.PHONY: all test trace-check clean
all: test

$(TEST_BIN): $(CORE) tests/test_sw1000xg_hw.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@

$(TRACE_BIN): $(CORE) tests/trace_startup.c tests/zero_assets.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $^ -o $@

# The C core's startup must match an independent expansion of the recipe, and
# the comparator must reject a trace with one altered write.
trace-check: $(TRACE_BIN)
	./$(TRACE_BIN) > $(TRACE_OUT)
	python3 tools/recipe_trace.py compare $(TRACE_OUT)
	awk '!done && $$0 == "SWXG W 3F084 00000800" \
		{ print "SWXG W 3F084 08000000"; done = 1; next } { print }' \
		$(TRACE_OUT) > $(TRACE_OUT).bad
	! cmp -s $(TRACE_OUT) $(TRACE_OUT).bad
	! python3 tools/recipe_trace.py compare $(TRACE_OUT).bad > /dev/null

test: $(TEST_BIN) trace-check
	./$(TEST_BIN)
	python3 -m json.tool docs/startup-recipe.json >/dev/null
	python3 -m py_compile tools/extract_yswds.py tools/generate_assets.py tools/recipe_trace.py

clean:
	rm -rf $(BUILD_DIR)
