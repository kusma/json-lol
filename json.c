#include "json.h"

#include <stddef.h>

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
};

static void init_parser(struct parser *p, const char *str)
{
	p->str = skip_space(str);
	p->error = NULL;
}

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

void parse_error(struct parser *p, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	va_end(va);
	exit(1);
}

char next(struct parser *p)
{
	return *p->str;
}

char consume(struct parser *p)
{
	char ret = *p->str;
	p->str = skip_space(p->str + 1);
	return ret;
}

void consume_span(struct parser *p, int chars)
{
	p->str = skip_space(p->str + chars);
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

const char *parse_raw_string(struct parser *p)
{
	int len = 0;
	char *ret = malloc(1);

	expect(p, '"');
	while (next(p) != '"') {
		unsigned int u, bm = 0;
		char ch;
		int i, t = 0;
		switch (next(p)) {
		case '\\':
			consume(p);
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
				u = parse_hexquad(p);
				if (u >= 0xd800 && u <= 0xdbff) {
					/* start surrogate pair */
					unsigned short b;
					expect(p, '\\');
					expect(p, 'u');
					b = parse_hexquad(p);
					if (b >= 0xdc00 && b < 0xdfff) {
						/* end surrogate pair */
						u = (u << 10) + b - 0x35fdc00;
						if (u > 0x10ffff)
							parse_error(p, "invalid unicode code-point");
					} else
						parse_error(p, "surrogate pair mismatch");
				}

				if (u >= 0x10000) {
					t = 3;
					bm = 0xf0;
				} else if (u >= 0x800) {
					t = 2;
					bm = 0xe0;
				} else if (u >= 0x80) {
					t = 1;
					bm = 0xc0;
				}

				ret = realloc(ret, len + 2 + t);
				if (!ret)
					parse_error(p, "out of memory");

				ret[len++] = bm | (u >> (6 * t)) & 0x7f;
				for (i = 0; i < t; ++i)
					ret[len++] = 0x80 | (u >> (6 * (t - 1 - i))) & 0xbf;
				continue;

			default:
				unexpected_token(p);
			}
			break;

		default:
			ch = consume(p);
		}

		ret = realloc(ret, len + 2);
		if (!ret)
			parse_error(p, "out of memory");
		ret[len++] = ch;
	}
	consume(p);

	ret[len] = '\0';
	return ret;
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
		if (!strncmp(p->str + 1, "rue", 3)) {
			struct json_value *ret = malloc(sizeof(*ret));
			ret->type = JSON_BOOLEAN;
			ret->value.boolean = 1;
			consume_span(p, 4);
			return ret;
		} else
			unexpected_token(p);

	case 'f':
		if (!strncmp(p->str + 1, "alse", 4)) {
			struct json_value *ret = malloc(sizeof(*ret));
			ret->type = JSON_BOOLEAN;
			ret->value.boolean = 0;
			consume_span(p, 5);
			return ret;
		} else
			unexpected_token(p);

	case 'n':
		if (!strncmp(p->str + 1, "ull", 3)) {
			struct json_value *ret = malloc(sizeof(*ret));
			ret->type = JSON_NULL;
			consume_span(p, 4);
			return ret;
		} else
			unexpected_token(p);

	case '-':
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		return parse_number(p);

	default:
		unexpected_token(p);
	}
}

struct json_value *json_parse(const char *str)
{
	struct parser p;
	init_parser(&p, str);

	switch (next(&p)) {
	case '{': return parse_object(&p);
	case '[': return parse_array(&p);
	default:
		unexpected_token(&p);
	}
}
