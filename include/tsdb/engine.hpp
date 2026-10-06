#pragma once
#include "tsdb/types.hpp"
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tsdb {

class Engine {
 public:
  explicit Engine(std::filesystem::path root, std::size_t flush_points = 4096);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  void write(const SeriesKey& key, Timestamp ts, double value);
  void write_batch(const SeriesKey& key, const std::vector<Point>& points);
  std::vector<SeriesResult> query(const Query& q);
  void flush();
  Stats stats() const;

 private:
  struct MemSeries { std::vector<Point> points; };
  void recover();
  void append_wal(const SeriesKey& key, Timestamp ts, double value);
  void write_segment_locked();
  void load_segment(const std::filesystem::path& path);
  std::vector<std::uint32_t> match_locked(const Query& q) const;

  std::filesystem::path root_;
  std::size_t flush_points_;
  mutable std::mutex mu_;
  std::vector<std::string> series_keys_;
  std::unordered_map<std::string, std::uint32_t> series_index_;
  std::unordered_map<std::string, std::vector<std::uint32_t>> tag_index_;
  std::unordered_map<std::uint32_t, MemSeries> mem_;
  std::unordered_map<std::uint32_t, std::vector<Point>> sealed_;
  std::size_t mem_points_ = 0;
  std::uint64_t segments_ = 0;
  std::uint64_t wal_bytes_ = 0;
  std::uint64_t segment_bytes_ = 0;
  bool dirty_wal_ = false;
};

}  // namespace tsdb
