#pragma once
#include "geometry.h"

// lon/lat --> webMercator
Point lonlatToWebMerc(double lon, double lat);

//webMercator -->lon/lat
Point webMercToLonLat(double x, double y);

