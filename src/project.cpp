module;

module cppan.project;
import cppan.json;
import cppan.types;
import cppan.util;
import cppan.inventory;
import std;

namespace cppan {
namespace {

std::string text_of(const Json& value, std::string_view key) {
  const Json* field = value.find(key);
  if (field == nullptr || !field->is_string()) return {};
  return field->as_string();
}

std::optional<double> number_of(const Json& value, std::string_view key) {
  const Json* field = value.find(key);
  if (field == nullptr || !field->is_number()) return std::nullopt;
  return field->as_number();
}

std::filesystem::path resolve_against(const std::filesystem::path& base,
                                      const std::string& text) {
  if (text.empty()) return {};
  const std::filesystem::path path(text);
  if (path.is_absolute()) return path;
  return base / path;
}

std::filesystem::path canonical_or_self(const std::filesystem::path& path) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  return error ? path : canonical;
}

}  // namespace

Project Project::from_unit(const Unit& unit, Target target, const std::filesystem::path& dir) {
  Project project;
  project.unit = unit.unit;
  project.module = unit.module;
  project.source_file = unit.file;
  project.package_root = unit.package_root;
  project.source_lang = "python";
  project.target = std::move(target);
  project.functions = unit.functions;
  project.skipped = unit.skipped;
  project.risks = unit.risks;
  project.imports = unit.imports;
  project.lines = unit.lines;
  project.created_at = now_iso8601();
  project.dir = dir;
  return project;
}

Result<Project> Project::load(const std::filesystem::path& dir) {
  const std::filesystem::path path = dir / kMetaDir / kMetaFile;
  const auto text = read_file(path);
  if (!text.has_value()) {
    return std::unexpected(path.string() + ": cannot read (is this a cpp-anything project?)");
  }
  Json doc;
  try {
    doc = parse(*text);
  } catch (const JsonError& error) {
    return std::unexpected(std::string("project.json: ") + error.what());
  }
  const std::filesystem::path dir_absolute = canonical_or_self(dir);
  const Json* unit_doc = doc.find("unit");
  Project project;
  if (unit_doc != nullptr) {
    project.unit = text_of(*unit_doc, "name");
    project.module = text_of(*unit_doc, "module");
    const std::filesystem::path file = resolve_against(dir_absolute, text_of(*unit_doc, "file"));
    project.source_file = canonical_or_self(file);
    const std::string root = text_of(*unit_doc, "package_root");
    if (!root.empty()) {
      project.package_root = canonical_or_self(resolve_against(dir_absolute, root));
    }
    if (const Json* lines = unit_doc->find("lines");
        lines != nullptr && lines->is_integer()) {
      project.lines = static_cast<std::uint32_t>(lines->as_int());
    }
    project.source_lang = text_of(*unit_doc, "lang");
  }
  if (const Json* target = doc.find("target"); target != nullptr) {
    project.target.toolchain = text_of(*target, "toolchain");
    project.target.version = text_of(*target, "version");
    project.target.standard = text_of(*target, "std");
  }
  if (const Json* functions = doc.find("functions");
      functions != nullptr && functions->is_array()) {
    for (const Json& item : functions->as_array()) {
      auto function = Function::parse(item);
      if (!function) return std::unexpected(function.error());
      project.functions.push_back(std::move(*function));
    }
  }
  project.created_at = text_of(doc, "created_at");
  if (const Json* recorded = doc.find("recorded"); recorded != nullptr) {
    Recorded entry;
    if (const Json* cases = recorded->find("cases");
        cases != nullptr && cases->is_integer()) {
      entry.cases = static_cast<std::size_t>(cases->as_int());
    }
    entry.at = text_of(*recorded, "at");
    project.recorded = entry;
  }
  project.tolerance = number_of(doc, "float_tolerance").value_or(0.0);
  if (const Json* modules = doc.find("modules"); modules != nullptr && modules->is_bool()) {
    project.modules = modules->as_bool();
  }
  if (const Json* args = doc.find("node_args"); args != nullptr && args->is_array()) {
    for (const Json& item : args->as_array()) {
      if (item.is_string()) project.node_args.push_back(item.as_string());
    }
  }
  if (const Json* resolve = doc.find("resolve_from");
      resolve != nullptr && resolve->is_string()) {
    project.resolve_from = resolve->as_string();
  }
  if (const Json* module = doc.find("source_module");
      module != nullptr && module->is_string()) {
    project.source_module = module->as_string();
  }
  if (const Json* batches = doc.find("batches"); batches != nullptr && batches->is_array()) {
    for (const Json& item : batches->as_array()) {
      project.batches.push_back({text_of(item, "function"), text_of(item, "param")});
    }
  }
  project.dir = dir_absolute;
  return project;
}

Result<void> Project::save() const {
  const std::filesystem::path path = dir / kMetaDir / kMetaFile;
  const std::string text = dump(to_json(), true);
  auto written = write_file(path, text);
  if (!written) return std::unexpected(path.string() + ": " + written.error());
  return {};
}

Json Project::to_json() const {
  // Paths are stored relative to the project directory: a fixture or a moved
  // checkout still describes its source, and `load` resolves them back.
  const auto stored = [&](const std::filesystem::path& path) {
    return relative_path(dir, path).value_or(path).string();
  };
  Json::Array skipped_items;
  for (const auto& [name, reason] : skipped) {
    skipped_items.push_back(Json::object({{"name", Json::string(name)},
                                          {"reason", Json::string(reason)}}));
  }
  Json::Array batch_items;
  for (const Batch& batch : batches) {
    batch_items.push_back(Json::object({{"function", Json::string(batch.function)},
                                        {"param", Json::string(batch.param)}}));
  }
  Json::Array function_items;
  for (const Function& function : functions) function_items.push_back(function.to_json());
  Json::Array import_items;
  for (const std::string& name : imports) import_items.push_back(Json::string(name));
  Json::Array risk_items;
  for (const std::string& risk : risks) risk_items.push_back(Json::string(risk));
  Json::Array node_arg_items;
  for (const std::string& arg : node_args) node_arg_items.push_back(Json::string(arg));

  return Json::object({
      {"schema", Json::string(std::string(kProjectSchema))},
      {"created_at", Json::string(created_at)},
      {"unit", Json::object({
                   {"name", Json::string(unit)},
                   {"module", Json::string(module)},
                   {"file", Json::string(stored(source_file))},
                   {"package_root",
                    package_root.has_value() ? Json::string(stored(*package_root)) : Json::null()},
                   {"lang", Json::string(source_lang)},
                   {"lines", Json::integer(lines)},
               })},
      {"target", Json::object({
                     {"toolchain", Json::string(target.toolchain)},
                     {"version", Json::string(target.version)},
                     {"std", Json::string(target.standard)},
                 })},
      {"float_tolerance", Json::real(tolerance)},
      {"modules", Json::boolean(modules)},
      {"node_args", Json::array(std::move(node_arg_items))},
      {"resolve_from",
       resolve_from.has_value() ? Json::string(*resolve_from) : Json::null()},
      {"source_module",
       source_module.has_value() ? Json::string(*source_module) : Json::null()},
      {"batches", Json::array(std::move(batch_items))},
      {"functions", Json::array(std::move(function_items))},
      {"skipped", Json::array(std::move(skipped_items))},
      {"imports", Json::array(std::move(import_items))},
      {"risks", Json::array(std::move(risk_items))},
  });
}

}  // namespace cppan
