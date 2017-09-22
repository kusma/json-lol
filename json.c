#include "json.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct alloc {
	struct alloc *prev, *next;
	char data[0];
};

struct json_parser {
	struct json_sax_cb *cb;
	void *user;

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

#define PR(buf, c) (sprintf((buf), isgraph(c) ? "'%c'" : "\\x%02x", (c)), (buf))

static void unexpected_token(struct json_parser *p)
{
	char tmp[32];
	char ch = next(p);
	if (ch == '\0')
		parse_error(p, "unexpected end of input");
	else
		parse_error(p, "unexpected token %s", PR(tmp, ch));
}

static void expect(struct json_parser *p, char ch)
{
	if (next(p) != ch) {
		char a[32], b[32];
		parse_error(p, "unexpected token %s, expected %s",
		            PR(a, next(p)), PR(b, ch));
	}

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

		dst[ret++] = bm | ((ch >> (6 * t)) & 0x7f);
		for (j = 1; j <= t; ++j)
			dst[ret++] = 0x80 | ((ch >> (6 * (t - j))) & 0xbf);
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

			if (buf[0] >= 0xd800 && buf[0] <= 0xdbff) {
				/* start surrogate pair */
				if (next(p) == '\\') {
					buf[1] = parse_escaped_char(p);
					if (buf[1] >= 0xdc00 && buf[1] <= 0xdfff) {
						/* end surrogate pair */
						buf[0] = (buf[0] << 10) + buf[1] - 0x35fdc00;
						assert(buf[0] <= 0x10ffff);
					} else {
						buf[0] = 0xfffd; /* U+FFFD */
						chars = 2;
					}
				} else
					buf[0] = 0xfffd; /* U+FFFD */
			} else if (buf[0] >= 0xdc00 && buf[0] <= 0xdfff)
				buf[0] = 0xfffd; /* U+FFFD */

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


static void parse_string(struct json_parser *p)
{
	p->cb->on_string(p->user, parse_raw_string(p));
}

static void parse_value(struct json_parser *p);

static void parse_object(struct json_parser *p)
{
	expect(p, '{');
	p->cb->on_object_start(p->user);

	if (next(p) == '}') {
		consume(p);
		p->cb->on_object_end(p->user);
		return;
	}

	while (1) {
		void *tmp;
		const char *name = parse_raw_string(p);
		p->cb->on_key(p->user, name);

		expect(p, ':');
		parse_value(p);
		if (next(p) == '}')
			break;
		expect(p, ',');
	}
	consume(p);

	p->cb->on_object_end(p->user);
}

void parse_array(struct json_parser *p)
{
	expect(p, '[');
	p->cb->on_array_start(p->user);

	if (next(p) == ']') {
		consume(p);
		p->cb->on_array_end(p->user);
		return;
	}

	while (1) {
		parse_value(p);
		if (next(p) == ']')
			break;
		expect(p, ',');
	}
	consume(p);

	p->cb->on_array_end(p->user);
}

void parse_number(struct json_parser *p)
{
	double number;
	const char *start = p->str;
	char *end;

	if (next(p) == '-')
		consume(p);

	if (!isdigit(next(p)))
		unexpected_token(p);

	if (consume(p) != '0')
		while (isdigit(next(p)))
			consume(p);

	if (next(p) == '.') {
		consume(p);

		if (!isdigit(next(p)))
			unexpected_token(p);

		while (isdigit(next(p)))
			consume(p);
	}

	if (tolower(next(p)) == 'e') {
		consume(p);
		if (next(p) == '+' ||
		    next(p) == '-')
			consume(p);

		if (!isdigit(next(p)))
			unexpected_token(p);

		while (isdigit(next(p)))
			consume(p);
	}

	number = strtod(start, &end);
	if (end == start)
		parse_error(p, "strtod failed: %s", strerror(errno));

	p->cb->on_number(p->user, number);
}

static struct json_value *parse_keyword(struct json_parser *p, const char *str, int len)
{
	int i;
	assert(next(p) == str[0]); /* should already be matched at this point */
	consume(p);
	for (i = 1; i < len; ++i)
		expect(p, str[i]);
	return mem_alloc(p, sizeof(struct json_value));
}

static void parse_value(struct json_parser *p)
{
	switch (next(p)) {
	case '{': parse_object(p); break;
	case '[': parse_array(p); break;
	case '"': parse_string(p); break;

	case '-':
	case '0': case '1': case '2': case '3': case '4':
	case '5': case '6': case '7': case '8': case '9':
		parse_number(p);
		break;

	case 't':
		parse_keyword(p, "true", 4);
		p->cb->on_boolean(p->user, 1);
		break;

	case 'f':
		parse_keyword(p, "false", 5);
		p->cb->on_boolean(p->user, 0);
		break;

	case 'n':
		parse_keyword(p, "null", 4);
		p->cb->on_null(p->user);
		break;

	default:
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

int json_parse_sax(struct json_parser *p, const char *str,
                   struct json_sax_cb *cb, void *user)
{
	if (setjmp(p->jmp)) {
		if (cb->on_error)
			cb->on_error(user, p->line, p->error);

		free_allocs(p);
		return -1;
	}

	p->cb = cb;
	p->user = user;
	p->skip_space = 1;
	p->str = str;
	p->line = 1;
	skip_space(p);

	parse_value(p);
	expect(p, '\0');
	return 0;
}

struct json_node {
	struct json_node *parent;
	struct json_value value;
};

struct json_dom_context
{
	struct json_parser *p;
	struct json_node *curr, *root;
	const char *key;
	void (*on_err)(int, const char *);
};

struct json_dom_context *json_dom_context(void *user)
{
	return (struct json_dom_context*)user;
}

static void on_error(void *user , int line, const char *err)
{
	struct json_dom_context *ctx = json_dom_context(user);
	ctx->on_err(line, err);
}

static void on_node(struct json_dom_context *ctx, struct json_node *node)
{
	struct json_node *parent_node = ctx->curr;
	struct json_value *value = &node->value;

	node->parent = parent_node;

	if (parent_node) {
		void *tmp;
		struct json_value *parent = &parent_node->value;

		switch (parent->type) {
		case JSON_ARRAY:
			if (parent->value.array.num_values == INT_MAX / sizeof(void *))
				parse_error(ctx->p, "too big array");

			parent->value.array.values = mem_realloc(ctx->p,
				parent->value.array.values,
				sizeof(*parent->value.array.values) *
				(parent->value.array.num_values + 1));

			parent->value.array.values[parent->value.array.num_values++] = value;

			break;

		case JSON_OBJECT:
			if (parent->value.object.num_properties == INT_MAX / sizeof(void *))
				parse_error(ctx->p, "too big object");

			assert(ctx->key && "internal error: unexpected json-type");

			tmp = mem_realloc(ctx->p,
				parent->value.object.properties,
				sizeof(*parent->value.object.properties) *
				(parent->value.object.num_properties + 1));

			parent->value.object.properties = tmp;
			parent->value.object.properties[parent->value.object.num_properties].name = ctx->key;
			parent->value.object.properties[parent->value.object.num_properties].value = value;
			++parent->value.object.num_properties;
			ctx->key = NULL;
			break;

		default:
			assert(0 && "internal error: unexpected json-type");
		}
	} else {
		assert(!ctx->root);
		ctx->root = node;
	}
}

static void on_null(void *user)
{
	struct json_dom_context *ctx = json_dom_context(user);

	struct json_node *node = mem_alloc(ctx->p, sizeof(*node));
	node->value.type = JSON_NULL;

	on_node(ctx, node);
}

static void on_boolean(void *user, int boolean)
{
	struct json_dom_context *ctx = json_dom_context(user);

	struct json_node *node = mem_alloc(ctx->p, sizeof(*node));
	node->value.type = JSON_BOOLEAN;
	node->value.value.boolean = boolean;

	on_node(ctx, node);
}

static void on_number(void *user, double number)
{
	struct json_dom_context *ctx = json_dom_context(user);

	struct json_node *node = mem_alloc(ctx->p, sizeof(*node));
	node->value.type = JSON_NUMBER;
	node->value.value.number = number;

	on_node(ctx, node);
}

static void on_string(void *user, const char *string)
{
	struct json_dom_context *ctx = json_dom_context(user);

	struct json_node *node = mem_alloc(ctx->p, sizeof(*node));
	node->value.type = JSON_STRING;
	node->value.value.string = string;

	on_node(ctx, node);
}

static void on_array_start(void *user)
{
	struct json_dom_context *ctx = json_dom_context(user);

	struct json_node *node = mem_alloc(ctx->p, sizeof(*node));
	node->value.type = JSON_ARRAY;
	node->value.value.array.values = NULL;
	node->value.value.array.num_values = 0;
	on_node(ctx, node);

	ctx->curr = node;
}

static void on_array_end(void *user)
{
	struct json_dom_context *ctx = json_dom_context(user);
	assert(ctx->curr != NULL);
	ctx->curr = ctx->curr->parent;
}

static void on_object_start(void *user)
{
	struct json_dom_context *ctx = json_dom_context(user);

	struct json_node *node = mem_alloc(ctx->p, sizeof(*node));
	node->value.type = JSON_OBJECT;
	node->value.value.object.properties = NULL;
	node->value.value.object.num_properties = 0;
	on_node(ctx, node);
	ctx->curr = node;
}

static void on_key(void *user, const char *key)
{
	struct json_dom_context *ctx = json_dom_context(user);
	ctx->key = key;
}

static void on_object_end(void *user)
{
	struct json_dom_context *ctx = json_dom_context(user);
	assert(ctx->curr != NULL);
	ctx->curr = ctx->curr->parent;
}

struct json_value *json_parse_dom(struct json_parser *p, const char *str,
                                  void (*err)(int, const char *))
{
	struct json_dom_context ctx = {
		p,
		NULL,
		NULL,
		NULL,
		err
	};

	struct json_sax_cb cb = {
		on_error,
		on_null,
		on_boolean,
		on_number,
		on_string,

		on_array_start,
		on_array_end,

		on_object_start,
		on_key,
		on_object_end
	};


	if (json_parse_sax(p, str, &cb, &ctx) < 0)
		return NULL;

	assert(ctx.root != NULL);
	return &ctx.root->value;
}
