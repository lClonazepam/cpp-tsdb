#include "tsdb/engine.hpp"
#include <iostream>
#include <string>

namespace {
void usage() {
  std::cerr << "tsdb write <dir> <metric> <ts> <value> [k=v...]\n"
            << "tsdb query <dir> <metric> <start> <end> <raw|sum|avg|min|max|count> [k=v...]\n"
            << "tsdb flush <dir>\n"
            << "tsdb stats <dir>\n";
}
tsdb::Agg parse_agg(const std::string& s) {
  if (s == "sum") return tsdb::Agg::Sum;
  if (s == "avg") return tsdb::Agg::Avg;
  if (s == "min") return tsdb::Agg::Min;
  if (s == "max") return tsdb::Agg::Max;
  if (s == "count") return tsdb::Agg::Count;
  return tsdb::Agg::Raw;
}
}

int main(int argc, char** argv) {
  if (argc < 3) { usage(); return 2; }
  std::string cmd = argv[1];
  tsdb::Engine db(argv[2], 2048);
  if (cmd == "stats") {
    auto s = db.stats();
    std::cout << "series=" << s.series << " points=" << s.points << " segments=" << s.segments
              << " wal_bytes=" << s.wal_bytes << " segment_bytes=" << s.segment_bytes << "\n";
    return 0;
  }
  if (cmd == "flush") { db.flush(); std::cout << "flushed\n"; return 0; }
  if (cmd == "write") {
    if (argc < 6) { usage(); return 2; }
    tsdb::SeriesKey key;
    key.metric = argv[3];
    for (int i = 6; i < argc; ++i) {
      std::string kv = argv[i];
      auto eq = kv.find('=');
      if (eq == std::string::npos) continue;
      key.tags.emplace(kv.substr(0, eq), kv.substr(eq + 1));
    }
    db.write(key, std::stoll(argv[4]), std::stod(argv[5]));
    std::cout << "ok " << key.canonical() << "\n";
    return 0;
  }
  if (cmd == "query") {
    if (argc < 7) { usage(); return 2; }
    tsdb::Query q;
    q.metric = argv[3];
    q.start = std::stoll(argv[4]);
    q.end = std::stoll(argv[5]);
    q.agg = parse_agg(argv[6]);
    for (int i = 7; i < argc; ++i) {
      std::string kv = argv[i];
      auto eq = kv.find('=');
      if (eq == std::string::npos) continue;
      q.tags.emplace(kv.substr(0, eq), kv.substr(eq + 1));
    }
    auto rows = db.query(q);
    for (const auto& row : rows) {
      std::cout << row.key << " count=" << row.count;
      if (q.agg != tsdb::Agg::Raw) std::cout << " agg=" << row.aggregate << "\n";
      else {
        std::cout << "\n";
        for (const auto& p : row.points) std::cout << "  " << p.ts << " " << p.value << "\n";
      }
    }
    return 0;
  }
  usage();
  return 2;
}
