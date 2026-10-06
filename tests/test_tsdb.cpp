#include "tsdb/codec.hpp"
#include "tsdb/engine.hpp"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

int main() {
  std::vector<tsdb::Point> pts;
  for (int i = 0; i < 500; ++i) pts.push_back({1700000000000LL + i * 1000, 20.0 + (i % 7) * 0.25});
  auto blob = tsdb::encode_points(pts);
  auto back = tsdb::decode_points(blob);
  assert(back.size() == pts.size());
  for (std::size_t i = 0; i < pts.size(); ++i) {
    assert(back[i].ts == pts[i].ts);
    assert(back[i].value == pts[i].value);
  }
  assert(blob.size() < pts.size() * 16);

  const auto dir = std::filesystem::temp_directory_path() / "cpp-tsdb-test";
  std::filesystem::remove_all(dir);
  {
    tsdb::Engine db(dir, 100);
    tsdb::SeriesKey a{.metric = "cpu", .tags = {{"host", "a"}, {"dc", "sjc"}}};
    tsdb::SeriesKey b{.metric = "cpu", .tags = {{"host", "b"}, {"dc", "sjc"}}};
    for (int i = 0; i < 250; ++i) {
      db.write(a, 1000 + i, 1.0 + i);
      db.write(b, 1000 + i, 10.0);
    }
    db.flush();
    db.write(a, 1100, 42);
  }
  tsdb::Engine db(dir, 100);
  tsdb::Query q;
  q.metric = "cpu";
  q.tags = {{"host", "a"}};
  q.start = 1000;
  q.end = 3000;
  q.agg = tsdb::Agg::Raw;
  auto rows = db.query(q);
  assert(rows.size() == 1);
  assert(rows[0].count == 250);
  bool found = false;
  for (const auto& p : rows[0].points) if (p.ts == 1100) { assert(p.value == 42); found = true; }
  assert(found);
  q.agg = tsdb::Agg::Avg;
  q.start = 1000;
  q.end = 1009;
  rows = db.query(q);
  assert(rows.size() == 1);
  assert(std::abs(rows[0].aggregate - 5.5) < 1e-9);
  q.tags = {{"dc", "sjc"}};
  q.agg = tsdb::Agg::Count;
  q.start = 0;
  q.end = 100000;
  rows = db.query(q);
  assert(rows.size() == 2);
  auto st = db.stats();
  assert(st.series == 2);
  assert(st.segments >= 1);
  std::cout << "tsdb ok series=" << st.series << " points=" << st.points
            << " segments=" << st.segments << " codec=" << blob.size() << "\n";
}
