/* prefs.c -- persistent key/value store backing cocos2d-x UserDefault
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "prefs.h"
#include "util.h"

#define MAX_ENTRIES 1024
#define KEY_MAX 128
#define VAL_MAX 1024

typedef struct {
  char key[KEY_MAX];
  char val[VAL_MAX]; // values are stored as text; typed getters parse on read
} Entry;

static Entry entries[MAX_ENTRIES];
static int entry_count = 0;
static char prefs_path[512];
static int dirty = 0;

// Values may contain arbitrary bytes. We use TAB as the key/value separator, so
// newline, backslash AND tab must be escaped or a value containing one would
// corrupt the file / be mis-split on reload.
static void decode_inplace(char *s) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if (p[0] == '\\' && p[1] == 'n') { *o++ = '\n'; p++; }
    else if (p[0] == '\\' && p[1] == 't') { *o++ = '\t'; p++; }
    else if (p[0] == '\\' && p[1] == '\\') { *o++ = '\\'; p++; }
    else *o++ = *p;
  }
  *o = 0;
}

static void encode(FILE *f, const char *s) {
  for (; *s; s++) {
    if (*s == '\n') fputs("\\n", f);
    else if (*s == '\t') fputs("\\t", f);
    else if (*s == '\\') fputs("\\\\", f);
    else fputc(*s, f);
  }
}

static Entry *find(const char *key) {
  for (int i = 0; i < entry_count; i++)
    if (!strcmp(entries[i].key, key))
      return &entries[i];
  return NULL;
}

void prefs_init(const char *path) {
  snprintf(prefs_path, sizeof(prefs_path), "%s", path);
  entry_count = 0;
  dirty = 0;

  FILE *f = fopen(prefs_path, "r");
  if (!f)
    return;
  // worst case a stored line is key + TAB + fully-escaped value (2x) + NL
  char line[KEY_MAX + 2 * VAL_MAX + 8];
  while (fgets(line, sizeof(line), f) && entry_count < MAX_ENTRIES) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    char *sp = strchr(line, '\t');
    if (!sp) continue;
    *sp = 0;
    Entry *e = &entries[entry_count++];
    snprintf(e->key, sizeof(e->key), "%.*s", (int)sizeof(e->key) - 1, line);
    snprintf(e->val, sizeof(e->val), "%.*s", (int)sizeof(e->val) - 1, sp + 1);
    decode_inplace(e->val);
  }
  fclose(f);
}

void prefs_flush(void) {
  if (!dirty || !prefs_path[0])
    return;
  FILE *f = fopen(prefs_path, "w");
  if (!f)
    return;
  for (int i = 0; i < entry_count; i++) {
    fputs(entries[i].key, f);
    fputc('\t', f);
    encode(f, entries[i].val);
    fputc('\n', f);
  }
  fclose(f);
  dirty = 0;
}

static void set_raw(const char *key, const char *val) {
  Entry *e = find(key);
  if (!e) {
    if (entry_count >= MAX_ENTRIES) {
      debugPrintf("prefs: table full, dropping %s\n", key);
      return;
    }
    e = &entries[entry_count++];
    snprintf(e->key, sizeof(e->key), "%s", key);
  }
  snprintf(e->val, sizeof(e->val), "%s", val);
  dirty = 1;
  prefs_flush(); // cocos setters apply() immediately; keep parity
}

const char *prefs_get_string(const char *key, const char *def) {
  Entry *e = find(key);
  return e ? e->val : def;
}
int prefs_get_bool(const char *key, int def) {
  Entry *e = find(key);
  if (!e) return def;
  return (!strcmp(e->val, "true") || !strcmp(e->val, "1")) ? 1 : 0;
}
int prefs_get_int(const char *key, int def) {
  Entry *e = find(key);
  return e ? atoi(e->val) : def;
}
float prefs_get_float(const char *key, float def) {
  Entry *e = find(key);
  return e ? (float)atof(e->val) : def;
}

void prefs_set_string(const char *key, const char *val) { set_raw(key, val ? val : ""); }
void prefs_set_bool(const char *key, int val) { set_raw(key, val ? "true" : "false"); }
void prefs_set_int(const char *key, int val) {
  char buf[32]; snprintf(buf, sizeof(buf), "%d", val); set_raw(key, buf);
}
void prefs_set_float(const char *key, float val) {
  char buf[48]; snprintf(buf, sizeof(buf), "%.9g", (double)val); set_raw(key, buf);
}
void prefs_delete(const char *key) {
  for (int i = 0; i < entry_count; i++) {
    if (!strcmp(entries[i].key, key)) {
      entries[i] = entries[--entry_count];
      dirty = 1;
      prefs_flush();
      return;
    }
  }
}
