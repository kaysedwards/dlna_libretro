/*
 * json.c - see json.h.  A plain recursive descent parser.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "util.h"

#define JSON_MAX_DEPTH 64

typedef struct parser
{
   const char *p;
   const char *start;
   char       *error;
   size_t      error_size;
   int         failed;
} parser;

static void fail(parser *ps, const char *what)
{
   const char *c;
   int         line = 1;

   if (ps->failed)
      return;
   ps->failed = 1;
   for (c = ps->start; c < ps->p && *c; c++)
      if (*c == '\n')
         line++;
   if (ps->error && ps->error_size > 48)
      sprintf(ps->error, "%.40s on line %d", what, line);
}

static void skip_space(parser *ps)
{
   while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
      ps->p++;
}

static json_value *new_value(json_type type)
{
   json_value *v = (json_value*)calloc(1, sizeof(*v));
   if (v)
      v->type = type;
   return v;
}

void json_free(json_value *v)
{
   size_t i;
   if (!v)
      return;
   for (i = 0; i < v->count; i++)
   {
      json_free(v->items[i]);
      if (v->keys)
         free(v->keys[i]);
   }
   free(v->items);
   free(v->keys);
   free(v->text);
   free(v);
}

static int hex4(const char *s, unsigned long *out)
{
   int i;
   *out = 0;
   for (i = 0; i < 4; i++)
   {
      int c = s[i], d;
      if (c >= '0' && c <= '9')
         d = c - '0';
      else if (c >= 'a' && c <= 'f')
         d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
         d = c - 'A' + 10;
      else
         return 0;
      *out = (*out << 4) | (unsigned long)d;
   }
   return 1;
}

static size_t put_utf8(char *o, unsigned long cp)
{
   if (cp < 0x80)
   {
      o[0] = (char)cp;
      return 1;
   }
   if (cp < 0x800)
   {
      o[0] = (char)(0xC0 | (cp >> 6));
      o[1] = (char)(0x80 | (cp & 0x3F));
      return 2;
   }
   if (cp < 0x10000UL)
   {
      o[0] = (char)(0xE0 | (cp >> 12));
      o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
      o[2] = (char)(0x80 | (cp & 0x3F));
      return 3;
   }
   o[0] = (char)(0xF0 | (cp >> 18));
   o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
   o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
   o[3] = (char)(0x80 | (cp & 0x3F));
   return 4;
}

/* Parse a string starting at the opening quote.  The result is never
 * longer than the source, so the source length bounds the buffer. */
static char *parse_string(parser *ps)
{
   const char *s = ps->p + 1;
   const char *end = s;
   char       *out, *o;

   while (*end && *end != '"')
      end += (*end == '\\' && end[1]) ? 2 : 1;
   if (*end != '"')
   {
      fail(ps, "Unterminated string");
      return NULL;
   }
   out = (char*)malloc((size_t)(end - s) + 1);
   if (!out)
      return NULL;
   o = out;
   while (s < end)
   {
      if (*s != '\\')
      {
         if ((unsigned char)*s < 0x20)
         {
            fail(ps, "Control character in string");
            free(out);
            return NULL;
         }
         *o++ = *s++;
         continue;
      }
      s++;
      switch (*s)
      {
         case '"':  *o++ = '"';  break;
         case '\\': *o++ = '\\'; break;
         case '/':  *o++ = '/';  break;
         case 'b':  *o++ = '\b'; break;
         case 'f':  *o++ = '\f'; break;
         case 'n':  *o++ = '\n'; break;
         case 'r':  *o++ = '\r'; break;
         case 't':  *o++ = '\t'; break;
         case 'u':
         {
            unsigned long cp, lo;
            if (!hex4(s + 1, &cp))
            {
               fail(ps, "Bad \\u escape");
               free(out);
               return NULL;
            }
            s += 4;
            /* A UTF-16 surrogate pair encodes one character. */
            if (cp >= 0xD800 && cp <= 0xDBFF && s[1] == '\\' && s[2] == 'u'
                  && hex4(s + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF)
            {
               cp  = 0x10000UL + ((cp - 0xD800) << 10) + (lo - 0xDC00);
               s  += 6;
            }
            /* \u escapes are at least 6 bytes and expand to at most 4. */
            o += put_utf8(o, cp);
            break;
         }
         default:
            fail(ps, "Bad escape in string");
            free(out);
            return NULL;
      }
      s++;
   }
   *o     = '\0';
   ps->p  = end + 1;
   return out;
}

static json_value *parse_value(parser *ps, int depth);

static int append(json_value *v, json_value *item, char *key)
{
   json_value **items = (json_value**)realloc(v->items,
         (v->count + 1) * sizeof(*items));
   if (!items)
      return 0;
   v->items = items;
   if (v->type == JSON_OBJECT)
   {
      char **keys = (char**)realloc(v->keys, (v->count + 1) * sizeof(*keys));
      if (!keys)
         return 0;
      v->keys            = keys;
      v->keys[v->count]  = key;
   }
   v->items[v->count++] = item;
   return 1;
}

static json_value *parse_container(parser *ps, int depth, int object)
{
   json_value *v = new_value(object ? JSON_OBJECT : JSON_ARRAY);
   char        close = object ? '}' : ']';

   if (!v)
      return NULL;
   ps->p++;
   skip_space(ps);
   if (*ps->p == close)
   {
      ps->p++;
      return v;
   }
   for (;;)
   {
      char       *key = NULL;
      json_value *item;

      skip_space(ps);
      if (object)
      {
         if (*ps->p != '"')
         {
            fail(ps, "Expected a key");
            break;
         }
         key = parse_string(ps);
         if (!key)
            break;
         skip_space(ps);
         if (*ps->p != ':')
         {
            free(key);
            fail(ps, "Expected ':'");
            break;
         }
         ps->p++;
      }
      item = parse_value(ps, depth + 1);
      if (!item || !append(v, item, key))
      {
         json_free(item);
         free(key);
         break;
      }
      skip_space(ps);
      if (*ps->p == ',')
      {
         ps->p++;
         continue;
      }
      if (*ps->p == close)
      {
         ps->p++;
         return v;
      }
      fail(ps, object ? "Expected ',' or '}'" : "Expected ',' or ']'");
      break;
   }
   json_free(v);
   return NULL;
}

static json_value *parse_value(parser *ps, int depth)
{
   json_value *v;

   if (depth > JSON_MAX_DEPTH)
   {
      fail(ps, "Too deeply nested");
      return NULL;
   }
   skip_space(ps);
   switch (*ps->p)
   {
      case '{':
         return parse_container(ps, depth, 1);
      case '[':
         return parse_container(ps, depth, 0);
      case '"':
         v = new_value(JSON_STRING);
         if (v && !(v->text = parse_string(ps)))
         {
            json_free(v);
            v = NULL;
         }
         return v;
      case 't':
      case 'f':
      case 'n':
         if (strncmp(ps->p, "true", 4) == 0 || strncmp(ps->p, "false", 5) == 0)
         {
            v = new_value(JSON_BOOL);
            if (v)
               v->boolean = *ps->p == 't';
            ps->p += *ps->p == 't' ? 4 : 5;
            return v;
         }
         if (strncmp(ps->p, "null", 4) == 0)
         {
            ps->p += 4;
            return new_value(JSON_NULL);
         }
         break;
      default:
         if (*ps->p == '-' || (*ps->p >= '0' && *ps->p <= '9'))
         {
            const char *s = ps->p;
            while (*ps->p == '-' || *ps->p == '+' || *ps->p == '.'
                  || *ps->p == 'e' || *ps->p == 'E'
                  || (*ps->p >= '0' && *ps->p <= '9'))
               ps->p++;
            v = new_value(JSON_NUMBER);
            if (v)
               v->text = util_strndup(s, (size_t)(ps->p - s));
            return v;
         }
         break;
   }
   fail(ps, "Unexpected character");
   return NULL;
}

json_value *json_parse(const char *text, char *error, size_t error_size)
{
   parser      ps;
   json_value *v;

   memset(&ps, 0, sizeof(ps));
   ps.p          = text;
   ps.start      = text;
   ps.error      = error;
   ps.error_size = error_size;
   if (error && error_size)
      error[0] = '\0';
   /* Skip a UTF-8 byte order mark if an editor added one. */
   if ((unsigned char)ps.p[0] == 0xEF && (unsigned char)ps.p[1] == 0xBB
         && (unsigned char)ps.p[2] == 0xBF)
      ps.p += 3;
   v = parse_value(&ps, 0);
   if (v)
   {
      skip_space(&ps);
      if (*ps.p)
      {
         fail(&ps, "Unexpected text after the end");
         json_free(v);
         v = NULL;
      }
   }
   return v;
}

const json_value *json_get(const json_value *object, const char *key)
{
   size_t i;
   if (!object || object->type != JSON_OBJECT)
      return NULL;
   for (i = 0; i < object->count; i++)
      if (strcmp(object->keys[i], key) == 0)
         return object->items[i];
   return NULL;
}

const char *json_get_text(const json_value *object, const char *key)
{
   const json_value *v = json_get(object, key);
   if (v && (v->type == JSON_STRING || v->type == JSON_NUMBER))
      return v->text;
   return NULL;
}
