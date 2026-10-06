#include "tsdb/engine.hpp"
#include "tsdb/codec.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tsdb {
namespace {

void append_u32(std::string& o, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
std::uint32_t read_u32(std::istream& in) {
  char b[4];
  in.read(b, 4);
  if (!in) throw std::runtime_error("truncated segment");
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(b[i])) << (8 * i);
  return v;
}
std::string read_str(std::istream& in) {
  const auto n = read_u32(in);
  std::string s(n, '\0');
  in.read(s.data(), static_cast<std::streamsize>(n));
  if (!in) throw std::runtime_error("truncated string");
  return s;
}
std::string escape_field(std::string s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '|' || c == '\n') o.push_back('\\');
    o.push_back(c);
  }
  return o;
}
std::string unescape_field(const std::string& s) {
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) o.push_back(s[++i]);
    else o.push_back(s[i]);
  }
  return o;
}

}  // namespace

Engine::Engine(std::filesystem::path root, std::size_t flush_points)
    : root_(std::move(root)), flush_points_(flush_points == 0 ? 1 : flush_points) {
  std::filesystem::create_directories(root_ / "segments");
  recover();
}
Engine::~Engine() { try { flush(); } catch (...) {} }

void Engine::write(const SeriesKey& key, Timestamp ts, double value) {
  std::lock_guard lock(mu_);
  append_wal(key, ts, value);
  const auto id_key = key.canonical();
  auto it = series_index_.find(id_key);
  std::uint32_t id = 0;
  if (it == series_index_.end()) {
    id = static_cast<std::uint32_t>(series_keys_.size());
    series_keys_.push_back(id_key);
    series_index_.emplace(id_key, id);
    for (const auto& [k, v] : key.tags) tag_index_[k + "=" + v].push_back(id);
    tag_index_["__metric=" + key.metric].push_back(id);
  } else id = it->second;
  auto& bucket = mem_[id].points;
  if (!bucket.empty() && ts < bucket.back().ts) {
    auto pos = std::lower_bound(bucket.begin(), bucket.end(), ts, [](const Point& p, Timestamp t) { return p.ts < t; });
    if (pos != bucket.end() && pos->ts == ts) pos->value = value;
    else { bucket.insert(pos, Point{ts, value}); ++mem_points_; }
  } else if (!bucket.empty() && bucket.back().ts == ts) bucket.back().value = value;
  else { bucket.push_back(Point{ts, value}); ++mem_points_; }
  if (mem_points_ >= flush_points_) write_segment_locked();
}

void Engine::write_batch(const SeriesKey& key, const std::vector<Point>& points) {
  for (const auto& p : points) write(key, p.ts, p.value);
}

void Engine::append_wal(const SeriesKey& key, Timestamp ts, double value) {
  std::ofstream out(root_ / "wal.log", std::ios::app);
  if (!out) throw std::runtime_error("wal open failed");
  out << escape_field(key.metric) << '|' << ts << '|' << value;
  for (const auto& [k, v] : key.tags) out << '|' << escape_field(k) << '=' << escape_field(v);
  out << '\n';
  wal_bytes_ += 32;
  dirty_wal_ = true;
}

void Engine::write_segment_locked() {
  if (mem_.empty()) return;
  std::string blob;
  append_u32(blob, 0x31475354u);
  append_u32(blob, static_cast<std::uint32_t>(mem_.size()));
  for (auto& [id, series] : mem_) {
    std::sort(series.points.begin(), series.points.end(), [](const Point& a, const Point& b) { return a.ts < b.ts; });
    append_u32(blob, id);
    append_u32(blob, static_cast<std::uint32_t>(series_keys_[id].size()));
    blob += series_keys_[id];
    auto encoded = encode_points(series.points);
    append_u32(blob, static_cast<std::uint32_t>(encoded.size()));
    blob += encoded;
    auto& dest = sealed_[id];
    dest.insert(dest.end(), series.points.begin(), series.points.end());
    std::stable_sort(dest.begin(), dest.end(), [](const Point& a, const Point& b) { return a.ts < b.ts; });
    std::vector<Point> merged;
    for (const auto& pt : dest) {
      if (!merged.empty() && merged.back().ts == pt.ts) merged.back() = pt;
      else merged.push_back(pt);
    }
    dest.swap(merged);
  }
  const auto path = root_ / "segments" / (std::to_string(++segments_) + ".seg");
  std::ofstream out(path, std::ios::binary);
  out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
  segment_bytes_ += blob.size();
  mem_.clear();
  mem_points_ = 0;
  std::ofstream wal(root_ / "wal.log", std::ios::trunc);
  wal_bytes_ = 0;
  dirty_wal_ = false;
}

void Engine::load_segment(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (read_u32(in) != 0x31475354u) throw std::runtime_error("bad segment magic");
  const auto n = read_u32(in);
  for (std::uint32_t i = 0; i < n; ++i) {
    const auto id = read_u32(in);
    const auto key = read_str(in);
    const auto blob_len = read_u32(in);
    std::string blob(blob_len, '\0');
    in.read(blob.data(), static_cast<std::streamsize>(blob_len));
    if (id >= series_keys_.size()) series_keys_.resize(id + 1);
    if (series_keys_[id].empty()) {
      series_keys_[id] = key;
      series_index_[key] = id;
    }
    auto points = decode_points(blob);
    auto& dest = sealed_[id];
    dest.insert(dest.end(), points.begin(), points.end());
  }
  segment_bytes_ += std::filesystem::file_size(path);
}

void Engine::recover() {
  std::vector<std::filesystem::path> segs;
  for (const auto& ent : std::filesystem::directory_iterator(root_ / "segments")) {
    if (ent.path().extension() == ".seg") segs.push_back(ent.path());
  }
  std::sort(segs.begin(), segs.end());
  for (const auto& p : segs) load_segment(p);
  segments_ = segs.size();
  for (auto& [id, pts] : sealed_) {
    std::stable_sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a.ts < b.ts; });
    std::vector<Point> merged;
    for (const auto& pt : pts) {
      if (!merged.empty() && merged.back().ts == pt.ts) merged.back() = pt;
      else merged.push_back(pt);
    }
    pts.swap(merged);
  }
  for (std::uint32_t id = 0; id < series_keys_.size(); ++id) {
    const auto& key = series_keys_[id];
    if (key.empty()) continue;
    series_index_[key] = id;
    auto brace = key.find('{');
    auto metric = key.substr(0, brace);
    tag_index_["__metric=" + metric].push_back(id);
    if (brace != std::string::npos && key.size() > brace + 2) {
      auto body = key.substr(brace + 1, key.size() - brace - 2);
      std::stringstream ss(body);
      std::string part;
      while (std::getline(ss, part, ',')) if (!part.empty()) tag_index_[part].push_back(id);
    }
  }
  std::ifstream wal(root_ / "wal.log");
  std::string line;
  while (std::getline(wal, line)) {
    if (line.empty()) continue;
    std::vector<std::string> fields;
    std::string cur;
    for (std::size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '\\' && i + 1 < line.size()) { cur.push_back(line[++i]); continue; }
      if (line[i] == '|') { fields.push_back(cur); cur.clear(); }
      else cur.push_back(line[i]);
    }
    fields.push_back(cur);
    if (fields.size() < 3) continue;
    SeriesKey key;
    key.metric = unescape_field(fields[0]);
    Timestamp ts = std::stoll(fields[1]);
    double value = std::stod(fields[2]);
    for (std::size_t i = 3; i < fields.size(); ++i) {
      auto eq = fields[i].find('=');
      if (eq == std::string::npos) continue;
      key.tags.emplace(unescape_field(fields[i].substr(0, eq)), unescape_field(fields[i].substr(eq + 1)));
    }
    const auto id_key = key.canonical();
    auto it = series_index_.find(id_key);
    std::uint32_t id = 0;
    if (it == series_index_.end()) {
      id = static_cast<std::uint32_t>(series_keys_.size());
      series_keys_.push_back(id_key);
      series_index_.emplace(id_key, id);
      tag_index_["__metric=" + key.metric].push_back(id);
      for (const auto& [k, v] : key.tags) tag_index_[k + "=" + v].push_back(id);
    } else id = it->second;
    mem_[id].points.push_back(Point{ts, value});
    ++mem_points_;
  }
  if (std::filesystem::exists(root_ / "wal.log")) wal_bytes_ = std::filesystem::file_size(root_ / "wal.log");
}

std::vector<std::uint32_t> Engine::match_locked(const Query& q) const {
  auto it = tag_index_.find("__metric=" + q.metric);
  if (it == tag_index_.end()) return {};
  std::vector<std::uint32_t> ids = it->second;
  for (const auto& [k, v] : q.tags) {
    auto tag = tag_index_.find(k + "=" + v);
    if (tag == tag_index_.end()) return {};
    std::vector<std::uint32_t> next;
    std::set_intersection(ids.begin(), ids.end(), tag->second.begin(), tag->second.end(), std::back_inserter(next));
    ids.swap(next);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

std::vector<SeriesResult> Engine::query(const Query& q) {
  std::lock_guard lock(mu_);
  std::vector<SeriesResult> out;
  for (auto id : match_locked(q)) {
    std::vector<Point> pts;
    if (auto s = sealed_.find(id); s != sealed_.end()) pts.insert(pts.end(), s->second.begin(), s->second.end());
    if (auto m = mem_.find(id); m != mem_.end()) pts.insert(pts.end(), m->second.points.begin(), m->second.points.end());
    std::stable_sort(pts.begin(), pts.end(), [](const Point& a, const Point& b) { return a.ts < b.ts; });
    std::vector<Point> merged;
    for (const auto& p : pts) {
      if (!merged.empty() && merged.back().ts == p.ts) merged.back() = p;
      else merged.push_back(p);
    }
    pts.swap(merged);
    SeriesResult row;
    row.key = series_keys_[id];
    double acc = 0;
    bool have = false;
    for (const auto& p : pts) {
      if (p.ts < q.start || p.ts > q.end) continue;
      row.points.push_back(p);
      ++row.count;
      if (!have) { acc = p.value; have = true; continue; }
      if (q.agg == Agg::Min) acc = std::min(acc, p.value);
      else if (q.agg == Agg::Max) acc = std::max(acc, p.value);
      else acc += p.value;
    }
    if (q.agg == Agg::Avg && row.count) row.aggregate = acc / static_cast<double>(row.count);
    else if (q.agg == Agg::Count) row.aggregate = static_cast<double>(row.count);
    else row.aggregate = acc;
    if (q.agg != Agg::Raw) row.points.clear();
    if (row.count) out.push_back(std::move(row));
  }
  return out;
}

void Engine::flush() {
  std::lock_guard lock(mu_);
  write_segment_locked();
}

Stats Engine::stats() const {
  std::lock_guard lock(mu_);
  Stats s;
  s.series = series_keys_.size();
  s.segments = segments_;
  s.wal_bytes = wal_bytes_;
  s.segment_bytes = segment_bytes_;
  for (const auto& [_, pts] : sealed_) s.points += pts.size();
  s.points += mem_points_;
  return s;
}

}  // namespace tsdb
