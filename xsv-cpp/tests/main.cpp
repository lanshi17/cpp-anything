#include <exception>
#include <iostream>

#include "harness.hpp"

int main() {
  int failed = 0;
  int passed = 0;
  for (const test::Case& item : test::registry()) {
    try {
      item.run();
      ++passed;
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "FAIL " << item.name << ": " << error.what() << '\n';
    } catch (...) {
      ++failed;
      std::cerr << "FAIL " << item.name << ": unknown exception\n";
    }
  }
  std::cout << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
