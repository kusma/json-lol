.PHONY: check

test-parser: test-parser.c json.c json.h
	gcc test-parser.c json.c -o test-parser

check: test-parser
	./test-parser <t/0000-basic.input.json >t/0000-basic.output.json && \
	diff t/0000-basic.expected.json t/0000-basic.output.json