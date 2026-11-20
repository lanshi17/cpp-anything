#include <cstdio>
// Unit checks for the pieces the reference implementation defines exactly:
// float text, hashing, version comparison and the host name of the probe.
import std;
import cppan.json;
import cppan.util;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
  if (condition) return;
  ++failures;
  std::fprintf(stderr, "FAIL %s\n", what.c_str());
}

void check_equal(std::string_view got, std::string_view want, const std::string& what) {
  if (got == want) return;
  ++failures;
  std::fprintf(stderr, "FAIL %s: got %.*s want %.*s\n", what.c_str(),
               static_cast<int>(got.size()), got.data(), static_cast<int>(want.size()),
               want.data());
}

}  // namespace

int main() {
  // Rust's `{:?}` for floats, which the recorded wire format is compared as text.
  check_equal(cppan::format_real(1e-9), "1e-9", "format 1e-9");
  check_equal(cppan::format_real(1e-5), "1e-5", "format 1e-5");
  check_equal(cppan::format_real(1e-4), "0.0001", "format 1e-4");
  check_equal(cppan::format_real(1e6), "1000000.0", "format 1e6");
  check_equal(cppan::format_real(1e15), "1000000000000000.0", "format 1e15");
  check_equal(cppan::format_real(1e16), "1e16", "format 1e16");
  check_equal(cppan::format_real(123456.789), "123456.789", "format 123456.789");
  check_equal(cppan::format_real(-0.0), "-0.0", "format -0.0");
  check_equal(cppan::format_real(7.0), "7.0", "format 7.0");
  check_equal(cppan::format_real(1.2345e-5), "1.2345e-5", "format 1.2345e-5");
  check_equal(cppan::format_real(9.999999999999999e15), "1e16", "format rounds like Rust");

  // The hash the reference uses for recorded-from.json and probe names.
  check_equal(cppan::hash64("abc"), "1922bb190541574b", "hash64 abc");

  // Non-standard literals the recorder writes must round-trip.
  check_equal(cppan::dump(cppan::parse("NaN")), "NaN", "parse NaN");
  check_equal(cppan::dump(cppan::parse("Infinity")), "Infinity", "parse Infinity");
  check_equal(cppan::dump(cppan::parse("-Infinity")), "-Infinity", "parse -Infinity");
  check_equal(cppan::dump(cppan::parse("1e-9")), "1e-9", "real keeps shortest text");

  // Versions compare numerically, not lexically.
  check(cppan::version_at_least("19.10", "19.9"), "19.10 >= 19.9");
  check(!cppan::version_at_least("14.2", "15"), "14.2 < 15");
  check(cppan::version_at_least("16", "15.2"), "16 >= 15.2");

  check_equal(cppan::truncate_text("abcdef", 3), "abc…", "truncate_text");
  check_equal(cppan::pad_right("ab", 4), "ab  ", "pad_right");
  check_equal(cppan::sanitize_identifier("9lives"), "u_9lives", "sanitize_identifier");
  check_equal(cppan::sanitize_identifier("textkit.words"), "textkit_words",
              "sanitize_identifier dotted");

  if (failures == 0) std::printf("unit checks: ok\n");
  return failures == 0 ? 0 : 1;
}
