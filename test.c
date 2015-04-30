#include "json.h"
#include <stdio.h>

void print_string(const char *str)
{
	printf("\"");
	while (*str) {
		/* TODO: unicode */
		char ch = *str++;
		switch (ch) {
		case '\"':
			printf("\"");
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
	int i, j;
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
		printf("}\n");
		break;

	case JSON_ARRAY:
		printf("[\n");
		for (i = 0; i < obj->value.array.num_values; ++i) {
			indent(ind + 1);
			json_dump(obj->value.array.values[i], ind + 1);
			printf("%s\n", i != obj->value.array.num_values - 1 ? "," : "");
		}
		indent(ind);
		printf("]\n");
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

int main()
{
	const char *str = "{ \"foo\" : [ \"b\\nar\", -1e+1, \"foo \\uD834\\uDD1E \" ] }";
	struct json_value *value;
	printf("input: %s\n", str);
	value = json_parse(str);
	json_dump(value, 0);
}
