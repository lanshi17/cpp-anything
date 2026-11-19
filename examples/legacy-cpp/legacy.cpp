// C-style C++: the source language of `cpp-anything ... --lang cpp`.
// The port's job is to keep every result and replace all of this with modern C++.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int word_count(const char* text) {
  int words = 0;
  int in_word = 0;
  for (int i = 0; i < (int)strlen(text); ++i) {
    if (isspace((unsigned char)text[i])) {
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
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == ' ' || *cursor == '\t') continue;
    if (*cursor < '0' || *cursor > '9') return -1;
    value = value * 10 + (*cursor - '0');
    seen = 1;
  }
  return seen ? value : -1;
}

double average_word_length(const char* text) {
  int words = 0;
  int letters = 0;
  int in_word = 0;
  for (int i = 0; text[i] != '\0'; ++i) {
    if (isspace((unsigned char)text[i])) {
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

int sum_ascii(const char* text) {
  int total = 0;
  for (int i = 0; i < (int)strlen(text); ++i) total += (unsigned char)text[i];
  return total;
}

std::string trim_copy(const std::string& text) {
  int start = 0;
  int end = (int)text.size();
  while (start < end && isspace((unsigned char)text[(size_t)start])) ++start;
  while (end > start && isspace((unsigned char)text[(size_t)end - 1])) --end;
  const int length = end - start;
  char* buffer = (char*)malloc((size_t)length + 1);
  memcpy(buffer, text.c_str() + start, (size_t)length);
  buffer[length] = '\0';
  std::string out(buffer);
  free(buffer);
  return out;
}

std::string repeat_join(const std::string& text, int times) {
  std::string out;
  for (int i = 0; i < times; ++i) {
    if (i > 0) out += " ";
    out += text;
  }
  return out;
}

void print_words(const char* text) {
  printf("words=%d\n", word_count(text));
}

int count_char(const char* text, char needle) {
  int count = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor == needle) ++count;
  }
  return count;
}
