# Host-side tests for the score engine. No Pebble SDK needed.
CC ?= cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -DER_DEMO=1

test: test_engine
	./test_engine

test_engine: test_engine.c ../src/c/engine.c ../src/c/demo_data.c ../src/c/*.h
	$(CC) $(CFLAGS) -o $@ test_engine.c ../src/c/engine.c ../src/c/demo_data.c

clean:
	rm -rf test_engine test_engine.dSYM

.PHONY: test clean
