.PHONY: check

all: test-parser

clean:
	$(RM) test-parser

test-parser: test-parser.c json.c json.h
	$(CC) $(CPPFLAGS) $(CFLAGS) test-parser.c json.c -o test-parser

check: test-parser
	./test-parser <t/0000-basic.input.json >t/0000-basic.output.json && \
	diff t/0000-basic.expected.json t/0000-basic.output.json && \
	./test-parser <t/0001-string.input.json >t/0001-string.output.json && \
	diff t/0001-string.expected.json t/0001-string.output.json
