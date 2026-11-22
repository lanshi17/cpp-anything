#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xsv/csv.hpp"
#include "xsv/format.hpp"
#include "xsv/simd.hpp"

namespace {

std::string make_csv(std::size_t rows) {
  std::string text = "id,name,category,value,flag,city,score,note,code,extra\n";
  text.reserve(rows * 64);
  for (std::size_t i = 0; i < rows; ++i) {
    text += std::to_string(i);
    text += ",name-";
    text += std::to_string(i % 1000);
    text += ",alpha,123.456,true,Paris,7,\"say \"\"hi\"\"\",deadbeef,";
    text += std::to_string(i % 97);
    text += '\n';
  }
  return text;
}

std::string make_bytes(std::size_t size) {
  std::string bytes(size, 'x');
  for (std::size_t i = 0; i < size; i += 97) bytes[i] = ',';
  for (std::size_t i = 0; i < size; i += 211) bytes[i] = '"';
  for (std::size_t i = 0; i < size; i += 313) bytes[i] = '\n';
  return bytes;
}

}  // namespace

static void BM_CsvScan(benchmark::State& state) {
  const std::string text = make_csv(static_cast<std::size_t>(state.range(0)));
  for (auto _ : state) {
    xsv::csv::ReaderOptions options;
    xsv::csv::Reader reader(std::span<const char>(text), options);
    benchmark::DoNotOptimize(reader.prime());
    xsv::csv::Record record;
    std::uint64_t count = 0;
    while (true) {
      auto next = reader.next(record);
      benchmark::DoNotOptimize(next);
      if (!next || !*next) break;
      ++count;
    }
    benchmark::DoNotOptimize(count);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(text.size()));
}
BENCHMARK(BM_CsvScan)->Arg(1000)->Arg(10000)->Arg(100000);

static void BM_FindRecordSpecial(benchmark::State& state) {
  const std::string bytes = make_bytes(1 << 20);
  for (auto _ : state) {
    std::size_t at = 0;
    std::size_t hits = 0;
    while (true) {
      const std::size_t found = xsv::simd::find_record_special(
          std::span<const char>(bytes).subspan(at), ',', '"');
      if (found == xsv::npos) break;
      at += found + 1;
      ++hits;
    }
    benchmark::DoNotOptimize(hits);
  }
  state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(bytes.size()));
}
BENCHMARK(BM_FindRecordSpecial);

static void BM_FormatF64(benchmark::State& state) {
  double value = 0.0;
  for (auto _ : state) {
    auto text = xsv::format_f64(value);
    benchmark::DoNotOptimize(text);
    value += 0.0001;
  }
}
BENCHMARK(BM_FormatF64);

BENCHMARK_MAIN();
