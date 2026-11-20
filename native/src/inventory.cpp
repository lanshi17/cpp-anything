#include "cppan/inventory.hpp"

#include <format>

namespace cppan {
namespace {

std::string string_of(const Json* value, std::string_view key, std::string_view fallback = "") {
  if (value == nullptr) return std::string(fallback);
  const Json* field = value->find(key);
  if (field == nullptr || !field->is_string()) return std::string(fallback);
  return field->as_string();
}

std::vector<std::string> strings(const Json* value) {
  std::vector<std::string> out;
  if (value == nullptr || !value->is_array()) return out;
  for (const Json& item : value->as_array()) {
    if (item.is_string()) out.push_back(item.as_string());
  }
  return out;
}

std::string first_line(const Json* value) {
  if (value == nullptr || !value->is_array() || value->as_array().empty()) return {};
  const Json& first = value->as_array().front();
  return first.is_string() ? first.as_string() : std::string();
}

}  // namespace

Result<Function> Function::parse(const Json& item) {
  Function function;
  std::vector<Param> params;
  if (const Json* list = item.find("params"); list != nullptr && list->is_array()) {
    for (const Json& param : list->as_array()) {
      Param entry;
      entry.annotation = string_of(&param, "annotation");
      entry.name = string_of(&param, "name", "arg");
      entry.ty = Ty::parse(entry.annotation);
      if (const Json* value = param.find("default");
          value != nullptr && !value->is_null()) {
        entry.default_value = *value;
      }
      if (const Json* has_default = param.find("has_default");
          has_default != nullptr && has_default->is_bool()) {
        entry.has_default = has_default->as_bool();
      }
      if (const Json* keyword = param.find("keyword_only");
          keyword != nullptr && keyword->is_bool()) {
        entry.keyword_only = keyword->as_bool();
      }
      params.push_back(std::move(entry));
    }
  }
  const std::string returns_annotation = string_of(&item, "returns");
  if (!item.find("name") || !item.find("name")->is_string()) {
    return std::unexpected("function without a name");
  }
  function.name = item.find("name")->as_string();
  if (const Json* line = item.find("line"); line != nullptr && line->is_integer()) {
    function.line = static_cast<std::uint32_t>(line->as_int());
  }
  function.params = std::move(params);
  function.returns = Ty::parse(returns_annotation);
  function.returns_annotation = returns_annotation;
  function.doc = first_line(item.find("doc"));
  function.risks = strings(item.find("risks"));
  if (const Json* raises = item.find("raises"); raises != nullptr && raises->is_bool()) {
    function.raises = raises->as_bool();
  }
  return function;
}

Json Function::to_json() const {
  Json::Array params_json;
  for (const Param& param : params) {
    params_json.push_back(Json::object({
        {"name", Json::string(param.name)},
        {"annotation", Json::string(param.annotation)},
        {"default", param.default_value.value_or(Json::null())},
        {"has_default", Json::boolean(param.has_default)},
        {"keyword_only", Json::boolean(param.keyword_only)},
    }));
  }
  Json::Array doc_lines;
  {
    std::string_view rest = doc;
    while (!rest.empty()) {
      const std::size_t newline = rest.find('\n');
      doc_lines.push_back(Json::string(std::string(rest.substr(0, newline))));
      if (newline == std::string_view::npos) break;
      rest.remove_prefix(newline + 1);
    }
  }
  Json::Array risk_items;
  for (const std::string& risk : risks) risk_items.push_back(Json::string(risk));
  return Json::object({
      {"name", Json::string(name)},
      {"line", Json::integer(line)},
      {"params", Json::array(std::move(params_json))},
      {"returns", Json::string(returns_annotation)},
      {"raises", Json::boolean(raises)},
      {"doc", Json::array(std::move(doc_lines))},
      {"risks", Json::array(std::move(risk_items))},
  });
}

std::string Function::cpp_declaration() const { return cpp_signature() + ";"; }

std::string Function::cpp_definition() const { return cpp_signature_with(false); }

std::string Function::cpp_signature() const { return cpp_signature_with(true); }

std::string Function::cpp_signature_with(bool with_defaults) const {
  std::string out = "[[nodiscard]] cppan::Result<" + returns.cpp_return() + "> " + name + "(";
  for (std::size_t index = 0; index < params.size(); ++index) {
    if (index > 0) out += ", ";
    const Param& param = params[index];
    out += param.ty.cpp_param(true) + " " + param.name;
    if (param.has_default && with_defaults && param.default_value.has_value()) {
      if (const auto value = param.ty.cpp_default(*param.default_value); value.has_value()) {
        out += " = " + *value;
      }
    }
  }
  out += ')';
  return out;
}

std::string Function::plain_signature() const {
  std::string out = returns.cpp_return() + " " + name + "(";
  for (std::size_t index = 0; index < params.size(); ++index) {
    if (index > 0) out += ", ";
    out += params[index].ty.cpp_param(true) + " " + params[index].name;
  }
  out += ')';
  return out;
}

std::string Function::signature_line() const {
  std::string out = name + "(";
  for (std::size_t index = 0; index < params.size(); ++index) {
    if (index > 0) out += ", ";
    out += params[index].name + ": " + params[index].ty.label();
    if (params[index].has_default) out += " = …";
  }
  out += ") -> " + returns.label();
  return out;
}

Result<Unit> Unit::parse(const Json& doc) {
  const auto text = [&](std::string_view key) -> Result<std::string> {
    const Json* value = doc.find(key);
    if (value == nullptr || !value->is_string()) {
      return std::unexpected("inventory is missing " + std::string(key));
    }
    return value->as_string();
  };
  auto file = text("file");
  if (!file) return std::unexpected(file.error());
  Unit unit;
  unit.file = *file;
  const Json* functions = doc.find("functions");
  if (functions == nullptr || !functions->is_array()) {
    return std::unexpected("inventory is missing functions");
  }
  for (const Json& item : functions->as_array()) {
    auto function = Function::parse(item);
    if (!function) return std::unexpected(function.error());
    unit.functions.push_back(std::move(*function));
  }
  if (const Json* skipped = doc.find("skipped"); skipped != nullptr && skipped->is_array()) {
    for (const Json& item : skipped->as_array()) {
      unit.skipped.emplace_back(string_of(&item, "name", "?"), string_of(&item, "reason", "?"));
    }
  }
  auto name = text("unit");
  if (!name) return std::unexpected(name.error());
  unit.unit = *name;
  auto module = text("module");
  if (!module) return std::unexpected(module.error());
  unit.module = *module;
  if (const Json* root = doc.find("package_root");
      root != nullptr && root->is_string() && !root->as_string().empty()) {
    unit.package_root = root->as_string();
  }
  if (const Json* lines = doc.find("lines"); lines != nullptr && lines->is_integer()) {
    unit.lines = static_cast<std::uint32_t>(lines->as_int());
  }
  unit.risks = strings(doc.find("risks"));
  unit.imports = strings(doc.find("imports"));
  return unit;
}

}  // namespace cppan
