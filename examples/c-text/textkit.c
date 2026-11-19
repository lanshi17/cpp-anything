/* The C fixture: small functions whose behaviour is easy to pin down. */
#include <ctype.h>
#include <stddef.h>
#include <string.h>

int word_count(const char* text) {
  int words = 0;
  int in_word = 0;
  if (text == NULL) return 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (isspace((unsigned char)*cursor)) {
      in_word = 0;
    } else if (!in_word) {
      in_word = 1;
      ++words;
    }
  }
  return words;
}

int digits_only(const char* text) {
  int value = 0;
  int seen = 0;
  if (text == NULL) return -1;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == ' ' || *cursor == '\t') continue;
    if (*cursor < '0' || *cursor > '9') return -1;
    value = value * 10 + (*cursor - '0');
    seen = 1;
  }
  return seen ? value : -1;
}

int repeat_count(const char* text, const char* needle) {
  int count = 0;
  if (text == NULL || needle == NULL || *needle == '\0') return 0;
  const size_t length = strlen(needle);
  for (const char* cursor = text; (cursor = strstr(cursor, needle)) != NULL; cursor += length) {
    ++count;
  }
  return count;
}

double average_word_length(const char* text) {
  int words = 0;
  int letters = 0;
  int in_word = 0;
  if (text == NULL) return 0.0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (isspace((unsigned char)*cursor)) {
      in_word = 0;
    } else {
      if (!in_word) {
        in_word = 1;
        ++words;
      }
      ++letters;
    }
  }
  return words == 0 ? 0.0 : (double)letters / (double)words;
}

int is_ascii_alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int alnum_count(const char* text) {
  int count = 0;
  if (text == NULL) return 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (is_ascii_alnum(*cursor)) ++count;
  }
  return count;
}
