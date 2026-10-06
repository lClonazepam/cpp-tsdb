#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tsdb {

using Timestamp = std::int64_t;

struct Point {
  Timestamp ts = 0;
  double value = 0;
};

struct SeriesKey {
  std::string metric;
  std::map<std::string, std::string> tags;

  std::string canonical() const {
    std::string out = metric;
    out.push_back('{');
    bool first = true;
    for (const auto& [k, v] : tags) {
      if (!first) out.push_back(',');
      first = false;
      out += k;
      out.push_back('=');
      out += v;
    }
    out.push_back('}');
    return out;
  }
};

enum class Agg { Raw, Sum, Avg, Min, Max, Count };

struct Query {
  std::string metric;
  std::map<std::string, std::string> tags;
  Timestamp start = 0;
  Timestamp end = 0;
  Agg agg = Agg::Raw;
};

struct SeriesResult {
  std::string key;
  std::vector<Point> points;
  double aggregate = 0;
  std::uint64_t count = 0;
};

struct Stats {
  std::uint64_t series = 0;
  std::uint64_t points = 0;
  std::uint64_t segments = 0;
  std::uint64_t wal_bytes = 0;
  std::uint64_t segment_bytes = 0;
};

}  // namespace tsdb
