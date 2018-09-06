#ifndef JSON_H
#define JSON_H

struct json_sax_cb {
	void (*on_error)(void *, int, const char *);

	// plain values
	void (*on_null)(void *);
	void (*on_boolean)(void *, int);
	void (*on_number)(void *, double);
	void (*on_string)(void *, const char *);

	// arrays
	void (*on_array_start)(void *);
	void (*on_array_end)(void *);

	// objects
	void (*on_object_start)(void *);
	void (*on_key)(void *, const char *);
	void (*on_object_end)(void *);
};

struct json_value {
	enum {
		JSON_NULL,
		JSON_BOOLEAN,
		JSON_NUMBER,
		JSON_STRING,
		JSON_ARRAY,
		JSON_OBJECT
	} type;

	union {
		int boolean;
		double number;
		const char *string;
		struct {
			struct json_value **values;
			int num_values;
		} array;
		struct {
			struct {
				const char *name;
				struct json_value *value;
			} *properties;
			int num_properties;
		} object;
	} value;
};

struct json_parser;

struct json_parser *json_create_parser(void);
void json_destroy_parser(struct json_parser *p);

int json_parse_sax(struct json_parser *p, const char *str,
                   struct json_sax_cb *cb, void *user);
struct json_value *json_parse_dom(struct json_parser *p, const char *str,
                                  void (*err)(int, const char *));

#endif /* JSON_H */
