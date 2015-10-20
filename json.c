#include "json.h"

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct alloc {
	struct alloc *prev, *next;
	char data[0];
};

struct json_parser {
	const char *str;
	jmp_buf jmp;
	char error[1024];
	int line;
	unsigned char skip_space : 1;
	struct alloc *alloc_head;
};

static void skip_space(struct json_parser *p)
{
	const char *str = p->str;
	while (1) {
		switch (*str) {
		case '\n':
			p->line++;
			++str;
			break;

		case '\r':
			p->line++;
			++str;
			if (*str == '\n')
				++str;
			break;

		case '\t':
		case ' ':
			++str;
			break;

		default:
			p->str = str;
			return;
		}
	}
}

static void init_parser(struct json_parser *p, const char *str)
{
	p->skip_space = 1;
	p->str = str;
	p->line = 1;
	skip_space(p);
}

static void parse_error(struct json_parser *p, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	vsnprintf(p->error, sizeof(p->error), fmt, va);
	va_end(va);
	longjmp(p->jmp, -1);
}

static void *mem_alloc(struct json_parser *p, size_t size)
{
	struct alloc *a;

	if (size > SIZE_MAX - sizeof(struct alloc))
		parse_error(p, "too large allocation");

	a = malloc(sizeof(struct alloc) + size);
	if (!a)
		parse_error(p, "malloc failed: %s", strerror(errno));

	a->prev = NULL;
	a->next = p->alloc_head;
	if (p->alloc_head)
		p->alloc_head->prev = a;
	p->alloc_head = a;

	return &a->data;
}

static void *mem_realloc(struct json_parser *p, void *ptr, size_t size)
{
	struct alloc *a, *new;

	if (!ptr)
		return mem_alloc(p, size);

	a = ptr - offsetof(struct alloc, data);

	new = realloc(a, sizeof(struct alloc) + size);
	if (!new)
		parse_error(p, "realloc failed: %s", strerror(errno));

	if (new->next)
		new->next->prev = new;
	if (new->prev)
		new->prev->next = new;
	if (p->alloc_head == a)
		p->alloc_head = new;

	return &new->data;
}

static void mem_free(struct json_parser *p, void *ptr)
{
	struct alloc *a = ptr - offsetof(struct alloc, data);

	if (a->next)
		a->next->prev = a->prev;
	if (a->prev)
		a->prev->next = a->next;
	if (p->alloc_head == a)
		p->alloc_head = a->next;

	free(a);
}

static char next(struct json_parser *p)
{
	return *p->str;
}

static char consume(struct json_parser *p)
{
	char ret = *p->str++;
	if (p->skip_space)
		skip_space(p);
	return ret;
}

static void consume_span(struct json_parser *p, int chars)
{
	p->str = p->str + chars;
	if (p->skip_space)
		skip_space(p);
}

static void unexpected_token(struct json_parser *p)
{
	parse_error(p, "unexpected token '%c'", *p->str);
}

static void expect(struct json_parser *p, char ch)
{
	if (next(p) != ch)
		parse_error(p, "unexpected token '%c', expected '%c'",
		            *p->str, ch);
	if (ch != '\0')
		consume(p);
}

static unsigned short parse_hexquad(struct json_parser *p)
{
	unsigned short val = 0;
	int i;
	for (i = 0; i < 4; ++i) {
		char ch;
		if (!isxdigit(next(p)))
			unexpected_token(p);

		val <<= 4;
		ch = consume(p);
		if ('0' <= ch && ch <= '9')
			val |= ch - '0';
		else
			val |= 10 + tolower(ch) - 'a';
	}
	return val;
}

static unsigned int parse_escaped_char(struct json_parser *p)
{
	char ch;
	expect(p, '\\');

	switch (next(p)) {
	case '"': ch = '"'; break;
	case '\\': ch = '\\'; break;
	case '/': ch = '/'; break;
	case 'b': ch = '\b'; break;
	case 'f': ch = '\f'; break;
	case 'n': ch = '\n'; break;
	case 'r': ch = '\r'; break;
	case 't': ch = '\t'; break;

	case 'u':
		consume(p);
		return parse_hexquad(p);

	default:
		unexpected_token(p);
	}

	consume(p);
	return ch;
}

static int encode_utf8(char *dst, unsigned int *src, int len)
{
	int i, ret = 0;
	for (i = 0; i < len; ++i) {
		unsigned int ch = src[i], bm = 0;
		int j, t = 0;

		if (ch >= 0x10000) {
			t = 3;
			bm = 0xf0;
		} else if (ch >= 0x800) {
			t = 2;
			bm = 0xe0;
		} else if (ch >= 0x80) {
			t = 1;
			bm = 0xc0;
		}

		dst[ret++] = bm | (ch >> (6 * t)) & 0x7f;
		for (j = 0; j < t; ++j)
			dst[ret++] = 0x80 | (ch >> (6 * (t - 1 - j))) & 0xbf;
	}
	assert(ret <= 4 * len);
	return ret;
}

static const char *parse_raw_string(struct json_parser *p)
{
	size_t alloc = 16, len = 0;
	char *ret = mem_alloc(p, alloc);

	expect(p, '"');
	p->skip_space = 0;
	while (next(p) != '"') {
		unsigned int buf[2], chars = 1;
		switch (next(p)) {
		case '\\':
			buf[0] = parse_escaped_char(p);

			if (buf[0] >= 0xd800 && buf[0] <= 0xdbff && next(p) == '\\') {
				/* start surrogate pair */
				buf[1] = parse_escaped_char(p);
				if (buf[1] >= 0xdc00 && buf[1] <= 0xdfff) {
					/* end surrogate pair */
					buf[0] = (buf[0] << 10) + buf[1] - 0x35fdc00;
					assert(buf[0] <= 0x10ffff);
				} else
					chars = 2;
			}
			break;

		default:
			if (iscntrl(next(p)))
				unexpected_token(p);
			buf[0] = consume(p);
		}

		/* Worst case length is 6 UTF-8 bytes, plus termination */
		if (len > alloc - 6 - 1) {
			if (alloc > SIZE_MAX / 3)
				parse_error(p, "too long string");

			alloc = (alloc * 3) >> 1; /* grow by 150% */
			ret = mem_realloc(p, ret, alloc);
		}

		len += encode_utf8(ret + len, buf, chars);
	}
	p->skip_space = 1;
	consume(p);

	assert(alloc > len);
	ret[len] = '\0';
	return ret;
}


static struct json_value *parse_string(struct json_parser *p)
{
	struct json_value *ret = mem_alloc(p, sizeof(*ret));
	ret->type = JSON_STRING;
	ret->value.string = parse_raw_string(p);
	return ret;
}

static struct json_value *parse_value(struct json_parser *p);

static struct json_value *parse_object(struct json_parser *p)
{
	struct json_value *ret = mem_alloc(p, sizeof(*ret));
	ret->type = JSON_OBJECT;
	ret->value.object.properties = NULL;
	ret->value.object.num_properties = 0;

	expect(p, '{');
	if (next(p) == '}') {
		consume(p);
		return ret;
	}

	while (1) {
		void *tmp;
		struct json_value *value;
		const char *name = parse_raw_string(p);
		expect(p, ':');
		value = parse_value(p);

		if (ret->value.array.num_values == SIZE_MAX / sizeof(void *))
			parse_error(p, "too big object");

		tmp = mem_realloc(p, ret->value.object.properties,
		                  sizeof(*ret->value.object.properties) *
		                  (ret->value.object.num_properties + 1));

		ret->value.object.properties = tmp;
		ret->value.object.properties[ret->value.object.num_properties].name = name;
		ret->value.object.properties[ret->value.object.num_properties].value = value;
		++ret->value.object.num_properties;

		if (next(p) == '}')
			break;
		expect(p, ',');
	}
	consume(p);

	return ret;
}

static struct json_value *parse_array(struct json_parser *p)
{
	struct json_value *ret = mem_alloc(p, sizeof(*ret));
	ret->type = JSON_ARRAY;
	ret->value.array.values = NULL;
	ret->value.array.num_values = 0;

	expect(p, '[');
	if (next(p) == ']') {
		consume(p);
		return ret;
	}

	while (1) {
		void *tmp;
		struct json_value *value = parse_value(p);

		if (ret->value.array.num_values == SIZE_MAX / sizeof(void *))
			parse_error(p, "too big array");

		tmp = mem_realloc(p, ret->value.array.values,
		                  sizeof(*ret->value.array.values) *
		                  (ret->value.array.num_values + 1));

		ret->value.array.values = tmp;
		ret->value.array.values[ret->value.array.num_values] = value;
		++ret->value.array.num_values;

		if (next(p) == ']')
			break;
		expect(p, ',');
	}
	consume(p);

	return ret;
}

static struct json_value *parse_number(struct json_parser *p)
{
	const char *start = p->str;
	struct json_value *ret = mem_alloc(p, sizeof(*ret));
	ret->type = JSON_NUMBER;

	if (next(p) == '-')
		consume(p);

	if (!isdigit(next(p)))
		unexpected_token(p);

	if (consume(p) != '0')
		while (isdigit(next(p)))
			consume(p);

	if (next(p) == '.') {
		consume(p);
		while (isdigit(next(p)))
			consume(p);
	}

	if (tolower(next(p)) == 'e') {
		consume(p);
		if (next(p) == '+' ||
		    next(p) == '-')
			consume(p);
		while (isdigit(next(p)))
			consume(p);
	}

	ret->value.number = atof(start);
	return ret;
}

static struct json_value *parse_value(struct json_parser *p)
{
	switch (next(p)) {
	case '{': return parse_object(p);
	case '[': return parse_array(p);
	case '"': return parse_string(p);

	case '-':
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		return parse_number(p);

	default:
		if (!strncmp(p->str, "true", 4)) {
			struct json_value *ret = mem_alloc(p, sizeof(*ret));
			ret->type = JSON_BOOLEAN;
			ret->value.boolean = 1;
			consume_span(p, 4);
			return ret;
		} else if (!strncmp(p->str, "false", 5)) {
			struct json_value *ret = mem_alloc(p, sizeof(*ret));
			ret->type = JSON_BOOLEAN;
			ret->value.boolean = 0;
			consume_span(p, 5);
			return ret;
		} else if (!strncmp(p->str, "null", 4)) {
			struct json_value *ret = mem_alloc(p, sizeof(*ret));
			ret->type = JSON_NULL;
			consume_span(p, 4);
			return ret;
		}
		unexpected_token(p);
	}
}

struct json_parser *json_create_parser(void)
{
	struct json_parser *p = malloc(sizeof(*p));
	if (!p)
		return NULL;

	p->alloc_head = NULL;
	return p;
}

static void free_allocs(struct json_parser *p)
{
	struct alloc *curr = p->alloc_head;
	while (curr != NULL) {
		void *mem = curr;
		curr = curr->next;
		free(mem);
	}
	p->alloc_head = NULL;
}

void json_destroy_parser(struct json_parser *p)
{
	free_allocs(p);
	free(p);
}

struct json_value *json_parse(struct json_parser *p, const char *str,
                              void (*err)(int, const char *))
{
	struct json_value *ret;

	if (setjmp(p->jmp)) {
		if (err)
			err(p->line, p->error);

		free_allocs(p);
		return NULL;
	}

	p->skip_space = 1;
	p->str = str;
	p->line = 1;
	skip_space(p);

	ret = parse_value(p);
	expect(p, '\0');

	return ret;
}
