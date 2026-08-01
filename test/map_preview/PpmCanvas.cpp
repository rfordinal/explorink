#include "PpmCanvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

PpmCanvas::PpmCanvas(int width, int height) : width_(width), height_(height), pixels_(width * height, 0) {}

void PpmCanvas::setPixel(int x, int y) {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
  pixels_[static_cast<size_t>(y) * width_ + x] = 1;
}

void PpmCanvas::drawLine(int x1, int y1, int x2, int y2, int lineWidth) {
  const int halfWidth = std::max(0, lineWidth / 2);
  const int dx = std::abs(x2 - x1);
  const int dy = -std::abs(y2 - y1);
  const int sx = x1 < x2 ? 1 : -1;
  const int sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;
  int x = x1;
  int y = y1;

  while (true) {
    for (int oy = -halfWidth; oy <= halfWidth; ++oy) {
      for (int ox = -halfWidth; ox <= halfWidth; ++ox) {
        setPixel(x + ox, y + oy);
      }
    }
    if (x == x2 && y == y2) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y += sy;
    }
  }
}

void PpmCanvas::fillRoundedRect(int x, int y, int width, int height, int cornerRadius) {
  const int r = std::clamp(cornerRadius, 0, std::min(width, height) / 2);
  for (int py = y; py < y + height; ++py) {
    for (int px = x; px < x + width; ++px) {
      // Distance into the nearest corner region, clamped to 0 outside it --
      // only the actual corner squares need the circle test.
      const int cornerX = (px < x + r) ? x + r : (px >= x + width - r ? x + width - r - 1 : px);
      const int cornerY = (py < y + r) ? y + r : (py >= y + height - r ? y + height - r - 1 : py);
      const bool inCornerBox = (px < x + r || px >= x + width - r) && (py < y + r || py >= y + height - r);
      if (inCornerBox) {
        const int ddx = px - cornerX;
        const int ddy = py - cornerY;
        if (ddx * ddx + ddy * ddy > r * r) continue;
      }
      setPixel(px, py);
    }
  }
}

void PpmCanvas::fillPolygon(const int* xPoints, const int* yPoints, int numPoints) {
  if (numPoints < 3) return;
  int minY = yPoints[0];
  int maxY = yPoints[0];
  for (int i = 1; i < numPoints; ++i) {
    minY = std::min(minY, yPoints[i]);
    maxY = std::max(maxY, yPoints[i]);
  }

  // Standard scanline polygon fill: for each row, collect x where edges
  // cross it, sort, fill between pairs.
  std::vector<int> crossings;
  for (int y = minY; y <= maxY; ++y) {
    crossings.clear();
    for (int i = 0; i < numPoints; ++i) {
      const int j = (i + 1) % numPoints;
      const int y1 = yPoints[i];
      const int y2 = yPoints[j];
      if (y1 == y2) continue;
      if ((y >= y1 && y < y2) || (y >= y2 && y < y1)) {
        const double t = static_cast<double>(y - y1) / (y2 - y1);
        crossings.push_back(static_cast<int>(std::round(xPoints[i] + t * (xPoints[j] - xPoints[i]))));
      }
    }
    std::sort(crossings.begin(), crossings.end());
    for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
      for (int x = crossings[i]; x <= crossings[i + 1]; ++x) {
        setPixel(x, y);
      }
    }
  }
}

bool PpmCanvas::writePpm(const std::string& path) const {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", width_, height_);
  std::vector<uint8_t> row(static_cast<size_t>(width_) * 3);
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      const uint8_t v = pixels_[static_cast<size_t>(y) * width_ + x] ? 0 : 255;
      row[static_cast<size_t>(x) * 3 + 0] = v;
      row[static_cast<size_t>(x) * 3 + 1] = v;
      row[static_cast<size_t>(x) * 3 + 2] = v;
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}
