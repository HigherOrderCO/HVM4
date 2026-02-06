# HVM4 Metal build
# Requires macOS with Metal SDK (Xcode command-line tools)

XCRUN     = xcrun
METAL_CC  = $(XCRUN) -sdk macosx metal
METALLIB  = $(XCRUN) -sdk macosx metallib
CLANG     = clang

# Metal shader compilation flags
# -std=metal3.1 for 64-bit atomics (requires Apple Silicon M1+)
METAL_FLAGS = -std=metal3.2 --target=air64-apple-macos15 -O2 -Wall

# Host compilation flags
HOST_FLAGS = -O2 -Wall -fobjc-arc -framework Metal -framework Foundation

# Outputs
SHADER_AIR  = hvm4.air
SHADER_LIB  = hvm4.metallib
HOST_OBJ    = host.o
TEST_BIN    = test_metal

.PHONY: all clean shader host test

all: $(SHADER_LIB) $(TEST_BIN)

# --- Shader compilation (two-step: .metal -> .air -> .metallib) ---
$(SHADER_AIR): hvm4.metal
	$(METAL_CC) $(METAL_FLAGS) -c $< -o $@

$(SHADER_LIB): $(SHADER_AIR)
	$(METALLIB) $< -o $@

shader: $(SHADER_LIB)

# --- Host library ---
$(HOST_OBJ): host.m host.h
	$(CLANG) $(HOST_FLAGS) -c $< -o $@

host: $(HOST_OBJ)

# --- Test binary (shader + host + test harness) ---
$(TEST_BIN): test.m $(HOST_OBJ) $(SHADER_LIB)
	$(CLANG) $(HOST_FLAGS) test.m $(HOST_OBJ) -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -f $(SHADER_AIR) $(SHADER_LIB) $(HOST_OBJ) $(TEST_BIN)
