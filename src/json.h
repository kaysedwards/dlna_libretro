/*
 * json.h - a small JSON reader (RFC 8259) for playlist files.
 *
 * The whole document is parsed into a tree of json_value nodes.  Numbers
 * keep their original text (so a port written as 5080 or "5080" reads the
 * same way and no locale-dependent conversion is needed).
 */
#ifndef DLNA_JSON_H
#define DLNA_JSON_H

#include <stddef.h>

typedef enum json_type
{
   JSON_NULL = 0,
   JSON_BOOL,
   JSON_NUMBER,
   JSON_STRING,
   JSON_ARRAY,
   JSON_OBJECT
} json_type;

typedef struct json_value
{
   json_type           type;
   int                 boolean;  /* JSON_BOOL                        */
   char               *text;     /* JSON_STRING (UTF-8), JSON_NUMBER */
   struct json_value **items;    /* JSON_ARRAY / JSON_OBJECT values  */
   char              **keys;     /* JSON_OBJECT keys                 */
   size_t              count;
} json_value;

/* Parse a NUL terminated document.  Returns NULL on error, with a short
 * description (including the line number) in 'error'. */
json_value *json_parse(const char *text, char *error, size_t error_size);
void        json_free(json_value *v);

/* Member of an object (NULL if missing or not an object). */
const json_value *json_get(const json_value *object, const char *key);

/* The text of a string or number member, or NULL. */
const char *json_get_text(const json_value *object, const char *key);

#endif
