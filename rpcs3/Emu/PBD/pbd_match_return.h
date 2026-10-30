#pragma once

#include "pbd_json_reader.h"
#include <filesystem>
#include <map>

namespace pbd::match_return
{
  struct request
  {
    std::string checkpoint, config, channel;
    std::map<std::string, std::string> environment;
  };
  inline json::result<request> parse(std::string_view text, std::string_view generation,
    std::string_view checkpoint_sha, std::string_view checkpoint_path)
  {
    using namespace json;
    if (text.size() > 65536) return {{}, "return_too_large"};
    auto parsed = detail::json_parser(text).parse();
    if (!parsed.value) return {{}, "return_invalid_json"};
    const auto& row = *parsed.value;
    const auto* env = row.find("environment");
    if (row.kind != json_kind::object || row.fields.size() != 7 ||
      !detail::is(row.find("schema"), "pbd-match-return-1") ||
      !detail::is(row.find("nativeGeneration"), generation) ||
      !detail::is(row.find("checkpointSha256"), checkpoint_sha) ||
      !detail::is(row.find("checkpoint"), checkpoint_path) ||
      !detail::plain_text(row.find("config"), 1, 4096) || !detail::plain_text(row.find("channel"), 1, 4096) ||
      !env || env->kind != json_kind::object || env->fields.size() > 256)
      return {{}, "return_identity_invalid"};
    request out{row.find("checkpoint")->text, row.find("config")->text, row.find("channel")->text, {}};
    const auto absolute = [](const std::string& value) { return std::filesystem::u8path(value).is_absolute(); };
    if (!absolute(out.checkpoint) || !absolute(out.config) || !absolute(out.channel))
      return {{}, "return_path_invalid"};
    for (const auto& [name, value] : env->fields)
    {
      if (!name.starts_with("PBD_") || name.starts_with("PBD_PEER_AUTH_") ||
        name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789") != name.npos ||
        value.kind != json_kind::string || value.text.size() > 8192 || value.text.find('\0') != value.text.npos)
        return {{}, "return_environment_invalid"};
      out.environment.emplace(name, value.text);
    }
    if (out.environment["PBD_MATCH_STANDBY_CHANNEL"] != out.channel ||
      out.environment.contains("PBD_PEER_ROLE") || out.environment.contains("PBD_MATCH_SEED") ||
      out.environment.contains("PBD_SET_CHANNEL") || out.environment.contains("PBD_SPECTATOR_CHANNEL"))
      return {{}, "return_not_standby"};
    return {std::move(out), {}};
  }
}
