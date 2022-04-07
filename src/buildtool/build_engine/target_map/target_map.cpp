#include "src/buildtool/build_engine/target_map/target_map.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "nlohmann/json.hpp"
#include "src/buildtool/build_engine/base_maps/field_reader.hpp"
#include "src/buildtool/build_engine/expression/configuration.hpp"
#include "src/buildtool/build_engine/expression/evaluator.hpp"
#include "src/buildtool/build_engine/expression/function_map.hpp"
#include "src/buildtool/build_engine/target_map/built_in_rules.hpp"
#include "src/buildtool/build_engine/target_map/utils.hpp"

namespace {

using namespace std::string_literals;

[[nodiscard]] auto ReadActionOutputExpr(ExpressionPtr const& out_exp,
                                        std::string const& field_name)
    -> ActionDescription::outputs_t {
    if (not out_exp->IsList()) {
        throw Evaluator::EvaluationError{
            fmt::format("{} has to be a list of strings, but found {}",
                        field_name,
                        out_exp->ToString())};
    }
    ActionDescription::outputs_t outputs;
    outputs.reserve(out_exp->List().size());
    for (auto const& out_path : out_exp->List()) {
        if (not out_path->IsString()) {
            throw Evaluator::EvaluationError{
                fmt::format("{} has to be a list of strings, but found {}",
                            field_name,
                            out_exp->ToString())};
        }
        outputs.emplace_back(out_path->String());
    }
    return outputs;
}

struct TargetData {
    using Ptr = std::shared_ptr<TargetData>;

    std::vector<std::string> target_vars;
    std::unordered_map<std::string, ExpressionPtr> config_exprs;
    std::unordered_map<std::string, ExpressionPtr> string_exprs;
    std::unordered_map<std::string, ExpressionPtr> target_exprs;
    ExpressionPtr tainted_expr;
    bool parse_target_names{};

    TargetData(std::vector<std::string> target_vars,
               std::unordered_map<std::string, ExpressionPtr> config_exprs,
               std::unordered_map<std::string, ExpressionPtr> string_exprs,
               std::unordered_map<std::string, ExpressionPtr> target_exprs,
               ExpressionPtr tainted_expr,
               bool parse_target_names)
        : target_vars{std::move(target_vars)},
          config_exprs{std::move(config_exprs)},
          string_exprs{std::move(string_exprs)},
          target_exprs{std::move(target_exprs)},
          tainted_expr{std::move(tainted_expr)},
          parse_target_names{parse_target_names} {}

    [[nodiscard]] static auto FromFieldReader(
        BuildMaps::Base::UserRulePtr const& rule,
        BuildMaps::Base::FieldReader::Ptr const& desc) -> TargetData::Ptr {
        desc->ExpectFields(rule->ExpectedFields());

        auto target_vars = desc->ReadStringList("arguments_config");
        auto tainted_expr =
            desc->ReadOptionalExpression("tainted", Expression::kEmptyList);

        auto convert_to_exprs =
            [&desc](gsl::not_null<
                        std::unordered_map<std::string, ExpressionPtr>*> const&
                        expr_map,
                    std::vector<std::string> const& field_names) -> bool {
            for (auto const& field_name : field_names) {
                auto expr = desc->ReadOptionalExpression(
                    field_name, Expression::kEmptyList);
                if (not expr) {
                    return false;
                }
                expr_map->emplace(field_name, std::move(expr));
            }
            return true;
        };

        std::unordered_map<std::string, ExpressionPtr> config_exprs;
        std::unordered_map<std::string, ExpressionPtr> string_exprs;
        std::unordered_map<std::string, ExpressionPtr> target_exprs;
        if (target_vars and tainted_expr and
            convert_to_exprs(&config_exprs, rule->ConfigFields()) and
            convert_to_exprs(&string_exprs, rule->StringFields()) and
            convert_to_exprs(&target_exprs, rule->TargetFields())) {
            return std::make_shared<TargetData>(std::move(*target_vars),
                                                std::move(config_exprs),
                                                std::move(string_exprs),
                                                std::move(target_exprs),
                                                std::move(tainted_expr),
                                                /*parse_target_names=*/true);
        }
        return nullptr;
    }

    [[nodiscard]] static auto FromTargetNode(
        BuildMaps::Base::UserRulePtr const& rule,
        TargetNode::Abstract const& node,
        ExpressionPtr const& rule_map,
        gsl::not_null<AsyncMapConsumerLoggerPtr> const& logger)
        -> TargetData::Ptr {

        auto const& string_fields = node.string_fields->Map();
        auto const& target_fields = node.target_fields->Map();

        std::unordered_map<std::string, ExpressionPtr> config_exprs;
        std::unordered_map<std::string, ExpressionPtr> string_exprs;
        std::unordered_map<std::string, ExpressionPtr> target_exprs;

        for (auto const& field_name : rule->ConfigFields()) {
            if (target_fields.Find(field_name)) {
                (*logger)(
                    fmt::format(
                        "Expected config field '{}' in string_fields of "
                        "abstract node type '{}', and not in target_fields",
                        field_name,
                        node.node_type),
                    /*fatal=*/true);
                return nullptr;
            }
            auto const& config_expr =
                string_fields.Find(field_name)
                    .value_or(std::reference_wrapper{Expression::kEmptyList})
                    .get();
            config_exprs.emplace(field_name, config_expr);
        }

        for (auto const& field_name : rule->StringFields()) {
            if (target_fields.Find(field_name)) {
                (*logger)(
                    fmt::format(
                        "Expected string field '{}' in string_fields of "
                        "abstract node type '{}', and not in target_fields",
                        field_name,
                        node.node_type),
                    /*fatal=*/true);
                return nullptr;
            }
            auto const& string_expr =
                string_fields.Find(field_name)
                    .value_or(std::reference_wrapper{Expression::kEmptyList})
                    .get();
            string_exprs.emplace(field_name, string_expr);
        }

        for (auto const& field_name : rule->TargetFields()) {
            if (string_fields.Find(field_name)) {
                (*logger)(
                    fmt::format(
                        "Expected target field '{}' in target_fields of "
                        "abstract node type '{}', and not in string_fields",
                        field_name,
                        node.node_type),
                    /*fatal=*/true);
                return nullptr;
            }
            auto const& target_expr =
                target_fields.Find(field_name)
                    .value_or(std::reference_wrapper{Expression::kEmptyList})
                    .get();
            auto const& nodes = target_expr->List();
            Expression::list_t targets{};
            targets.reserve(nodes.size());
            for (auto const& node_expr : nodes) {
                targets.emplace_back(ExpressionPtr{BuildMaps::Base::EntityName{
                    BuildMaps::Base::AnonymousTarget{rule_map, node_expr}}});
            }
            target_exprs.emplace(field_name, targets);
        }

        return std::make_shared<TargetData>(std::vector<std::string>{},
                                            std::move(config_exprs),
                                            std::move(string_exprs),
                                            std::move(target_exprs),
                                            Expression::kEmptyList,
                                            /*parse_target_names=*/false);
    }
};

void withDependencies(
    const std::vector<BuildMaps::Target::ConfiguredTarget>& transition_keys,
    const std::vector<AnalysedTargetPtr const*>& dependency_values,
    const BuildMaps::Base::UserRulePtr& rule,
    const TargetData::Ptr& data,
    const BuildMaps::Target::ConfiguredTarget& key,
    std::unordered_map<std::string, ExpressionPtr> params,
    const BuildMaps::Target::TargetMap::SetterPtr& setter,
    const BuildMaps::Target::TargetMap::LoggerPtr& logger,
    const gsl::not_null<BuildMaps::Target::ResultTargetMap*>& result_map) {
    // Associate dependency keys with values
    std::unordered_map<BuildMaps::Target::ConfiguredTarget, AnalysedTargetPtr>
        deps_by_transition;
    deps_by_transition.reserve(transition_keys.size());
    for (size_t i = 0; i < transition_keys.size(); ++i) {
        deps_by_transition.emplace(transition_keys[i], *dependency_values[i]);
    }

    // Compute the effective dependecy on config variables
    std::unordered_set<std::string> effective_vars;
    auto const& param_vars = data->target_vars;
    effective_vars.insert(param_vars.begin(), param_vars.end());
    auto const& config_vars = rule->ConfigVars();
    effective_vars.insert(config_vars.begin(), config_vars.end());
    for (auto const& [transition, target] : deps_by_transition) {
        for (auto const& x : target->Vars()) {
            if (not transition.config.VariableFixed(x)) {
                effective_vars.insert(x);
            }
        }
    }
    auto effective_conf = key.config.Prune(effective_vars);

    // Compute and verify taintedness
    auto tainted = std::set<std::string>{};
    auto got_tainted = BuildMaps::Target::Utils::getTainted(
        &tainted, key.config.Prune(param_vars), data->tainted_expr, logger);
    if (not got_tainted) {
        return;
    }
    tainted.insert(rule->Tainted().begin(), rule->Tainted().end());
    for (auto const& dep : dependency_values) {
        if (not std::includes(tainted.begin(),
                              tainted.end(),
                              (*dep)->Tainted().begin(),
                              (*dep)->Tainted().end())) {
            (*logger)(
                "Not tainted with all strings the dependencies are tainted "
                "with",
                true);
            return;
        }
    }

    // Evaluate string parameters
    auto string_fields_fcts =
        FunctionMap::MakePtr(FunctionMap::underlying_map_t{
            {"outs",
             [&deps_by_transition, &key](
                 auto&& eval, auto const& expr, auto const& env) {
                 return BuildMaps::Target::Utils::keys_expr(
                     BuildMaps::Target::Utils::obtainTargetByName(
                         eval, expr, env, key.target, deps_by_transition)
                         ->Artifacts());
             }},
            {"runfiles",
             [&deps_by_transition, &key](
                 auto&& eval, auto const& expr, auto const& env) {
                 return BuildMaps::Target::Utils::keys_expr(
                     BuildMaps::Target::Utils::obtainTargetByName(
                         eval, expr, env, key.target, deps_by_transition)
                         ->RunFiles());
             }}});
    auto param_config = key.config.Prune(param_vars);
    params.reserve(params.size() + rule->StringFields().size());
    for (auto const& field_name : rule->StringFields()) {
        auto const& field_exp = data->string_exprs[field_name];
        auto field_value = field_exp.Evaluate(
            param_config,
            string_fields_fcts,
            [&logger, &field_name](auto const& msg) {
                (*logger)(fmt::format("While evaluating string field {}:\n{}",
                                      field_name,
                                      msg),
                          true);
            });
        if (not field_value) {
            return;
        }
        if (not field_value->IsList()) {
            (*logger)(fmt::format("String field {} should be a list of "
                                  "strings, but found {}",
                                  field_name,
                                  field_value->ToString()),
                      true);
            return;
        }
        for (auto const& entry : field_value->List()) {
            if (not entry->IsString()) {
                (*logger)(fmt::format("String field {} should be a list of "
                                      "strings, but found entry {}",
                                      field_name,
                                      entry->ToString()),
                          true);
                return;
            }
        }
        params.emplace(field_name, std::move(field_value));
    }

    // Evaluate main expression
    auto expression_config = key.config.Prune(config_vars);
    std::vector<ActionDescription::Ptr> actions{};
    std::vector<std::string> blobs{};
    std::vector<Tree::Ptr> trees{};
    auto main_exp_fcts = FunctionMap::MakePtr(
        {{"FIELD",
          [&params](auto&& eval, auto const& expr, auto const& env) {
              auto name = eval(expr["name"], env);
              if (not name->IsString()) {
                  throw Evaluator::EvaluationError{
                      fmt::format("FIELD argument 'name' should evaluate to a "
                                  "string, but got {}",
                                  name->ToString())};
              }
              auto it = params.find(name->String());
              if (it == params.end()) {
                  throw Evaluator::EvaluationError{
                      fmt::format("FIELD '{}' unknown", name->String())};
              }
              return it->second;
          }},
         {"DEP_ARTIFACTS",
          [&deps_by_transition](
              auto&& eval, auto const& expr, auto const& env) {
              return BuildMaps::Target::Utils::obtainTarget(
                         eval, expr, env, deps_by_transition)
                  ->Artifacts();
          }},
         {"DEP_RUNFILES",
          [&deps_by_transition](
              auto&& eval, auto const& expr, auto const& env) {
              return BuildMaps::Target::Utils::obtainTarget(
                         eval, expr, env, deps_by_transition)
                  ->RunFiles();
          }},
         {"DEP_PROVIDES",
          [&deps_by_transition](
              auto&& eval, auto const& expr, auto const& env) {
              auto const& provided = BuildMaps::Target::Utils::obtainTarget(
                                         eval, expr, env, deps_by_transition)
                                         ->Provides();
              auto provider = eval(expr["provider"], env);
              auto provided_value = provided->At(provider->String());
              if (provided_value) {
                  return provided_value->get();
              }
              auto const& empty_list = Expression::kEmptyList;
              return eval(expr->Get("default", empty_list), env);
          }},
         {"ACTION",
          [&actions, &rule](auto&& eval, auto const& expr, auto const& env) {
              auto const& empty_map_exp = Expression::kEmptyMapExpr;
              auto inputs_exp = eval(expr->Get("inputs", empty_map_exp), env);
              if (not inputs_exp->IsMap()) {
                  throw Evaluator::EvaluationError{fmt::format(
                      "inputs has to be a map of artifacts, but found {}",
                      inputs_exp->ToString())};
              }
              for (auto const& [input_path, artifact] : inputs_exp->Map()) {
                  if (not artifact->IsArtifact()) {
                      throw Evaluator::EvaluationError{
                          fmt::format("inputs has to be a map of Artifacts, "
                                      "but found {} for {}",
                                      artifact->ToString(),
                                      input_path)};
                  }
              }
              auto conflict =
                  BuildMaps::Target::Utils::tree_conflict(inputs_exp);
              if (conflict) {
                  throw Evaluator::EvaluationError{
                      fmt::format("inputs conflicts on subtree {}", *conflict)};
              }

              Expression::map_t::underlying_map_t result;
              auto outputs = ReadActionOutputExpr(
                  eval(expr->Get("outs", Expression::list_t{}), env), "outs");
              auto output_dirs = ReadActionOutputExpr(
                  eval(expr->Get("out_dirs", Expression::list_t{}), env),
                  "out_dirs");
              if (outputs.empty() and output_dirs.empty()) {
                  throw Evaluator::EvaluationError{
                      "either outs or out_dirs must be specified for ACTION"};
              }

              std::sort(outputs.begin(), outputs.end());
              std::sort(output_dirs.begin(), output_dirs.end());
              std::vector<std::string> dups{};
              std::set_intersection(outputs.begin(),
                                    outputs.end(),
                                    output_dirs.begin(),
                                    output_dirs.end(),
                                    std::back_inserter(dups));
              if (not dups.empty()) {
                  throw Evaluator::EvaluationError{
                      "outs and out_dirs for ACTION must be disjoint"};
              }

              std::vector<std::string> cmd;
              auto cmd_exp = eval(expr->Get("cmd", Expression::list_t{}), env);
              if (not cmd_exp->IsList()) {
                  throw Evaluator::EvaluationError{fmt::format(
                      "cmd has to be a list of strings, but found {}",
                      cmd_exp->ToString())};
              }
              if (cmd_exp->List().empty()) {
                  throw Evaluator::EvaluationError{
                      "cmd must not be an empty list"};
              }
              cmd.reserve(cmd_exp->List().size());
              for (auto const& arg : cmd_exp->List()) {
                  if (not arg->IsString()) {
                      throw Evaluator::EvaluationError{fmt::format(
                          "cmd has to be a list of strings, but found {}",
                          cmd_exp->ToString())};
                  }
                  cmd.emplace_back(arg->String());
              }
              auto env_exp = eval(expr->Get("env", empty_map_exp), env);
              if (not env_exp->IsMap()) {
                  throw Evaluator::EvaluationError{
                      fmt::format("env has to be a map of string, but found {}",
                                  env_exp->ToString())};
              }
              for (auto const& [env_var, env_value] : env_exp->Map()) {
                  if (not env_value->IsString()) {
                      throw Evaluator::EvaluationError{fmt::format(
                          "env has to be a map of string, but found {}",
                          env_exp->ToString())};
                  }
              }
              auto may_fail_exp = expr->Get("may_fail", Expression::list_t{});
              if (not may_fail_exp->IsList()) {
                  throw Evaluator::EvaluationError{
                      fmt::format("may_fail has to be a list of "
                                  "strings, but found {}",
                                  may_fail_exp->ToString())};
              }
              for (auto const& entry : may_fail_exp->List()) {
                  if (not entry->IsString()) {
                      throw Evaluator::EvaluationError{
                          fmt::format("may_fail has to be a list of "
                                      "strings, but found {}",
                                      may_fail_exp->ToString())};
                  }
                  if (rule->Tainted().find(entry->String()) ==
                      rule->Tainted().end()) {
                      throw Evaluator::EvaluationError{
                          fmt::format("may_fail contains entry {} the the rule "
                                      "is not tainted with",
                                      entry->ToString())};
                  }
              }
              std::optional<std::string> may_fail = std::nullopt;
              if (not may_fail_exp->List().empty()) {
                  auto fail_msg =
                      eval(expr->Get("fail_message", "action failed"s), env);
                  if (not fail_msg->IsString()) {
                      throw Evaluator::EvaluationError{fmt::format(
                          "fail_message has to evalute to a string, but got {}",
                          fail_msg->ToString())};
                  }
                  may_fail = std::optional{fail_msg->String()};
              }
              auto no_cache_exp = expr->Get("no_cache", Expression::list_t{});
              if (not no_cache_exp->IsList()) {
                  throw Evaluator::EvaluationError{
                      fmt::format("no_cache has to be a list of"
                                  "strings, but found {}",
                                  no_cache_exp->ToString())};
              }
              for (auto const& entry : no_cache_exp->List()) {
                  if (not entry->IsString()) {
                      throw Evaluator::EvaluationError{
                          fmt::format("no_cache has to be a list of"
                                      "strings, but found {}",
                                      no_cache_exp->ToString())};
                  }
                  if (rule->Tainted().find(entry->String()) ==
                      rule->Tainted().end()) {
                      throw Evaluator::EvaluationError{
                          fmt::format("no_cache contains entry {} the the rule "
                                      "is not tainted with",
                                      entry->ToString())};
                  }
              }
              bool no_cache = not no_cache_exp->List().empty();
              auto action =
                  BuildMaps::Target::Utils::createAction(outputs,
                                                         output_dirs,
                                                         std::move(cmd),
                                                         env_exp,
                                                         may_fail,
                                                         no_cache,
                                                         inputs_exp);
              auto action_id = action->Id();
              actions.emplace_back(std::move(action));
              for (auto const& out : outputs) {
                  result.emplace(out,
                                 ExpressionPtr{ArtifactDescription{
                                     action_id, std::filesystem::path{out}}});
              }
              for (auto const& out : output_dirs) {
                  result.emplace(out,
                                 ExpressionPtr{ArtifactDescription{
                                     action_id, std::filesystem::path{out}}});
              }

              return ExpressionPtr{Expression::map_t{result}};
          }},
         {"BLOB",
          [&blobs](auto&& eval, auto const& expr, auto const& env) {
              auto data = eval(expr->Get("data", ""s), env);
              if (not data->IsString()) {
                  throw Evaluator::EvaluationError{
                      fmt::format("BLOB data has to be a string, but got {}",
                                  data->ToString())};
              }
              blobs.emplace_back(data->String());
              return ExpressionPtr{ArtifactDescription{
                  {ComputeHash(data->String()), data->String().size()},
                  ObjectType::File}};
          }},
         {"TREE",
          [&trees](auto&& eval, auto const& expr, auto const& env) {
              auto val = eval(expr->Get("$1", Expression::kEmptyMapExpr), env);
              if (not val->IsMap()) {
                  throw Evaluator::EvaluationError{
                      fmt::format("TREE argument has to be a map of artifacts, "
                                  "but found {}",
                                  val->ToString())};
              }
              std::unordered_map<std::string, ArtifactDescription> artifacts;
              artifacts.reserve(val->Map().size());
              for (auto const& [input_path, artifact] : val->Map()) {
                  if (not artifact->IsArtifact()) {
                      throw Evaluator::EvaluationError{fmt::format(
                          "TREE argument has to be a map of artifacts, "
                          "but found {} for {}",
                          artifact->ToString(),
                          input_path)};
                  }
                  auto norm_path = std::filesystem::path{input_path}
                                       .lexically_normal()
                                       .string();
                  if (norm_path == "." or norm_path.empty()) {
                      if (val->Map().size() > 1) {
                          throw Evaluator::EvaluationError{
                              "input path '.' or '' for TREE is only allowed "
                              "for trees with single input artifact"};
                      }
                      if (not artifact->Artifact().IsTree()) {
                          throw Evaluator::EvaluationError{
                              "input path '.' or '' for TREE must be tree "
                              "artifact"};
                      }
                      return artifact;
                  }
                  artifacts.emplace(std::move(norm_path), artifact->Artifact());
              }
              auto conflict = BuildMaps::Target::Utils::tree_conflict(val);
              if (conflict) {
                  throw Evaluator::EvaluationError{
                      fmt::format("TREE conflicts on subtree {}", *conflict)};
              }
              auto tree = std::make_shared<Tree>(std::move(artifacts));
              auto tree_id = tree->Id();
              trees.emplace_back(std::move(tree));
              return ExpressionPtr{ArtifactDescription{tree_id}};
          }},
         {"VALUE_NODE",
          [](auto&& eval, auto const& expr, auto const& env) {
              auto val = eval(expr->Get("$1", Expression::kNone), env);
              if (not val->IsResult()) {
                  throw Evaluator::EvaluationError{
                      "argument '$1' for VALUE_NODE not a RESULT type."};
              }
              return ExpressionPtr{TargetNode{std::move(val)}};
          }},
         {"ABSTRACT_NODE",
          [](auto&& eval, auto const& expr, auto const& env) {
              auto type = eval(expr->Get("node_type", Expression::kNone), env);
              if (not type->IsString()) {
                  throw Evaluator::EvaluationError{
                      "argument 'node_type' for ABSTRACT_NODE not a string."};
              }
              auto string_fields = eval(
                  expr->Get("string_fields", Expression::kEmptyMapExpr), env);
              if (not string_fields->IsMap()) {
                  throw Evaluator::EvaluationError{
                      "argument 'string_fields' for ABSTRACT_NODE not a map."};
              }
              auto target_fields = eval(
                  expr->Get("target_fields", Expression::kEmptyMapExpr), env);
              if (not target_fields->IsMap()) {
                  throw Evaluator::EvaluationError{
                      "argument 'target_fields' for ABSTRACT_NODE not a map."};
              }

              std::optional<std::string> dup_key{std::nullopt};
              auto check_entries =
                  [&dup_key](auto const& map,
                             auto const& type_check,
                             std::string const& fields_name,
                             std::string const& type_name,
                             std::optional<ExpressionPtr> const& disjoint_map =
                                 std::nullopt) {
                      for (auto const& [key, list] : map->Map()) {
                          if (not list->IsList()) {
                              throw Evaluator::EvaluationError{fmt::format(
                                  "value for key {} in argument '{}' for "
                                  "ABSTRACT_NODE is not a list.",
                                  key,
                                  fields_name)};
                          }
                          for (auto const& entry : list->List()) {
                              if (not type_check(entry)) {
                                  throw Evaluator::EvaluationError{fmt::format(
                                      "list entry for {} in argument '{}' for "
                                      "ABSTRACT_NODE is not a {}:\n{}",
                                      key,
                                      fields_name,
                                      type_name,
                                      entry->ToString())};
                              }
                          }
                          if (disjoint_map) {
                              if ((*disjoint_map)->Map().Find(key)) {
                                  dup_key = key;
                                  return;
                              }
                          }
                      }
                  };

              auto is_string = [](auto const& e) { return e->IsString(); };
              check_entries(string_fields,
                            is_string,
                            "string_fields",
                            "string",
                            target_fields);
              if (dup_key) {
                  throw Evaluator::EvaluationError{
                      fmt::format("string_fields and target_fields are not "
                                  "disjoint maps, found duplicate key: {}.",
                                  *dup_key)};
              }

              auto is_node = [](auto const& e) { return e->IsNode(); };
              check_entries(
                  target_fields, is_node, "target_fields", "target node");

              return ExpressionPtr{
                  TargetNode{TargetNode::Abstract{type->String(),
                                                  std::move(string_fields),
                                                  std::move(target_fields)}}};
          }},
         {"RESULT", [](auto&& eval, auto const& expr, auto const& env) {
              auto const& empty_map_exp = Expression::kEmptyMapExpr;
              auto artifacts = eval(expr->Get("artifacts", empty_map_exp), env);
              auto runfiles = eval(expr->Get("runfiles", empty_map_exp), env);
              auto provides = eval(expr->Get("provides", empty_map_exp), env);
              if (not artifacts->IsMap()) {
                  throw Evaluator::EvaluationError{fmt::format(
                      "artifacts has to be a map of artifacts, but found {}",
                      artifacts->ToString())};
              }
              for (auto const& [path, entry] : artifacts->Map()) {
                  if (not entry->IsArtifact()) {
                      throw Evaluator::EvaluationError{
                          fmt::format("artifacts has to be a map of artifacts, "
                                      "but found {} for {}",
                                      entry->ToString(),
                                      path)};
                  }
              }
              if (not runfiles->IsMap()) {
                  throw Evaluator::EvaluationError{fmt::format(
                      "runfiles has to be a map of artifacts, but found {}",
                      runfiles->ToString())};
              }
              for (auto const& [path, entry] : runfiles->Map()) {
                  if (not entry->IsArtifact()) {
                      throw Evaluator::EvaluationError{
                          fmt::format("runfiles has to be a map of artifacts, "
                                      "but found {} for {}",
                                      entry->ToString(),
                                      path)};
                  }
              }
              if (not provides->IsMap()) {
                  throw Evaluator::EvaluationError{
                      fmt::format("provides has to be a map, but found {}",
                                  provides->ToString())};
              }
              return ExpressionPtr{TargetResult{artifacts, provides, runfiles}};
          }}});

    auto result = rule->Expression()->Evaluate(
        expression_config, main_exp_fcts, [logger](auto const& msg) {
            (*logger)(
                fmt::format("While evaluating defining expression of rule:\n{}",
                            msg),
                true);
        });
    if (not result) {
        return;
    }
    if (not result->IsResult()) {
        (*logger)(fmt::format("Defining expression should evaluate to a "
                              "RESULT, but got: {}",
                              result->ToString()),
                  true);
        return;
    }
    auto analysis_result =
        std::make_shared<AnalysedTarget>((*std::move(result)).Result(),
                                         std::move(actions),
                                         std::move(blobs),
                                         std::move(trees),
                                         std::move(effective_vars),
                                         std::move(tainted));
    analysis_result =
        result_map->Add(key.target, effective_conf, std::move(analysis_result));
    (*setter)(std::move(analysis_result));
}

[[nodiscard]] auto isTransition(
    const ExpressionPtr& ptr,
    std::function<void(std::string const&)> const& logger) -> bool {
    if (not ptr->IsList()) {
        logger(fmt::format("expected list, but got {}", ptr->ToString()));
        return false;
    }
    if (not std::all_of(ptr->List().begin(),
                        ptr->List().end(),
                        [](auto const& entry) { return entry->IsMap(); })) {
        logger(fmt::format("expected list of dicts, but found {}",
                           ptr->ToString()));
        return false;
    }

    return true;
}

void withRuleDefinition(
    const BuildMaps::Base::UserRulePtr& rule,
    const TargetData::Ptr& data,
    const BuildMaps::Target::ConfiguredTarget& key,
    const BuildMaps::Target::TargetMap::SubCallerPtr& subcaller,
    const BuildMaps::Target::TargetMap::SetterPtr& setter,
    const BuildMaps::Target::TargetMap::LoggerPtr& logger,
    const gsl::not_null<BuildMaps::Target::ResultTargetMap*> result_map) {
    auto param_config = key.config.Prune(data->target_vars);

    // Evaluate the config_fields

    std::unordered_map<std::string, ExpressionPtr> params;
    params.reserve(rule->ConfigFields().size() + rule->TargetFields().size() +
                   rule->ImplicitTargetExps().size());
    for (auto field_name : rule->ConfigFields()) {
        auto const& field_expression = data->config_exprs[field_name];
        auto field_value = field_expression.Evaluate(
            param_config, {}, [&logger, &field_name](auto const& msg) {
                (*logger)(fmt::format("While evaluating config fieled {}:\n{}",
                                      field_name,
                                      msg),
                          true);
            });
        if (not field_value) {
            return;
        }
        if (not field_value->IsList()) {
            (*logger)(fmt::format("Config field {} should evaluate to a list "
                                  "of strings, but got{}",
                                  field_name,
                                  field_value->ToString()),
                      true);
            return;
        }
        for (auto const& entry : field_value->List()) {
            if (not entry->IsString()) {
                (*logger)(fmt::format("Config field {} should evaluate to a "
                                      "list of strings, but got{}",
                                      field_name,
                                      field_value->ToString()),
                          true);
                return;
            }
        }
        params.emplace(field_name, field_value);
    }

    // Evaluate config transitions

    auto config_trans_fcts = FunctionMap::MakePtr(
        "FIELD", [&params](auto&& eval, auto const& expr, auto const& env) {
            auto name = eval(expr["name"], env);
            if (not name->IsString()) {
                throw Evaluator::EvaluationError{
                    fmt::format("FIELD argument 'name' should evaluate to a "
                                "string, but got {}",
                                name->ToString())};
            }
            auto it = params.find(name->String());
            if (it == params.end()) {
                throw Evaluator::EvaluationError{
                    fmt::format("FIELD {} unknown", name->String())};
            }
            return it->second;
        });

    auto const& config_vars = rule->ConfigVars();
    auto expression_config = key.config.Prune(config_vars);

    std::unordered_map<std::string, ExpressionPtr> config_transitions;
    config_transitions.reserve(rule->TargetFields().size() +
                               rule->ImplicitTargets().size() +
                               rule->AnonymousDefinitions().size());
    for (auto const& target_field_name : rule->TargetFields()) {
        auto exp = rule->ConfigTransitions().at(target_field_name);
        auto transition_logger = [&logger,
                                  &target_field_name](auto const& msg) {
            (*logger)(
                fmt::format("While evaluating config transition for {}:\n{}",
                            target_field_name,
                            msg),
                true);
        };
        auto transition = exp->Evaluate(
            expression_config, config_trans_fcts, transition_logger);
        if (not transition) {
            return;
        }
        if (not isTransition(transition, transition_logger)) {
            return;
        }
        config_transitions.emplace(target_field_name, transition);
    }
    for (const auto& name_value : rule->ImplicitTargets()) {
        auto implicit_field_name = name_value.first;
        auto exp = rule->ConfigTransitions().at(implicit_field_name);
        auto transition_logger = [&logger,
                                  &implicit_field_name](auto const& msg) {
            (*logger)(fmt::format("While evaluating config transition for "
                                  "implicit {}:\n{}",
                                  implicit_field_name,
                                  msg),
                      true);
        };
        auto transition = exp->Evaluate(
            expression_config, config_trans_fcts, transition_logger);
        if (not transition) {
            return;
        }
        if (not isTransition(transition, transition_logger)) {
            return;
        }
        config_transitions.emplace(implicit_field_name, transition);
    }
    for (const auto& entry : rule->AnonymousDefinitions()) {
        auto const& anon_field_name = entry.first;
        auto exp = rule->ConfigTransitions().at(anon_field_name);
        auto transition_logger = [&logger, &anon_field_name](auto const& msg) {
            (*logger)(fmt::format("While evaluating config transition for "
                                  "anonymous {}:\n{}",
                                  anon_field_name,
                                  msg),
                      true);
        };
        auto transition = exp->Evaluate(
            expression_config, config_trans_fcts, transition_logger);
        if (not transition) {
            return;
        }
        if (not isTransition(transition, transition_logger)) {
            return;
        }
        config_transitions.emplace(anon_field_name, transition);
    }

    // Request dependencies

    std::unordered_map<std::string, std::vector<std::size_t>> anon_positions;
    anon_positions.reserve(rule->AnonymousDefinitions().size());
    for (auto const& [_, def] : rule->AnonymousDefinitions()) {
        anon_positions.emplace(def.target, std::vector<std::size_t>{});
    }

    std::vector<BuildMaps::Target::ConfiguredTarget> dependency_keys;
    std::vector<BuildMaps::Target::ConfiguredTarget> transition_keys;
    for (auto target_field_name : rule->TargetFields()) {
        auto const& deps_expression = data->target_exprs[target_field_name];
        auto deps_names = deps_expression.Evaluate(
            param_config, {}, [logger, target_field_name](auto const& msg) {
                (*logger)(
                    fmt::format("While evaluating target parameter {}:\n{}",
                                target_field_name,
                                msg),
                    true);
            });
        if (not deps_names->IsList()) {
            (*logger)(fmt::format("Target parameter {} should evaluate to a "
                                  "list, but got {}",
                                  target_field_name,
                                  deps_names->ToString()),
                      true);
            return;
        }
        Expression::list_t dep_target_exps;
        if (data->parse_target_names) {
            dep_target_exps.reserve(deps_names->List().size());
            for (const auto& dep_name : deps_names->List()) {
                auto target = BuildMaps::Base::ParseEntityNameFromExpression(
                    dep_name,
                    key.target,
                    [&logger, &target_field_name, &dep_name](
                        std::string const& parse_err) {
                        (*logger)(fmt::format("Parsing entry {} in target "
                                              "field {} failed with:\n{}",
                                              dep_name->ToString(),
                                              target_field_name,
                                              parse_err),
                                  true);
                    });
                if (not target) {
                    return;
                }
                dep_target_exps.emplace_back(ExpressionPtr{*target});
            }
        }
        else {
            dep_target_exps = deps_names->List();
        }
        auto anon_pos = anon_positions.find(target_field_name);
        auto const& transitions = config_transitions[target_field_name]->List();
        for (const auto& transition : transitions) {
            auto transitioned_config = key.config.Update(transition);
            for (const auto& dep : dep_target_exps) {
                if (anon_pos != anon_positions.end()) {
                    anon_pos->second.emplace_back(dependency_keys.size());
                }

                dependency_keys.emplace_back(
                    BuildMaps::Target::ConfiguredTarget{dep->Name(),
                                                        transitioned_config});
                transition_keys.emplace_back(
                    BuildMaps::Target::ConfiguredTarget{
                        dep->Name(), Configuration{transition}});
            }
        }
        params.emplace(target_field_name,
                       ExpressionPtr{std::move(dep_target_exps)});
    }
    for (auto const& [implicit_field_name, implicit_target] :
         rule->ImplicitTargets()) {
        auto anon_pos = anon_positions.find(implicit_field_name);
        auto transitions = config_transitions[implicit_field_name]->List();
        for (const auto& transition : transitions) {
            auto transitioned_config = key.config.Update(transition);
            for (const auto& dep : implicit_target) {
                if (anon_pos != anon_positions.end()) {
                    anon_pos->second.emplace_back(dependency_keys.size());
                }

                dependency_keys.emplace_back(
                    BuildMaps::Target::ConfiguredTarget{dep,
                                                        transitioned_config});
                transition_keys.emplace_back(
                    BuildMaps::Target::ConfiguredTarget{
                        dep, Configuration{transition}});
            }
        }
    }
    params.insert(rule->ImplicitTargetExps().begin(),
                  rule->ImplicitTargetExps().end());

    (*subcaller)(
        dependency_keys,
        [transition_keys = std::move(transition_keys),
         rule,
         data,
         key,
         params = std::move(params),
         setter,
         logger,
         result_map,
         subcaller,
         config_transitions = std::move(config_transitions),
         anon_positions =
             std::move(anon_positions)](auto const& values) mutable {
            // Now that all non-anonymous targets have been evaluated we can
            // read their provides map to construct and evaluate anonymous
            // targets.
            std::vector<BuildMaps::Target::ConfiguredTarget> anonymous_keys;
            for (auto const& [name, def] : rule->AnonymousDefinitions()) {
                Expression::list_t anon_names{};
                for (auto pos : anon_positions.at(def.target)) {
                    auto const& provider_value =
                        (*values[pos])->Provides()->Map().Find(def.provider);
                    if (not provider_value) {
                        (*logger)(
                            fmt::format("Provider {} in {} does not exist",
                                        def.provider,
                                        def.target),
                            true);
                        return;
                    }
                    auto const& exprs = provider_value->get();
                    if (not exprs->IsList()) {
                        (*logger)(fmt::format("Provider {} in {} must be list "
                                              "of target nodes but found: {}",
                                              def.provider,
                                              def.target,
                                              exprs->ToString()),
                                  true);
                        return;
                    }

                    auto const& list = exprs->List();
                    anon_names.reserve(anon_names.size() + list.size());
                    for (auto const& node : list) {
                        if (not node->IsNode()) {
                            (*logger)(
                                fmt::format("Entry in provider {} in {} must "
                                            "be target node but found: {}",
                                            def.provider,
                                            def.target,
                                            node->ToString()),
                                true);
                            return;
                        }
                        anon_names.emplace_back(BuildMaps::Base::EntityName{
                            BuildMaps::Base::AnonymousTarget{def.rule_map,
                                                             node}});
                    }
                }

                for (const auto& transition :
                     config_transitions.at(name)->List()) {
                    auto transitioned_config = key.config.Update(transition);
                    for (auto const& anon : anon_names) {
                        anonymous_keys.emplace_back(
                            BuildMaps::Target::ConfiguredTarget{
                                anon->Name(), transitioned_config});

                        transition_keys.emplace_back(
                            BuildMaps::Target::ConfiguredTarget{
                                anon->Name(), Configuration{transition}});
                    }
                }

                params.emplace(name, ExpressionPtr{std::move(anon_names)});
            }
            (*subcaller)(
                anonymous_keys,
                [dependency_values = values,
                 transition_keys = std::move(transition_keys),
                 rule,
                 data,
                 key,
                 params = std::move(params),
                 setter,
                 logger,
                 result_map](auto const& values) mutable {
                    // Join dependency values and anonymous values
                    dependency_values.insert(
                        dependency_values.end(), values.begin(), values.end());
                    withDependencies(transition_keys,
                                     dependency_values,
                                     rule,
                                     data,
                                     key,
                                     params,
                                     setter,
                                     logger,
                                     result_map);
                },
                logger);
        },
        logger);
}

void withTargetsFile(
    const BuildMaps::Target::ConfiguredTarget& key,
    const nlohmann::json& targets_file,
    const gsl::not_null<BuildMaps::Base::SourceTargetMap*>& source_target,
    const gsl::not_null<BuildMaps::Base::UserRuleMap*>& rule_map,
    const gsl::not_null<TaskSystem*>& ts,
    const BuildMaps::Target::TargetMap::SubCallerPtr& subcaller,
    const BuildMaps::Target::TargetMap::SetterPtr& setter,
    const BuildMaps::Target::TargetMap::LoggerPtr& logger,
    const gsl::not_null<BuildMaps::Target::ResultTargetMap*> result_map) {
    auto desc_it = targets_file.find(key.target.GetNamedTarget().name);
    if (desc_it == targets_file.end()) {
        // Not a defined taraget, treat as source target
        source_target->ConsumeAfterKeysReady(
            ts,
            {key.target},
            [setter](auto values) { (*setter)(AnalysedTargetPtr{*values[0]}); },
            [logger, target = key.target](auto const& msg, auto fatal) {
                (*logger)(fmt::format("While analysing target {} as implicit "
                                      "source target:\n{}",
                                      target.ToString(),
                                      msg),
                          fatal);
            });
    }
    else {
        nlohmann::json desc = *desc_it;
        auto rule_it = desc.find("type");
        if (rule_it == desc.end()) {
            (*logger)(
                fmt::format("No type specified in the definition of target {}",
                            key.target.ToString()),
                true);
            return;
        }
        // Handle built-in rule, if it is
        auto handled_as_builtin = BuildMaps::Target::HandleBuiltin(
            *rule_it, desc, key, subcaller, setter, logger, result_map);
        if (handled_as_builtin) {
            return;
        }

        // Not a built-in rule, so has to be a user rule
        auto rule_name = BuildMaps::Base::ParseEntityNameFromJson(
            *rule_it,
            key.target,
            [&logger, &rule_it, &key](std::string const& parse_err) {
                (*logger)(fmt::format("Parsing rule name {} for target {} "
                                      "failed with:\n{}",
                                      rule_it->dump(),
                                      key.target.ToString(),
                                      parse_err),
                          true);
            });
        if (not rule_name) {
            return;
        }
        auto desc_reader = BuildMaps::Base::FieldReader::CreatePtr(
            desc,
            key.target,
            fmt::format("{} target", rule_name->ToString()),
            logger);
        if (not desc_reader) {
            return;
        }
        rule_map->ConsumeAfterKeysReady(
            ts,
            {*rule_name},
            [desc = std::move(desc_reader),
             subcaller,
             setter,
             logger,
             key,
             result_map,
             rn = *rule_name](auto values) {
                auto data = TargetData::FromFieldReader(*values[0], desc);
                if (not data) {
                    (*logger)(fmt::format("Failed to read data from target {} "
                                          "with rule {}",
                                          key.target.ToString(),
                                          rn.ToString()),
                              /*fatal=*/true);
                    return;
                }
                withRuleDefinition(
                    *values[0],
                    data,
                    key,
                    subcaller,
                    setter,
                    std::make_shared<AsyncMapConsumerLogger>(
                        [logger, target = key.target, rn](auto const& msg,
                                                          auto fatal) {
                            (*logger)(
                                fmt::format("While analysing {} target {}:\n{}",
                                            rn.ToString(),
                                            target.ToString(),
                                            msg),
                                fatal);
                        }),
                    result_map);
            },
            [logger, target = key.target](auto const& msg, auto fatal) {
                (*logger)(fmt::format("While looking up rule for {}:\n{}",
                                      target.ToString(),
                                      msg),
                          fatal);
            });
    }
}

void withTargetNode(
    const BuildMaps::Target::ConfiguredTarget& key,
    const gsl::not_null<BuildMaps::Base::UserRuleMap*>& rule_map,
    const gsl::not_null<TaskSystem*>& ts,
    const BuildMaps::Target::TargetMap::SubCallerPtr& subcaller,
    const BuildMaps::Target::TargetMap::SetterPtr& setter,
    const BuildMaps::Target::TargetMap::LoggerPtr& logger,
    const gsl::not_null<BuildMaps::Target::ResultTargetMap*> result_map) {
    auto const& target_node =
        key.target.GetAnonymousTarget().target_node->Node();
    auto const& rule_mapping = key.target.GetAnonymousTarget().rule_map->Map();
    if (target_node.IsValue()) {
        // fixed value node, create analysed target from result
        auto const& val = target_node.GetValue();
        (*setter)(std::make_shared<AnalysedTarget>(
            AnalysedTarget{val->Result(), {}, {}, {}, {}, {}}));
    }
    else {
        // abstract target node, lookup rule and instantiate target
        auto const& abs = target_node.GetAbstract();
        auto rule_name = rule_mapping.Find(abs.node_type);
        if (not rule_name) {
            (*logger)(fmt::format(
                          "Cannot resolve type of node {} via rule map "
                          "{}",
                          target_node.ToString(),
                          key.target.GetAnonymousTarget().rule_map->ToString()),
                      /*fatal=*/true);
        }
        rule_map->ConsumeAfterKeysReady(
            ts,
            {rule_name->get()->Name()},
            [abs,
             subcaller,
             setter,
             logger,
             key,
             result_map,
             rn = rule_name->get()](auto values) {
                auto data = TargetData::FromTargetNode(
                    *values[0],
                    abs,
                    key.target.GetAnonymousTarget().rule_map,
                    logger);
                if (not data) {
                    (*logger)(fmt::format("Failed to read data from target {} "
                                          "with rule {}",
                                          key.target.ToString(),
                                          rn->ToString()),
                              /*fatal=*/true);
                    return;
                }
                withRuleDefinition(*values[0],
                                   data,
                                   key,
                                   subcaller,
                                   setter,
                                   std::make_shared<AsyncMapConsumerLogger>(
                                       [logger, target = key.target, rn](
                                           auto const& msg, auto fatal) {
                                           (*logger)(
                                               fmt::format("While analysing {} "
                                                           "target {}:\n{}",
                                                           rn->ToString(),
                                                           target.ToString(),
                                                           msg),
                                               fatal);
                                       }),
                                   result_map);
            },
            [logger, target = key.target](auto const& msg, auto fatal) {
                (*logger)(fmt::format("While looking up rule for {}:\n{}",
                                      target.ToString(),
                                      msg),
                          fatal);
            });
    }
}

void TreeTarget(
    const BuildMaps::Target::ConfiguredTarget& key,
    const gsl::not_null<TaskSystem*>& ts,
    const BuildMaps::Target::TargetMap::SubCallerPtr& subcaller,
    const BuildMaps::Target::TargetMap::SetterPtr& setter,
    const BuildMaps::Target::TargetMap::LoggerPtr& logger,
    const gsl::not_null<BuildMaps::Target::ResultTargetMap*>& result_map,
    const gsl::not_null<BuildMaps::Base::DirectoryEntriesMap*>&
        directory_entries) {
    const auto& target = key.target.GetNamedTarget();
    const auto dir_name = std::filesystem::path{target.module} / target.name;
    auto module_ = BuildMaps::Base::ModuleName{target.repository, dir_name};

    directory_entries->ConsumeAfterKeysReady(
        ts,
        {module_},
        [setter, subcaller, target, key, result_map, logger, dir_name](
            auto values) {
            // expected values.size() == 1
            const auto& dir_entries = *values[0];
            using BuildMaps::Target::ConfiguredTarget;

            std::vector<ConfiguredTarget> v;

            for (const auto& x : dir_entries.FilesIterator()) {
                v.emplace_back(
                    ConfiguredTarget{BuildMaps::Base::EntityName{
                                         target.repository,
                                         dir_name,
                                         x,
                                         BuildMaps::Base::ReferenceType::kFile},
                                     Configuration{}});
            }

            for (const auto& x : dir_entries.DirectoriesIterator()) {
                v.emplace_back(
                    ConfiguredTarget{BuildMaps::Base::EntityName{
                                         target.repository,
                                         dir_name,
                                         x,
                                         BuildMaps::Base::ReferenceType::kTree},
                                     Configuration{}});
            }
            (*subcaller)(
                std::move(v),
                [setter, key, result_map, name = target.name](
                    const auto& values) mutable {
                    std::unordered_map<std::string, ArtifactDescription>
                        artifacts;

                    artifacts.reserve(values.size());

                    for (const auto& x : values) {
                        auto val = x->get()->RunFiles();

                        auto const& [input_path, artifact] =
                            *(val->Map().begin());
                        auto norm_path = std::filesystem::path{input_path}
                                             .lexically_normal()
                                             .string();

                        artifacts.emplace(std::move(norm_path),
                                          artifact->Artifact());
                    }

                    auto tree = std::make_shared<Tree>(std::move(artifacts));
                    auto tree_id = tree->Id();
                    auto tree_map = ExpressionPtr{Expression::map_t{
                        name, ExpressionPtr{ArtifactDescription{tree_id}}}};
                    auto analysis_result = std::make_shared<AnalysedTarget>(
                        TargetResult{tree_map, {}, tree_map},
                        std::vector<ActionDescription::Ptr>{},
                        std::vector<std::string>{},
                        std::vector<Tree::Ptr>{tree},
                        std::unordered_set<std::string>{},
                        std::set<std::string>{});
                    analysis_result = result_map->Add(
                        key.target, {}, std::move(analysis_result));
                    (*setter)(std::move(analysis_result));
                },
                logger);
        },
        [logger, target = key.target](auto const& msg, bool fatal) {
            (*logger)(fmt::format("While analysing entries of {}: {}",
                                  target.ToString(),
                                  msg),
                      fatal);
        });
}

}  // namespace

namespace BuildMaps::Target {
auto CreateTargetMap(
    const gsl::not_null<BuildMaps::Base::SourceTargetMap*>& source_target_map,
    const gsl::not_null<BuildMaps::Base::TargetsFileMap*>& targets_file_map,
    const gsl::not_null<BuildMaps::Base::UserRuleMap*>& rule_map,
    const gsl::not_null<BuildMaps::Base::DirectoryEntriesMap*>&
        directory_entries_map,
    const gsl::not_null<ResultTargetMap*>& result_map,
    std::size_t jobs) -> TargetMap {
    auto target_reader =
        [source_target_map,
         targets_file_map,
         rule_map,
         result_map,
         directory_entries_map](
            auto ts, auto setter, auto logger, auto subcaller, auto key) {
            if (key.target.IsAnonymousTarget()) {
                withTargetNode(
                    key, rule_map, ts, subcaller, setter, logger, result_map);
            }
            else if (key.target.GetNamedTarget().reference_t ==
                     BuildMaps::Base::ReferenceType::kTree) {

                auto wrapped_logger = std::make_shared<AsyncMapConsumerLogger>(
                    [logger, target = key.target](auto const& msg, bool fatal) {
                        (*logger)(fmt::format("While analysing {} as explicit "
                                              "tree reference:\n{}",
                                              target.ToString(),
                                              msg),
                                  fatal);
                    });
                TreeTarget(key,
                           ts,
                           subcaller,
                           setter,
                           wrapped_logger,
                           result_map,
                           directory_entries_map);
            }
            else if (key.target.GetNamedTarget().reference_t ==
                     BuildMaps::Base::ReferenceType::kFile) {
                // Not a defined target, treat as source target
                source_target_map->ConsumeAfterKeysReady(
                    ts,
                    {key.target},
                    [setter](auto values) {
                        (*setter)(AnalysedTargetPtr{*values[0]});
                    },
                    [logger, target = key.target](auto const& msg, auto fatal) {
                        (*logger)(fmt::format("While analysing target {} as "
                                              "explicit source target:\n{}",
                                              target.ToString(),
                                              msg),
                                  fatal);
                    });
            }
            else {
                targets_file_map->ConsumeAfterKeysReady(
                    ts,
                    {key.target.ToModule()},
                    [key,
                     source_target_map,
                     rule_map,
                     ts,
                     subcaller = std::move(subcaller),
                     setter = std::move(setter),
                     logger,
                     result_map](auto values) {
                        withTargetsFile(key,
                                        *values[0],
                                        source_target_map,
                                        rule_map,
                                        ts,
                                        subcaller,
                                        setter,
                                        logger,
                                        result_map);
                    },
                    [logger, target = key.target](auto const& msg, auto fatal) {
                        (*logger)(fmt::format("While searching targets "
                                              "description for {}:\n{}",
                                              target.ToString(),
                                              msg),
                                  fatal);
                    });
            }
        };
    return AsyncMapConsumer<ConfiguredTarget, AnalysedTargetPtr>(target_reader,
                                                                 jobs);
}
}  // namespace BuildMaps::Target
