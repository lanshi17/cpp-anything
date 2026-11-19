// The wired example: the `slug` command answered by the C++ port, and the rest of
// the CLI's small surface answered here so the front door can lift.
//
// This is the file `cpp-anything app wire` generates a stub for; filling it in is
// the point at which a command stops falling back to the original.
#include "cppan/glue.hpp"
#include "textkit_words/port.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string join_words(int argc, char** argv, int from) {
  std::string joined;
  for (int index = from; index < argc; ++index) {
    if (!joined.empty()) joined += ' ';
    joined += argv[index];
  }
  return joined;
}

}  // namespace

bool handle_textkit_words(int argc, char** argv, int* code) {
  const std::string_view command = argc > 1 ? argv[1] : "";
  if (command == "slug") {
    const std::string joined = join_words(argc, argv, 2);
    // The port's signature is in include/textkit_words/port.hpp; the failure type is
    // the Python exception's name, so the CLI can map it to the same exit code.
    const auto result = textkit_words::slug(joined);
    if (!result) {
      std::fprintf(stderr, "%s\n", result.error().message.c_str());
      *code = 1;
      return true;
    }
    std::printf("%s\n", result->c_str());
    *code = 0;
    return true;
  }
  if (command == "count") {
    std::size_t words = 0;
    std::string word;
    while (std::cin >> word) ++words;
    std::printf("%zu\n", words);
    *code = 0;
    return true;
  }
  if (command == "fail") {
    std::fprintf(stderr, "boom: the fixture fails on purpose\n");
    *code = 2;
    return true;
  }
  std::fprintf(stderr, "usage: slugify slug <words...> | count | fail\n");
  *code = 1;
  return true;
}
