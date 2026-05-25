#pragma once
/*
 * jsmn - minimalistic JSON tokenizer
 * (c) Serge Zaitsev, 2010-2020 — MIT License
 * Single-header form; define JSMN_STATIC for internal linkage.
 */
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  JSMN_UNDEFINED = 0,
  JSMN_OBJECT    = 1,
  JSMN_ARRAY     = 2,
  JSMN_STRING    = 3,
  JSMN_PRIMITIVE = 4
} jsmntype_t;

#define JSMN_ERROR_NOMEM (-1)
#define JSMN_ERROR_INVAL (-2)
#define JSMN_ERROR_PART  (-3)

typedef struct {
  jsmntype_t type;
  int start;
  int end;
  int size; /* number of direct child tokens */
} jsmntok_t;

typedef struct {
  unsigned int pos;
  int toknext;
  int toksuper;
} jsmn_parser;

static inline void jsmn_init(jsmn_parser *p) {
  p->pos = 0;
  p->toknext = 0;
  p->toksuper = -1;
}

static inline jsmntok_t *jsmn__alloc(jsmn_parser *p, jsmntok_t *toks,
                                      unsigned int ntoks) {
  if ((unsigned int)p->toknext >= ntoks) return NULL;
  jsmntok_t *t = &toks[p->toknext++];
  t->start = t->end = -1;
  t->size = 0;
  return t;
}

static inline int jsmn__primitive(jsmn_parser *p, const char *js, size_t len,
                                   jsmntok_t *toks, unsigned int ntoks) {
  int start = (int)p->pos;
  for (; p->pos < len; p->pos++) {
    switch (js[p->pos]) {
    case '\t': case '\r': case '\n': case ' ':
    case ',': case ']': case '}':
      goto found;
    default:
      if ((unsigned char)js[p->pos] < 0x20) {
        p->pos = (unsigned int)start;
        return JSMN_ERROR_INVAL;
      }
    }
  }
found:
  if (!toks) { p->pos--; return 0; }
  jsmntok_t *t = jsmn__alloc(p, toks, ntoks);
  if (!t) { p->pos = (unsigned int)start; return JSMN_ERROR_NOMEM; }
  t->type = JSMN_PRIMITIVE;
  t->start = start;
  t->end = (int)p->pos;
  p->pos--;
  return 0;
}

static inline int jsmn__string(jsmn_parser *p, const char *js, size_t len,
                                jsmntok_t *toks, unsigned int ntoks) {
  int start = (int)p->pos++;
  for (; p->pos < len; p->pos++) {
    char c = js[p->pos];
    if (c == '"') {
      if (!toks) return 0;
      jsmntok_t *t = jsmn__alloc(p, toks, ntoks);
      if (!t) { p->pos = (unsigned int)start; return JSMN_ERROR_NOMEM; }
      t->type = JSMN_STRING;
      t->start = start + 1;
      t->end = (int)p->pos;
      return 0;
    }
    if (c == '\\' && p->pos + 1 < len) {
      p->pos++;
      switch (js[p->pos]) {
      case '"': case '/': case '\\': case 'b':
      case 'f': case 'r': case 'n':  case 't': break;
      case 'u': {
        p->pos++;
        int i;
        for (i = 0; i < 4 && p->pos < len; i++, p->pos++) {
          char h = js[p->pos];
          if (!((h >= '0' && h <= '9') || (h >= 'A' && h <= 'F') ||
                (h >= 'a' && h <= 'f'))) {
            p->pos = (unsigned int)start;
            return JSMN_ERROR_INVAL;
          }
        }
        p->pos--;
        break;
      }
      default:
        p->pos = (unsigned int)start;
        return JSMN_ERROR_INVAL;
      }
    }
  }
  p->pos = (unsigned int)start;
  return JSMN_ERROR_PART;
}

static inline int jsmn_parse(jsmn_parser *p, const char *js, size_t len,
                               jsmntok_t *toks, unsigned int ntoks) {
  int r, i;
  jsmntok_t *tok;
  int count = p->toknext;

  for (; p->pos < len && js[p->pos] != '\0'; p->pos++) {
    char c = js[p->pos];
    switch (c) {
    case '{': case '[':
      count++;
      if (!toks) break;
      tok = jsmn__alloc(p, toks, ntoks);
      if (!tok) return JSMN_ERROR_NOMEM;
      if (p->toksuper != -1) toks[p->toksuper].size++;
      tok->type = (c == '{') ? JSMN_OBJECT : JSMN_ARRAY;
      tok->start = (int)p->pos;
      p->toksuper = p->toknext - 1;
      break;

    case '}': case ']':
      if (!toks) break;
      for (i = p->toknext - 1; i >= 0; i--) {
        tok = &toks[i];
        if (tok->start != -1 && tok->end == -1) {
          jsmntype_t expect = (c == '}') ? JSMN_OBJECT : JSMN_ARRAY;
          if (tok->type != expect) return JSMN_ERROR_INVAL;
          p->toksuper = -1;
          tok->end = (int)p->pos + 1;
          break;
        }
      }
      if (i == -1) return JSMN_ERROR_INVAL;
      for (; i >= 0; i--) {
        tok = &toks[i];
        if (tok->start != -1 && tok->end == -1) {
          p->toksuper = i;
          break;
        }
      }
      break;

    case '"':
      r = jsmn__string(p, js, len, toks, ntoks);
      if (r < 0) return r;
      count++;
      if (p->toksuper != -1 && toks) toks[p->toksuper].size++;
      break;

    case '\t': case '\r': case '\n': case ' ': break;

    case ':':
      p->toksuper = p->toknext - 1;
      break;

    case ',':
      if (toks && p->toksuper != -1 &&
          toks[p->toksuper].type != JSMN_ARRAY &&
          toks[p->toksuper].type != JSMN_OBJECT) {
        for (i = p->toknext - 1; i >= 0; i--) {
          if (toks[i].type == JSMN_ARRAY || toks[i].type == JSMN_OBJECT) {
            if (toks[i].start != -1 && toks[i].end == -1) {
              p->toksuper = i;
              break;
            }
          }
        }
      }
      break;

    default:
      r = jsmn__primitive(p, js, len, toks, ntoks);
      if (r < 0) return r;
      count++;
      if (p->toksuper != -1 && toks) toks[p->toksuper].size++;
      break;
    }
  }

  if (toks) {
    for (i = p->toknext - 1; i >= 0; i--)
      if (toks[i].start != -1 && toks[i].end == -1) return JSMN_ERROR_PART;
  }
  return count;
}

#ifdef __cplusplus
}
#endif
