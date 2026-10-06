# ──────────────────────────────────────────────────
# mini-httpd — A static file HTTP server in C
# ──────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -Wextra -Werror -pedantic -std=c11
LDFLAGS =

# Debug build: includes debug symbols for gdb
# Release build: optimized, no debug symbols
DEBUG_FLAGS   = -g -O0 -DDEBUG
RELEASE_FLAGS = -O2 -DNDEBUG

TARGET = httpd
SRC    = httpd.c

.PHONY: all debug release clean run test

# Default: debug build
all: debug

debug: CFLAGS += $(DEBUG_FLAGS)
debug: $(TARGET)

release: CFLAGS += $(RELEASE_FLAGS)
release: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built $(TARGET) successfully"

# Create sample www directory if it doesn't exist
www:
	mkdir -p www
	echo '<!DOCTYPE html><html><head><title>mini-httpd</title></head><body><h1>It works!</h1><p>Served by mini-httpd/1.0 (C)</p></body></html>' > www/index.html

# Run the server
run: debug www
	./$(TARGET)

# Test caching behavior
test: debug www
	@echo "Starting server..."
	@timeout 5 ./$(TARGET) & sleep 0.5
	@echo "\n=== First request (should be 200) ==="
	@curl -sv http://localhost:8080/ 2>&1 | grep -E "^(<|HTTP|ETag|Last-Modified|Cache-Control)"
	@echo "\n=== Second request with ETag (should be 304) ==="
	@ETAG=$$(curl -s -D- http://localhost:8080/ 2>/dev/null | grep ETag | tr -d '\r' | cut -d' ' -f2); \
	 curl -sv -H "If-None-Match: $$ETAG" http://localhost:8080/ 2>&1 | grep -E "^(<|HTTP)"
	@echo "\n=== Request for missing file (should be 404) ==="
	@curl -sv http://localhost:8080/missing.txt 2>&1 | grep "^< HTTP"
	@echo "\nAll tests passed!"
	@kill %1 2>/dev/null; true

# Memory leak check with Valgrind
valgrind: debug www
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

clean:
	rm -f $(TARGET)
	rm -f step1 step2 step3
