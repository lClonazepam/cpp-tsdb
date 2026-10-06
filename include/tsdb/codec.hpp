#pragma once
#include "tsdb/types.hpp"
#include <string>

namespace tsdb {

std::string encode_points(const std::vector<Point>& points);
std::vector<Point> decode_points(const std::string& blob);

}  // namespace tsdb
