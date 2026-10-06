#include "tsdb/codec.hpp"
#include <cstring>
#include <stdexcept>

namespace tsdb {
namespace {

void append_u32(std::string& o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void append_u64(std::string& o, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void append_varint(std::string& o, std::uint64_t v) {
  while (v >= 0x80) {
    o.push_back(static_cast<char>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  o.push_back(static_cast<char>(v));
}
std::uint64_t zigzag(std::int64_t v) {
  return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}
std::int64_t unzigzag(std::uint64_t v) {
  return static_cast<std::int64_t>((v >> 1) ^ (~(v & 1) + 1));
}

struct Reader {
  const std::string& s;
  std::size_t i = 0;
  explicit Reader(const std::string& in) : s(in) {}
  std::uint8_t u8() {
    if (i >= s.size()) throw std::runtime_error("codec truncated");
    return static_cast<std::uint8_t>(s[i++]);
  }
  std::uint32_t u32() {
    std::uint32_t v = 0;
    for (int b = 0; b < 4; ++b) v |= static_cast<std::uint32_t>(u8()) << (8 * b);
    return v;
  }
  std::uint64_t u64() {
    std::uint64_t v = 0;
    for (int b = 0; b < 8; ++b) v |= static_cast<std::uint64_t>(u8()) << (8 * b);
    return v;
  }
  std::uint64_t varint() {
    std::uint64_t v = 0;
    int shift = 0;
    for (int n = 0; n < 10; ++n) {
      auto b = u8();
      v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
      if ((b & 0x80) == 0) return v;
      shift += 7;
    }
    throw std::runtime_error("bad varint");
  }
};

std::uint64_t bitcast(double v) {
  std::uint64_t u = 0;
  static_assert(sizeof(double) == 8);
  std::memcpy(&u, &v, 8);
  return u;
}
double unbitcast(std::uint64_t u) {
  double v = 0;
  std::memcpy(&v, &u, 8);
  return v;
}

}  // namespace

std::string encode_points(const std::vector<Point>& points) {
  std::string o;
  append_u32(o, 0x31534254u);
  append_u32(o, static_cast<std::uint32_t>(points.size()));
  if (points.empty()) return o;
  append_u64(o, static_cast<std::uint64_t>(points[0].ts));
  append_u64(o, bitcast(points[0].value));
  std::int64_t prev_ts = points[0].ts;
  std::int64_t prev_delta = 0;
  std::uint64_t prev_bits = bitcast(points[0].value);
  for (std::size_t i = 1; i < points.size(); ++i) {
    const std::int64_t delta = points[i].ts - prev_ts;
    const std::int64_t dod = delta - prev_delta;
    append_varint(o, zigzag(dod));
    const auto bits = bitcast(points[i].value);
    append_u64(o, bits ^ prev_bits);
    prev_ts = points[i].ts;
    prev_delta = delta;
    prev_bits = bits;
  }
  return o;
}

std::vector<Point> decode_points(const std::string& blob) {
  Reader r(blob);
  if (r.u32() != 0x31534254u) throw std::runtime_error("bad codec magic");
  const auto n = r.u32();
  std::vector<Point> out;
  out.reserve(n);
  if (n == 0) return out;
  Point first;
  first.ts = static_cast<Timestamp>(r.u64());
  first.value = unbitcast(r.u64());
  out.push_back(first);
  std::int64_t prev_ts = first.ts;
  std::int64_t prev_delta = 0;
  std::uint64_t prev_bits = bitcast(first.value);
  for (std::uint32_t i = 1; i < n; ++i) {
    const auto dod = unzigzag(r.varint());
    const auto delta = prev_delta + dod;
    const auto bits = r.u64() ^ prev_bits;
    Point p;
    p.ts = prev_ts + delta;
    p.value = unbitcast(bits);
    out.push_back(p);
    prev_ts = p.ts;
    prev_delta = delta;
    prev_bits = bits;
  }
  return out;
}

}  // namespace tsdb
