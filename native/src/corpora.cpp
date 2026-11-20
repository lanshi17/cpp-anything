#include "cppan/corpora.hpp"

#include <algorithm>
#include <format>
#include <initializer_list>

namespace cppan::corpora {
namespace {

constexpr std::string_view kStrings[] = {
    "",           " ",           "Hello World", "  Mixed CASE  ", "café — naïve",
    "a",          "123",         "-",           "trailing---",    " leading",
    "one\ntwo",   "Москва",      "0",           "a-b_c",          "x",
};

constexpr std::int64_t kInts[] = {
    0, 1, -1, 7, 42, 12, 100, -100, 10'000, 3, 19, 255, -7, 2'000'000, 8, 11,
};

constexpr double kFloats[] = {
    0.0, -0.0, 1.5, -2.25, 0.1, 2.0, -0.5, 1e-9, 123456.789, -1.0, 7.0, 1e6, 3.25, -99.5, 0.25,
};

std::vector<Json> strings() {
  std::vector<Json> out;
  for (const std::string_view text : kStrings) out.push_back(Json::string(std::string(text)));
  return out;
}

std::vector<Json> integers() {
  std::vector<Json> out;
  for (const std::int64_t value : kInts) out.push_back(Json::integer(value));
  return out;
}

std::vector<Json> floats() {
  std::vector<Json> out;
  for (const double value : kFloats) out.push_back(Json::real(value));
  return out;
}

Json list_of(std::initializer_list<Json> items) {
  Json::Array array;
  for (const Json& item : items) array.push_back(item);
  return Json::array(std::move(array));
}

std::vector<Json> values_for(const Ty& ty) {
  switch (ty.kind) {
    case Ty::Kind::Str: return strings();
    case Ty::Kind::Int: return integers();
    case Ty::Kind::Float: return floats();
    case Ty::Kind::Bool: return {Json::boolean(true), Json::boolean(false)};
    case Ty::Kind::None: return {Json::null()};
    case Ty::Kind::List: {
      const Ty& inner = ty.items.at(0);
      if (inner.kind == Ty::Kind::Str) {
        return {
            list_of({}),
            list_of({Json::string("a")}),
            list_of({Json::string("a"), Json::string("b"), Json::string("c")}),
            list_of({Json::string(""), Json::string(" ")}),
            list_of({Json::string("café"), Json::string("Mixed CASE")}),
        };
      }
      if (inner.kind == Ty::Kind::Int) {
        return {
            list_of({}),
            list_of({Json::integer(0)}),
            list_of({Json::integer(1), Json::integer(2), Json::integer(3)}),
            list_of({Json::integer(-1), Json::integer(0), Json::integer(1)}),
        };
      }
      return {list_of({}), list_of({Json::null()})};
    }
    case Ty::Kind::Dict: {
      const Ty& value = ty.items.at(0);
      if (value.kind == Ty::Kind::Str) {
        return {
            Json::object(),
            Json::object({{"a", Json::string("x")}}),
            Json::object({{"a", Json::string("1")}, {"b", Json::string("2")}}),
        };
      }
      return {
          Json::object(),
          Json::object({{"a", Json::integer(1)}}),
          Json::object({{"n", Json::integer(-1)}, {"m", Json::integer(2)}}),
      };
    }
    case Ty::Kind::Optional: {
      std::vector<Json> values = {Json::null()};
      for (Json& value : values_for(ty.items.at(0))) values.push_back(std::move(value));
      if (values.size() > 8) values.resize(8);
      return values;
    }
    case Ty::Kind::Tuple:
    case Ty::Kind::Any: break;
  }
  return {
      Json::null(),
      Json::string("x"),
      Json::integer(0),
      Json::real(1.5),
      list_of({Json::integer(1)}),
      Json::object({{"a", Json::integer(1)}}),
      Json::boolean(true),
  };
}

}  // namespace

std::vector<Json> calls(const std::vector<Function>& functions, std::size_t count) {
  std::vector<Json> out;
  for (const Function& function : functions) {
    if (function.params.empty()) {
      out.push_back(Json::object({
          {"id", Json::string(function.name + ":0")},
          {"fn", Json::string(function.name)},
          {"args", Json::array()},
      }));
      continue;
    }
    std::vector<std::vector<Json>> corpora;
    for (const Param& param : function.params) corpora.push_back(values_for(param.ty));
    std::size_t total = 0;
    for (const auto& values : corpora) total = std::max(total, values.size());
    total = std::min(total, std::max<std::size_t>(count, 1));
    for (std::size_t index = 0; index < total; ++index) {
      Json::Array args;
      for (const auto& values : corpora) args.push_back(values[index % values.size()]);
      out.push_back(Json::object({
          {"id", Json::string(function.name + ":" + std::to_string(index))},
          {"fn", Json::string(function.name)},
          {"args", Json::array(std::move(args))},
      }));
    }
  }
  return out;
}

}  // namespace cppan::corpora
