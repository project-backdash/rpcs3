#pragma once

// A bounded reader for the JSON files the launcher and the emulator exchange. Every document has a byte, node, depth and
// string budget, UTF-8 is validated, a duplicate key is refused and a number keeps its text, so each caller checks its own
// exact ranges.

#include "pbd_json.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pbd::json
{
	inline constexpr std::size_t max_file_bytes = 262144;
	inline constexpr std::size_t max_json_nodes = 8192;
	inline constexpr std::size_t max_json_depth = 16;
	inline constexpr std::uint64_t max_safe_integer = 9007199254740991ull;

	template <typename T>
	struct result
	{
		std::optional<T> value;
		std::string error;
	};

	enum class json_kind
	{
		null,
		boolean,
		number,
		string,
		array,
		object
	};

	struct json_value
	{
		json_kind kind = json_kind::null;
		bool truth = false;
		std::string text;
		std::vector<json_value> items;
		std::vector<std::pair<std::string, json_value>> fields;

		static json_value string(std::string value)
		{
			json_value out;
			out.kind = json_kind::string;
			out.text = std::move(value);
			return out;
		}
		static json_value number(std::uint64_t value)
		{
			json_value out;
			out.kind = json_kind::number;
			out.text = std::to_string(value);
			return out;
		}
		static json_value boolean(bool value)
		{
			json_value out;
			out.kind = json_kind::boolean;
			out.truth = value;
			return out;
		}
		static json_value object(std::vector<std::pair<std::string, json_value>> value = {})
		{
			json_value out;
			out.kind = json_kind::object;
			out.fields = std::move(value);
			return out;
		}
		static json_value array(std::vector<json_value> value = {})
		{
			json_value out;
			out.kind = json_kind::array;
			out.items = std::move(value);
			return out;
		}
		const json_value* find(std::string_view key) const
		{
			if (kind != json_kind::object)
				return nullptr;
			for (const auto& [name, value] : fields)
				if (name == key)
					return &value;
			return nullptr;
		}
	};

	namespace detail
	{
		inline bool valid_utf8(std::string_view text)
		{
			for (std::size_t i = 0; i < text.size();)
			{
				const auto first = static_cast<unsigned char>(text[i++]);
				if (first < 0x80)
					continue;
				std::uint32_t cp = 0;
				std::size_t tails = 0;
				std::uint32_t minimum = 0;
				if (first >= 0xc2 && first <= 0xdf)
				{
					cp = first & 0x1fu;
					tails = 1;
					minimum = 0x80;
				}
				else if (first >= 0xe0 && first <= 0xef)
				{
					cp = first & 0x0fu;
					tails = 2;
					minimum = 0x800;
				}
				else if (first >= 0xf0 && first <= 0xf4)
				{
					cp = first & 0x07u;
					tails = 3;
					minimum = 0x10000;
				}
				else
					return false;
				if (text.size() - i < tails)
					return false;
				for (std::size_t n = 0; n < tails; n++)
				{
					const auto tail = static_cast<unsigned char>(text[i++]);
					if ((tail & 0xc0u) != 0x80u)
						return false;
					cp = (cp << 6) | (tail & 0x3fu);
				}
				if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
					return false;
			}
			return true;
		}

		inline void append_utf8(std::string& text, std::uint32_t cp)
		{
			if (cp <= 0x7f)
				text.push_back(static_cast<char>(cp));
			else if (cp <= 0x7ff)
			{
				text.push_back(static_cast<char>(0xc0u | (cp >> 6)));
				text.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
			}
			else if (cp <= 0xffff)
			{
				text.push_back(static_cast<char>(0xe0u | (cp >> 12)));
				text.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
				text.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
			}
			else
			{
				text.push_back(static_cast<char>(0xf0u | (cp >> 18)));
				text.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3fu)));
				text.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
				text.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
			}
		}

		class json_parser
		{
			std::string_view source;
			std::size_t pos = 0;
			std::size_t nodes = 0;
			std::size_t node_budget = max_json_nodes;
			std::size_t byte_budget = max_file_bytes;
			std::size_t string_budget = 4096;
			std::string failure;

			bool bad(std::string code)
			{
				if (failure.empty())
					failure = std::move(code);
				return false;
			}
			void white()
			{
				while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\r' || source[pos] == '\n'))
					pos++;
			}
			bool take(char c)
			{
				white();
				if (pos < source.size() && source[pos] == c)
				{
					pos++;
					return true;
				}
				return false;
			}
			bool hex4(std::uint32_t& cp)
			{
				if (source.size() - pos < 4)
					return bad("invalid_unicode");
				cp = 0;
				for (std::size_t n = 0; n < 4; n++)
				{
					const char c = source[pos++];
					const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
					                                               c >= 'A' && c <= 'F'     ? c - 'A' + 10 :
					                                                                          -1;
					if (digit < 0)
						return bad("invalid_unicode");
					cp = (cp << 4) | static_cast<std::uint32_t>(digit);
				}
				return true;
			}
			bool string(std::string& out, std::size_t limit)
			{
				if (!take('"'))
					return bad("expected_string");
				while (pos < source.size())
				{
					const char c = source[pos++];
					if (c == '"')
						return valid_utf8(out) || bad("invalid_utf8");
					if (static_cast<unsigned char>(c) < 0x20)
						return bad("string_control");
					if (c != '\\')
						out.push_back(c);
					else
					{
						if (pos >= source.size())
							return bad("unterminated_escape");
						const char escaped = source[pos++];
						switch (escaped)
						{
						case '"':
						case '\\':
						case '/': out.push_back(escaped); break;
						case 'b': out.push_back('\b'); break;
						case 'f': out.push_back('\f'); break;
						case 'n': out.push_back('\n'); break;
						case 'r': out.push_back('\r'); break;
						case 't': out.push_back('\t'); break;
						case 'u':
						{
							std::uint32_t cp = 0;
							if (!hex4(cp))
								return false;
							if (cp >= 0xd800 && cp <= 0xdbff)
							{
								if (source.size() - pos < 6 || source[pos] != '\\' || source[pos + 1] != 'u')
									return bad("invalid_surrogate");
								pos += 2;
								std::uint32_t lo = 0;
								if (!hex4(lo) || lo < 0xdc00 || lo > 0xdfff)
									return bad("invalid_surrogate");
								cp = 0x10000 + ((cp - 0xd800) << 10) + lo - 0xdc00;
							}
							else if (cp >= 0xdc00 && cp <= 0xdfff)
								return bad("invalid_surrogate");
							append_utf8(out, cp);
							break;
						}
						default: return bad("invalid_escape");
						}
					}
					if (out.size() > limit)
						return bad("string_too_large");
				}
				return bad("unterminated_string");
			}
			bool number(json_value& out)
			{
				const std::size_t start = pos;
				if (source[pos] == '-')
					pos++;
				if (pos >= source.size())
					return bad("invalid_number");
				if (source[pos] == '0')
					pos++;
				else if (source[pos] >= '1' && source[pos] <= '9')
					while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9')
						pos++;
				else
					return bad("invalid_number");
				if (pos < source.size() && source[pos] == '.')
				{
					pos++;
					const std::size_t digits = pos;
					while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9')
						pos++;
					if (digits == pos)
						return bad("invalid_number");
				}
				if (pos < source.size() && (source[pos] == 'e' || source[pos] == 'E'))
				{
					pos++;
					if (pos < source.size() && (source[pos] == '+' || source[pos] == '-'))
						pos++;
					const std::size_t digits = pos;
					while (pos < source.size() && source[pos] >= '0' && source[pos] <= '9')
						pos++;
					if (digits == pos)
						return bad("invalid_number");
				}
				if (pos - start > 64)
					return bad("number_too_large");
				out.kind = json_kind::number;
				out.text = source.substr(start, pos - start);
				return true;
			}
			bool node(json_value& out, std::size_t depth)
			{
				if (++nodes > node_budget || depth > max_json_depth)
					return bad("json_limit");
				white();
				if (pos >= source.size())
					return bad("unexpected_end");
				const char c = source[pos];
				if (c == '"')
				{
					out.kind = json_kind::string;
					return string(out.text, string_budget);
				}
				if (c == '{')
				{
					pos++;
					out.kind = json_kind::object;
					if (take('}'))
						return true;
					for (;;)
					{
						std::string key;
						json_value value;
						if (!string(key, 4096) || !take(':'))
							return bad("invalid_object");
						if (out.find(key))
							return bad("duplicate_key");
						if (!node(value, depth + 1))
							return false;
						out.fields.emplace_back(std::move(key), std::move(value));
						if (take('}'))
							return true;
						if (!take(','))
							return bad("invalid_object");
					}
				}
				if (c == '[')
				{
					pos++;
					out.kind = json_kind::array;
					if (take(']'))
						return true;
					for (;;)
					{
						json_value value;
						if (!node(value, depth + 1))
							return false;
						out.items.push_back(std::move(value));
						if (take(']'))
							return true;
						if (!take(','))
							return bad("invalid_array");
					}
				}
				using literal = std::pair<std::string_view, json_kind>;
				constexpr std::array<literal, 3> literals{
					{{"null", json_kind::null}, {"true", json_kind::boolean}, {"false", json_kind::boolean}}};
				for (const auto& [word, kind] : literals)
				{
					if (source.substr(pos, word.size()) == word)
					{
						pos += word.size();
						out.kind = kind;
						out.truth = word == "true";
						return true;
					}
				}
				if (c == '-' || (c >= '0' && c <= '9'))
					return number(out);
				return bad("invalid_value");
			}

		public:
			explicit json_parser(std::string_view value, std::size_t limit = max_json_nodes, std::size_t bytes = max_file_bytes,
				std::size_t strings = 4096)
				: source(value), node_budget(limit), byte_budget(std::min(bytes, max_file_bytes * 64)),
				  string_budget(std::min(strings, byte_budget)) {}
			result<json_value> parse()
			{
				if (source.size() > byte_budget)
					return {{}, "file_too_large"};
				json_value value;
				if (!node(value, 0))
					return {{}, failure};
				white();
				if (pos != source.size())
					return {{}, "trailing_json"};
				return {std::move(value), {}};
			}
		};

		inline bool plain_text(const json_value* value, std::size_t min, std::size_t max)
		{
			return value && value->kind == json_kind::string && value->text.size() >= min && value->text.size() <= max &&
			       valid_utf8(value->text) && std::none_of(value->text.begin(), value->text.end(), [](unsigned char c)
												  {
													  return c < 0x20 || c == 0x7f;
												  });
		}
		inline bool identifier(const json_value* value)
		{
			return plain_text(value, 1, 64) && std::all_of(value->text.begin(), value->text.end(), [](unsigned char c)
												   {
													   return c >= 0x21 && c <= 0x7e;
												   });
		}
		inline bool is(const json_value* value, std::string_view expected)
		{
			return value && value->kind == json_kind::string && value->text == expected;
		}
		inline bool number(const json_value* value, std::uint64_t min, std::uint64_t max, std::uint64_t& out)
		{
			if (!value || value->kind != json_kind::number || value->text.empty())
				return false;
			const auto [tail, ec] = std::from_chars(value->text.data(), value->text.data() + value->text.size(), out);
			return ec == std::errc{} && tail == value->text.data() + value->text.size() && out >= min && out <= max;
		}
		inline bool number32(const json_value* value, std::uint32_t min, std::uint32_t max, std::uint32_t& out)
		{
			std::uint64_t wide = 0;
			if (!number(value, min, max, wide))
				return false;
			out = static_cast<std::uint32_t>(wide);
			return true;
		}
		inline bool boolean(const json_value* value, bool& out)
		{
			if (!value || value->kind != json_kind::boolean)
				return false;
			out = value->truth;
			return true;
		}
		inline std::string serialize(const json_value& value)
		{
			switch (value.kind)
			{
			case json_kind::null: return "null";
			case json_kind::boolean: return value.truth ? "true" : "false";
			case json_kind::number: return value.text;
			case json_kind::string: return pbd::json::quoted(value.text);
			case json_kind::array:
			{
				std::string out = "[";
				for (const auto& item : value.items)
				{
					if (out.size() > 1)
						out += ',';
					out += serialize(item);
				}
				return out + ']';
			}
			case json_kind::object:
			{
				std::string out = "{";
				for (const auto& [key, item] : value.fields)
				{
					if (out.size() > 1)
						out += ',';
					out += pbd::json::quoted(key) + ':' + serialize(item);
				}
				return out + '}';
			}
			}
			return {};
		}
		inline result<std::string> read_text(const std::filesystem::path& file)
		{
			std::error_code ec;
			const auto size = std::filesystem::file_size(file, ec);
			if (ec)
				return {{}, ec == std::errc::no_such_file_or_directory ? "unavailable" : "read_failed"};
			if (size > max_file_bytes)
				return {{}, "file_too_large"};
			std::ifstream stream(file, std::ios::binary);
			std::string text(static_cast<std::size_t>(size), '\0');
			if (!stream || (size && !stream.read(text.data(), static_cast<std::streamsize>(size))))
				return {{}, "read_failed"};
			return {std::move(text), {}};
		}
	} // namespace detail
} // namespace pbd::json
