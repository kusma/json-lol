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

struct json_value *json_parse(const char *str,
                              void (*err)(int line, const char *str));

#endif /* JSON_H */
