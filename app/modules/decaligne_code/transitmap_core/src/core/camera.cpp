#include <algorithm>


#include "camera.h"

void Camera::resize(int w, int h) {

    screenW = std::max(1, w);
    screenH = std::max(1, h);

}

// webMercator to screen
double Camera::sx(double x) const {return (x - centerX) * scale + screenW * 0.5;}
double Camera::sy(double y) const {return (y - centerY) * scale + screenH * 0.5;}

//void Camera::vertex(double x, double y) const {glVertex2f((float)sx(x), (float)sy(y));}

void Camera::pan(double dx, double dy) {

    centerX -= dx / scale;
    centerY += dy / scale;
}

//void Camera::zoom(double factor) {
//
//    scale *= factor;
//    scale = std::clamp(scale, 0.01, 1000.0);
//}

// for railway scale
void Camera::zoom(double factor) {
    scale *= factor;
    scale = std::clamp(scale, 1e-6, 1000.0);
}

void Camera::zoomAt(double factor, double screenX, double screenY) {




    const Point anchor = screenToWorld(screenX, screenY);
    zoom(factor);
    centerX = anchor.x - (screenX - screenW * 0.5) / scale;
    centerY = anchor.y - (screenY - screenH * 0.5) / scale;
}
void Camera::fitToData(const GeoData& data) {

    double minX = 1e30, minY = 1e30;
    double maxX = -1e30, maxY = -1e30;

    auto expand = [&](const Point& p) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    };

    for (auto& p : data.stations) {
        expand(p.pos);
    }

    for (const auto& route : data.routes) {
        for (const auto& segment : route.segments) {
            for (const auto& p : segment) {
                expand(p);
            }
        }
    }
    for (auto& o : data.obstacles) {
        for (auto& p : o.outer) {
            expand(p);
        }
        for (auto& h : o.holes) {
            for (auto& p : h) {
                expand(p);
            }
        }
    }

    centerX = (minX + maxX) * 0.5;
    centerY = (minY + maxY) * 0.5;

    double dx = maxX - minX;
    double dy = maxY - minY;

    if (minX > maxX) { centerX = centerY = 0; scale = 1; return; }
    scale = 0.9 * std::min(screenW / std::max(dx, 1.0), screenH / std::max(dy, 1.0));
}

double Camera::getScale() const { return scale; }

int Camera::getScreenW() const { return screenW; }
int Camera::getScreenH() const { return screenH; }

// webMercator to normalized device coordinates
float Camera::ndcX(double wx) const {

    const double px = sx(wx);
    return static_cast<float>(px / screenW * 2.0 - 1.0);
}
float Camera::ndcY(double wy) const {

    const double py = sy(wy);                 
    return static_cast<float>(py / screenH * 2.0 - 1.0);
}

Point Camera::screenToWorld(double px, double py) const {

    return {
        (px - screenW * 0.5) / scale + centerX,
        (py - screenH * 0.5) / scale + centerY
    };
}
