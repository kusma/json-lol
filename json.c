#include "json.h"

#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>

const char *skip_space(const char *str)
{
	while (1) {
		switch (*str) {
		case '\t':
		case '\n':
		case '\r':
		case ' ':
			++str;
			break;

		default:
			return str;
		}
	}
}

struct parser {
	const char *str;
	const char *error;
	unsigned char skip_space : 1;
};

static void init_parser(struct parser *p, const char *str)
{
	p->skip_space = 1;
	p->str = skip_space(str);
	p->error = NULL;
}

void parse_error(struct parser *p, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	putc('\n', stderr);
	va_end(va);
	exit(1);
}

char next(struct parser *p)
{
	return *p->str;
}

char consume(struct parser *p)
{
	char ret = *p->str++;
	if (p->skip_space)
		p->str = skip_space(p->str);
	return ret;
}

void consume_span(struct parser *p, int chars)
{
	p->str = p->str + chars;
	if (p->skip_space)
		p->str = skip_space(p->str);
}

void unexpected_token(struct parser *p)
{
	parse_error(p, "unexpected token '%c'", *p->str);
}

void expect(struct parser *p, char ch)
{
	if (next(p) != ch)
		unexpected_token(p);
	consume(p);
}

unsigned short parse_hexquad(struct parser *p)
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

unsigned int parse_escaped_char(struct parser *p)
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

const char *parse_raw_string(struct parser *p)
{
	int i, len = 0, pos = 0;
	unsigned int *tmp = malloc(1);
	char *ret;

	expect(p, '"');
	p->skip_space = 0;
	while (next(p) != '"') {
		unsigned int ch, bm = 0;
		int i, t = 0;
		switch (next(p)) {
		case '\\':
			ch = parse_escaped_char(p);

			if (ch >= 0xd800 && ch <= 0xdbff && next(p) == '\\') {
				/* start surrogate pair */
				unsigned int ch2 = parse_escaped_char(p);
				if (ch2 >= 0xdc00 && ch2 <= 0xdfff) {
					/* end surrogate pair */
					ch = (ch << 10) + ch2 - 0x35fdc00;
					if (ch > 0x10ffff)
						parse_error(p, "invalid unicode code-point");
				} else {
					tmp = realloc(tmp, (len + 2) * sizeof(*tmp));
					if (!tmp)
						parse_error(p, "out of memory");
					tmp[len++] = ch;
					tmp[len++] = ch2;
					continue;
				}
			}
			break;

		default:
			if (iscntrl(next(p)))
				unexpected_token(p);
			ch = consume(p);
		}

		tmp = realloc(tmp, (len + 1) * sizeof(*tmp));
		if (!tmp)
			parse_error(p, "out of memory");

		tmp[len++] = ch;
	}
	p->skip_space = 1;
	consume(p);

	ret = malloc(len * 4 + 1);
	if (!ret)
		parse_error(p, "out of memory");

	for (i = 0; i < len; ++i) {
		unsigned int ch = tmp[i], bm = 0;
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

		ret[pos++] = bm | (ch >> (6 * t)) & 0x7f;
		for (j = 0; j < t; ++j)
			ret[pos++] = 0x80 | (ch >> (6 * (t - 1 - i))) & 0xbf;
	}
	assert(pos <= 4 * len);
	ret[pos] = '\0';

	free(tmp);
	return ret; /* TODO: trim 'ret' to actually used size */
}


struct json_value *parse_string(struct parser *p)
{
	struct json_value *ret;
	int len = 0;

	ret = malloc(sizeof(*ret));
	ret->type = JSON_STRING;
	ret->value.string = parse_raw_string(p);
	return ret;
}

struct json_value *parse_value(struct parser *p);

struct json_value *parse_object(struct parser *p)
{
	struct json_value *ret;

	ret = malloc(sizeof(*ret));
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

		tmp = realloc(ret->value.object.properties, sizeof(*ret->value.object.properties) * (ret->value.object.num_properties + 1));
		if (!tmp)
			parse_error(p, "out of memory");
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

struct json_value *parse_array(struct parser *p)
{
	struct json_value *ret = malloc(sizeof(*ret));
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

		tmp = realloc(ret->value.array.values, sizeof(*ret->value.array.values) * (ret->value.array.num_values + 1));
		if (!tmp)
			parse_error(p, "out of memory");
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

struct json_value *parse_number(struct parser *p)
{
	const char *start = p->str;
	struct json_value *ret = malloc(sizeof(*ret));
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

struct json_value *parse_value(struct parser *p)
{
	switch (next(p)) {
	case '{': return parse_object(p);
	case '[': return parse_array(p);
	case '"': return parse_string(p);

	case 't':

	case '-':
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		return parse_number(p);

	default:
		if (!strncmp(p->str, "true", 4)) {
			struct json_value *ret = malloc(sizeof(*ret));
			ret->type = JSON_BOOLEAN;
			ret->value.boolean = 1;
			consume_span(p, 4);
			return ret;
		} else if (!strncmp(p->str, "false", 5)) {
			struct json_value *ret = malloc(sizeof(*ret));
			ret->type = JSON_BOOLEAN;
			ret->value.boolean = 0;
			consume_span(p, 5);
			return ret;
		} else if (!strncmp(p->str, "null", 4)) {
			struct json_value *ret = malloc(sizeof(*ret));
			ret->type = JSON_NULL;
			consume_span(p, 4);
			return ret;
		}
		unexpected_token(p);
	}
}

struct json_value *json_parse(const char *str)
{
	struct json_value *ret;
	struct parser p;
	init_parser(&p, str);
	ret = parse_value(&p);
	expect(&p, '\0');
	return ret;
}
