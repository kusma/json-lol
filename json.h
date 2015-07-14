#ifndef JSON_H
#define JSON_H

struct json_value {
	enum {
		JSON_STRING,
		JSON_NUMBER,
		JSON_OBJECT,
		JSON_ARRAY,
		JSON_BOOLEAN,
		JSON_NULL
	} type;

	union {
		const char *string;
		double number;
		struct {
			struct {
				const char *name;
				struct json_value *value;
			} *properties;
			int num_properties;
		} object;
		struct {
			struct json_value **values;
			int num_values;
		} array;
		int boolean;
	} value;
};

struct json_parser;

struct json_parser *json_create_parser(void);
void json_destroy_parser(struct json_parser *p);
struct json_value *json_parse(struct json_parser *p, const char *str,
                              void (*err)(int, const char *));

#endif /* JSON_H */
