.PHONY: check

all: test-parser

clean:
	$(RM) test-parser

test-parser: test-parser.c json.c json.h
	$(CC) -D_GNU_SOURCE $(CPPFLAGS) $(CFLAGS) test-parser.c json.c -o test-parser

check: test-parser
	@                                                                \
	for input in t/*.input.json;                                     \
	do                                                               \
		output=$${input%.input.json}.output.json;                \
		expected=$${input%.input.json}.expected.json;            \
		echo $$input;                                            \
		$(TESTS_ENVIRONMENT) ./test-parser <$$input >$$output && \
		diff $$expected $$output ||                              \
		exit;                                                    \
	done
