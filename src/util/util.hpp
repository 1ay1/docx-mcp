// Small self-contained helpers: hashing, base64, ids, time, utf-8.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace docx::util {

using Bytes = std::vector<std::uint8_t>;

enum class HashAlg { SHA1, SHA256, SHA384, SHA512 };
Bytes hash(HashAlg alg, const std::uint8_t* data, std::size_t n);
inline Bytes hash(HashAlg alg, const Bytes& b) { return hash(alg, b.data(), b.size()); }
std::string hex(const Bytes& b, bool upper = false);

std::string base64_encode(const Bytes& b);
std::optional<Bytes> base64_decode(std::string_view s);

// random
std::uint32_t random_u32();
Bytes random_bytes(std::size_t n);
std::string new_guid();  // "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" upper case

// "2024-05-01T10:20:30Z" (Word's w:date format, seconds precision)
std::string now_iso8601();

// utf-8 <-> utf-32 / utf-16
std::u32string to_u32(std::string_view s);
std::string to_utf8(std::u32string_view s);
std::string to_utf8(char32_t c);
std::u16string to_u16(std::string_view s);
std::size_t u32_length(std::string_view s);

// Strip characters that can't appear in XML 1.0.
std::string xml_safe(std::string_view s);
// xml_safe plus &<>" escaped, for text spliced into XML source.
std::string xml_escape(std::string_view s);

std::string lower(std::string_view s);
std::string upper(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
bool starts_with(std::string_view s, std::string_view p);
std::string trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep);

// "unknown <what> 'k' (did you mean 'x'? valid: a, b, ...)"
std::string unknown_key(std::string_view what, std::string_view key, const std::vector<std::string_view>& valid);

// Numbers read from document markup: tolerate junk the way Word does
// (leading number wins, otherwise the default), never throw.
long to_long(std::string_view s, long def = 0);
inline int to_int(std::string_view s, int def = 0) { return (int)to_long(s, def); }
double to_double(std::string_view s, double def = 0);

// A length argument in twips: a number is points, or a string with a unit
// (pt, in, cm, mm, pc/pi, tw/twips, "1.5 in"). Throws a clear message on junk
// or absurd sizes (beyond 22 inches either way, Word's page limit).
long length_twips(const nlohmann::json& v);

}  // namespace docx::util
