CC ?= cc
CFLAGS ?= -O1 -g
WARN = -std=c11 -Wall -Wextra -Wconversion -Werror -pedantic
SANITIZERS ?= -fsanitize=address,undefined -fno-omit-frame-pointer
BUILD ?= /tmp/203-lighting-core-tests

.PHONY: test clean
test:
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(SANITIZERS) -I. lighting_authority.c tests/test_authority.c -o $(BUILD)/test_authority
	$(BUILD)/test_authority

clean:
	rm -f $(BUILD)/test_authority
