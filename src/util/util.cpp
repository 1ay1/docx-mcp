#include "util/util.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>

namespace docx::util {

namespace {

// ── SHA-1 ────────────────────────────────────────────────────────────────
inline std::uint32_t rol32(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline std::uint32_t ror32(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline std::uint64_t ror64(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

template <class Word, std::size_t Block>
Bytes md_pad(const std::uint8_t* data, std::size_t n) {
    // Merkle–Damgård padding, big-endian length (64 or 128 bits).
    Bytes m(data, data + n);
    m.push_back(0x80);
    const std::size_t lenbytes = Block == 64 ? 8 : 16;
    while ((m.size() + lenbytes) % Block) m.push_back(0);
    std::uint64_t bits = std::uint64_t(n) * 8;
    if (lenbytes == 16) for (int i = 0; i < 8; i++) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back(std::uint8_t(bits >> (i * 8)));
    return m;
}

Bytes sha1(const std::uint8_t* data, std::size_t n) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    Bytes m = md_pad<std::uint32_t, 64>(data, n);
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (std::uint32_t(m[off + 4 * i]) << 24) | (std::uint32_t(m[off + 4 * i + 1]) << 16) |
                   (std::uint32_t(m[off + 4 * i + 2]) << 8) | m[off + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            std::uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    Bytes out;
    for (auto v : h) for (int i = 3; i >= 0; i--) out.push_back(std::uint8_t(v >> (i * 8)));
    return out;
}

constexpr std::uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

Bytes sha256(const std::uint8_t* data, std::size_t n) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Bytes m = md_pad<std::uint32_t, 64>(data, n);
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (std::uint32_t(m[off + 4 * i]) << 24) | (std::uint32_t(m[off + 4 * i + 1]) << 16) |
                   (std::uint32_t(m[off + 4 * i + 2]) << 8) | m[off + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            std::uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            std::uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
            std::uint32_t ch = (e & f) ^ (~e & g);
            std::uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
            std::uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
            std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    Bytes out;
    for (auto v : h) for (int i = 3; i >= 0; i--) out.push_back(std::uint8_t(v >> (i * 8)));
    return out;
}

constexpr std::uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

Bytes sha512_family(const std::uint8_t* data, std::size_t n, bool is384) {
    std::uint64_t h[8];
    if (is384) {
        const std::uint64_t iv[8] = {0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
                                     0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
                                     0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL};
        std::memcpy(h, iv, sizeof h);
    } else {
        const std::uint64_t iv[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                                     0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                     0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
        std::memcpy(h, iv, sizeof h);
    }
    Bytes m = md_pad<std::uint64_t, 128>(data, n);
    for (std::size_t off = 0; off < m.size(); off += 128) {
        std::uint64_t w[80];
        for (int i = 0; i < 16; i++) {
            std::uint64_t v = 0;
            for (int j = 0; j < 8; j++) v = (v << 8) | m[off + 8 * i + j];
            w[i] = v;
        }
        for (int i = 16; i < 80; i++) {
            std::uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            std::uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 80; i++) {
            std::uint64_t S1 = ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41);
            std::uint64_t ch = (e & f) ^ (~e & g);
            std::uint64_t t1 = hh + S1 + ch + K512[i] + w[i];
            std::uint64_t S0 = ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39);
            std::uint64_t mj = (a & b) ^ (a & c) ^ (b & c);
            std::uint64_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    Bytes out;
    int words = is384 ? 6 : 8;
    for (int k = 0; k < words; k++)
        for (int i = 7; i >= 0; i--) out.push_back(std::uint8_t(h[k] >> (i * 8)));
    return out;
}

std::mt19937_64& rng() {
    static std::mt19937_64 r([] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(),
                          unsigned(std::chrono::steady_clock::now().time_since_epoch().count())};
        return std::mt19937_64(seq);
    }());
    return r;
}
std::mutex rng_mu;

}  // namespace

Bytes hash(HashAlg alg, const std::uint8_t* data, std::size_t n) {
    switch (alg) {
        case HashAlg::SHA1: return sha1(data, n);
        case HashAlg::SHA256: return sha256(data, n);
        case HashAlg::SHA384: return sha512_family(data, n, true);
        case HashAlg::SHA512: return sha512_family(data, n, false);
    }
    return {};
}

std::string hex(const Bytes& b, bool upper) {
    const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string s;
    for (auto c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const Bytes& b) {
    std::string s;
    std::size_t i = 0;
    for (; i + 2 < b.size(); i += 3) {
        std::uint32_t v = (b[i] << 16) | (b[i + 1] << 8) | b[i + 2];
        s += B64[v >> 18]; s += B64[(v >> 12) & 63]; s += B64[(v >> 6) & 63]; s += B64[v & 63];
    }
    if (i + 1 == b.size()) {
        std::uint32_t v = b[i] << 16;
        s += B64[v >> 18]; s += B64[(v >> 12) & 63]; s += "==";
    } else if (i + 2 == b.size()) {
        std::uint32_t v = (b[i] << 16) | (b[i + 1] << 8);
        s += B64[v >> 18]; s += B64[(v >> 12) & 63]; s += B64[(v >> 6) & 63]; s += '=';
    }
    return s;
}

std::optional<Bytes> base64_decode(std::string_view s) {
    int map[256];
    for (int& m : map) m = -1;
    for (int i = 0; i < 64; i++) map[(unsigned char)B64[i]] = i;
    map[(unsigned char)'-'] = 62;  // url-safe variants
    map[(unsigned char)'_'] = 63;
    Bytes out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' ) break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        int v = map[(unsigned char)c];
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(std::uint8_t(acc >> bits)); }
    }
    return out;
}

std::uint32_t random_u32() {
    std::lock_guard lk(rng_mu);
    return std::uint32_t(rng()());
}

Bytes random_bytes(std::size_t n) {
    std::lock_guard lk(rng_mu);
    Bytes b(n);
    for (auto& c : b) c = std::uint8_t(rng()());
    return b;
}

std::string new_guid() {
    Bytes b = random_bytes(16);
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    std::string h = hex(b, true);
    return "{" + h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) +
           "-" + h.substr(20) + "}";
}

std::string now_iso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::u32string to_u32(std::string_view s) {
    std::u32string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; len = 4; }
        else { out.push_back(0xFFFD); i++; continue; }
        if (i + len > s.size()) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (int k = 1; k < len; k++) {
            unsigned char cc = s[i + k];
            if ((cc >> 6) != 2) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(0xFFFD); i++; continue; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string to_utf8(char32_t c) {
    std::string s;
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
        s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F));
    } else {
        s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F));
        s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F));
    }
    return s;
}

std::string to_utf8(std::u32string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) out += to_utf8(c);
    return out;
}

std::u16string to_u16(std::string_view s) {
    std::u16string out;
    for (char32_t c : to_u32(s)) {
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back(char16_t(0xD800 + (c >> 10)));
            out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}

std::size_t u32_length(std::string_view s) {
    std::size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

std::string xml_safe(std::string_view s) {
    std::u32string u = to_u32(s);
    std::u32string out;
    for (char32_t c : u) {
        bool ok = c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) ||
                  (c >= 0xE000 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0x10FFFF);
        if (ok) out.push_back(c);
    }
    return to_utf8(out);
}

std::string xml_escape(std::string_view s) {
    std::string o;
    for (char c : xml_safe(s)) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (auto& c : o) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return o;
}

std::string upper(std::string_view s) {
    std::string o(s);
    for (auto& c : o) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return o;
}

bool iequals(std::string_view a, std::string_view b) { return lower(a) == lower(b); }
bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n' || s[b] == '\r')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\n' || s[e - 1] == '\r')) e--;
    return std::string(s.substr(b, e - b));
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string unknown_key(std::string_view what, std::string_view key, const std::vector<std::string_view>& valid) {
    auto dist = [](std::string_view a, std::string_view b) {
        std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); j++) prev[j] = j;
        for (std::size_t i = 1; i <= a.size(); i++) {
            cur[0] = i;
            for (std::size_t j = 1; j <= b.size(); j++)
                cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            std::swap(prev, cur);
        }
        return prev[b.size()];
    };
    std::string k = lower(key), best;
    std::size_t bd = 3;
    for (auto v : valid) {
        std::size_t dd = dist(k, v);
        // also catch prefixes/suffixes: "margin" -> "cell_margins"
        if (dd > 2 && k.size() >= 4 && std::string_view(v).find(k) != std::string_view::npos) dd = 2;
        if (dd < bd) { bd = dd; best = v; }
    }
    std::string m = "unknown " + std::string(what) + " '" + std::string(key) + "'";
    m += " (";
    if (!best.empty()) m += "did you mean '" + best + "'? ";
    m += "valid: ";
    for (std::size_t i = 0; i < valid.size(); i++) m += (i ? ", " : "") + std::string(valid[i]);
    return m + ")";
}

long to_long(std::string_view s, long def) {
    double d = to_double(s, (double)def);
    if (!(d == d)) return def;
    // clamp to int range so callers can narrow safely
    if (d > 2147483647.0) return 2147483647L;
    if (d < -2147483648.0) return -2147483647L - 1;
    return (long)d;
}

double to_double(std::string_view s, double def) {
    std::string t(trim(s));
    if (t.empty()) return def;
    char* end = nullptr;
    errno = 0;
    double d = std::strtod(t.c_str(), &end);
    if (end == t.c_str() || errno == ERANGE || !(d == d)) return def;
    return d;
}

long length_twips(const nlohmann::json& v) {
    double tw;
    if (v.is_number()) {
        tw = v.get<double>() * 20;
    } else if (v.is_string()) {
        std::string s = lower(trim(v.get<std::string>()));
        char* end = nullptr;
        double n = std::strtod(s.c_str(), &end);
        std::string unit = trim(std::string_view(end));
        // strtod also takes "inf", "nan" and hex; only plain decimals are lengths
        bool plain = end != s.c_str() && s.find_first_of("xXnN") >= std::size_t(end - s.c_str());
        if (!plain)
            throw std::runtime_error("bad length '" + v.get<std::string>() + "' (use points, or a unit: 12pt, 1in, 2.5cm, 10mm)");
        static const std::pair<const char*, double> units[] = {
            {"", 20}, {"pt", 20}, {"in", 1440}, {"inch", 1440}, {"inches", 1440}, {"\"", 1440}, {"cm", 1440 / 2.54},
            {"mm", 144 / 2.54}, {"pc", 240}, {"pi", 240}, {"tw", 1}, {"twip", 1}, {"twips", 1}, {"dxa", 1}};
        double f = -1;
        for (auto& [u, k] : units)
            if (unit == u) f = k;
        if (f < 0) throw std::runtime_error("unknown length unit '" + unit + "' (pt, in, cm, mm, pc, tw)");
        tw = n * f;
    } else {
        throw std::runtime_error("a length must be a number of points or a string like \"1in\"");
    }
    if (!(tw == tw) || tw > 31680 || tw < -31680) throw std::runtime_error("length out of range (at most 22 inches)");
    return std::lround(tw);
}

}  // namespace docx::util
