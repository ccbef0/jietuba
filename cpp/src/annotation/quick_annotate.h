#pragma once
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <vector>

namespace jietuba {

// Finds the position within the given image region where red text (#FF0000) would
// have the highest contrast against the surrounding pixels. Returns a point in
// image coordinates suitable for placing a text item's top-left corner.
//
// avoid_points: positions (e.g. arrow endpoints) that the text must not overlap.
// text_size: approximate bounding rect of the text to place.
//
// Algorithm:
//   1. Sample a grid of candidate positions across the region.
//   2. For each candidate, compute the mean absolute luminance difference between
//      red (#FF0000) and the pixels in a text-sized window around it.
//   3. Skip candidates that overlap any avoid_point's exclusion zone.
//   4. Return the candidate with maximum contrast that fits within bounds.
QPoint find_max_contrast_position(const QImage &image, const QRect &region,
                                  const QSize &text_size,
                                  const std::vector<QPoint> &avoid_points = {});

// Computes the perceptual contrast between two colors (simplified WCAG-like).
double color_contrast(const QColor &a, const QColor &b);

}  // namespace jietuba
