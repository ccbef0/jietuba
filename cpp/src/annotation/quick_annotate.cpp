#include "annotation/quick_annotate.h"
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace jietuba {

double color_contrast(const QColor &a, const QColor &b) {
    // Simplified: mean absolute difference across R, G, B channels, normalized 0-1.
    double dr = std::abs(a.red() - b.red());
    double dg = std::abs(a.green() - b.green());
    double db = std::abs(a.blue() - b.blue());
    return (dr + dg + db) / (3.0 * 255.0);
}

QPoint find_max_contrast_position(const QImage &image, const QRect &region,
                                  const QSize &text_size,
                                  const std::vector<QPoint> &avoid_points) {
    if (image.isNull() || region.isEmpty()) return region.topLeft();

    const QColor red(255, 0, 0);
    const int step = 24;  // Sample every 24 pixels for speed.
    const int margin = 8;

    // Exclusion radius around avoid points (e.g. arrow endpoints).
    constexpr int avoid_radius = 60;

    double best_contrast = -1.0;
    QPoint best_pos = region.topLeft();

    // Ensure text fits within region.
    const int max_x = region.right() - text_size.width() - margin;
    const int max_y = region.bottom() - text_size.height() - margin;

    for (int y = region.top() + margin; y <= max_y; y += step) {
        for (int x = region.left() + margin; x <= max_x; x += step) {
            // Check avoid points.
            bool overlaps_avoid = false;
            for (const auto &ap : avoid_points) {
                if (std::abs(x - ap.x()) < avoid_radius && std::abs(y - ap.y()) < avoid_radius) {
                    overlaps_avoid = true;
                    break;
                }
            }
            if (overlaps_avoid) continue;

            // Sample pixels in the text-sized window.
            double total_contrast = 0.0;
            int samples = 0;
            const int sample_step = 6;  // Sample every 6 pixels within the window.
            for (int sy = y; sy < y + text_size.height() && sy < image.height(); sy += sample_step) {
                for (int sx = x; sx < x + text_size.width() && sx < image.width(); sx += sample_step) {
                    QColor pixel = image.pixelColor(sx, sy);
                    total_contrast += color_contrast(red, pixel);
                    samples++;
                }
            }
            if (samples == 0) continue;

            double avg_contrast = total_contrast / samples;
            if (avg_contrast > best_contrast) {
                best_contrast = avg_contrast;
                best_pos = QPoint(x, y);
            }
        }
    }

    return best_pos;
}

}  // namespace jietuba
