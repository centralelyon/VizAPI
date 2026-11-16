#pragma once
#include <string>
// GeoJSON in/out; uses upstream OctiMain.cpp::drawComp, no HTTP or child process.
std::string nativeOctiLayout(const std::string& graph, bool planarize = true);
