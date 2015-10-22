#include "json.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>

void print_string(const char *str)
{
	printf("\"");
	while (*str) {
		char ch = *str++;
		switch (ch) {
		case '\"':
			printf("\\\"");
			continue;

		case '\\':
			printf("\\\\");
			continue;

		case '/':
			printf("\\/");
			continue;

		case '\b':
			printf("\\b");
			continue;

		case '\f':
			printf("\\f");
			continue;

		case '\n':
			printf("\\n");
			continue;

		case '\r':
			printf("\\r");
			continue;

		case '\t':
			printf("\\t");
			continue;

		default:
			if (isascii(ch))
				printf("%c", ch);
			else
				printf("\\x%02X", (unsigned char)ch);
		}
	}
	printf("\"");
}

void indent(int indent)
{
	for (; indent > 0; --indent)
		printf("\t");
}

void json_dump(struct json_value *obj, int ind)
{
	int i;
	switch (obj->type) {
	case JSON_STRING:
		print_string(obj->value.string);
		break;

	case JSON_NUMBER:
		printf("%f", obj->value.number);
		break;

	case JSON_OBJECT:
		printf("{\n");
		for (i = 0; i < obj->value.object.num_properties; ++i) {
			indent(ind + 1);
			print_string(obj->value.object.properties[i].name);
			printf(" : ");
			json_dump(obj->value.object.properties[i].value, ind + 1);
			printf("\n");
		}
		indent(ind);
		printf("}");
		break;

	case JSON_ARRAY:
		printf("[\n");
		for (i = 0; i < obj->value.array.num_values; ++i) {
			indent(ind + 1);
			json_dump(obj->value.array.values[i], ind + 1);
			printf("%s\n", i != obj->value.array.num_values - 1 ? "," : "");
		}
		indent(ind);
		printf("]");
		break;

	case JSON_BOOLEAN:
		if (obj->value.boolean)
			printf("true");
		else
			printf("false");
		break;

	case JSON_NULL:
		printf("null");
	}
}

static char *read_file(FILE *fp)
{
	char *buf = NULL, *new_buf;
	size_t buf_size = 0, buf_room = 1000;

	/* load whole file into memory */
	while (!feof(fp)) {
		buf_room = (buf_room * 3) >> 1; /* grow by 1.5 */
		buf = realloc(buf, buf_room);
		if (!buf) {
			perror("realloc");
			exit(1);
		}
		buf_size += fread(buf + buf_size, 1,
		    buf_room - buf_size, fp);
	}
	fclose(fp);

	/* cap buffer, and zero-terminate */
	new_buf = realloc(buf, buf_size + 1);
	if (!new_buf)
		perror("realloc");
	else
		buf = new_buf;
	buf[buf_size] = '\0';
	return buf;
}

static void error(int line, const char *str)
{
	printf("ERROR:%d: %s\n", line, str);
}

int main()
{
	char *str = read_file(stdin);
	struct json_parser *p = json_create_parser();
	struct json_value *value;

#if defined(_GNU_SOURCE) || defined(MSC_VER)
	char *loc = setlocale(LC_ALL, NULL);
	setlocale(LC_ALL, "");
#endif

	value = json_parse(p, str, error);
	if (!value) {
		json_destroy_parser(p);
		free(str);
		exit(0);
	}

#if defined(_GNU_SOURCE) || defined(MSC_VER)
	setlocale(LC_ALL, loc);
#endif

	json_dump(value, 0);
	putchar('\n');

	json_destroy_parser(p);
	free(str);

	return 0;
}
