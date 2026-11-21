// Outlines of arcs, wedges and wide lines: see shapes.h.

#include "shapes.h"
#include "../fmath.h"

#include <algorithm>
#include <cmath>

namespace ww::text {

namespace {

constexpr double kPi = 3.14159265358979323846;
// How far a straight piece of a curve may stray from it, in pixels.
constexpr double kTolerance = 0.05;

using Points = std::vector<double>;  // x, y pairs

// Twice the signed area of a polygon, positive when it goes clockwise on the screen.
double signedArea(const Points& pts) {
    double sum = 0;
    const size_t n = pts.size() / 2;
    for (size_t i = 0; i < n; i++) {
        const size_t j = (i + 1) % n;
        sum += pts[2 * i] * pts[2 * j + 1] - pts[2 * j] * pts[2 * i + 1];
    }
    return sum;
}

// A piece of the shape, turned clockwise on the screen (or anticlockwise, to cut a hole).
void addPiece(Path& p, Points pts, bool hole = false) {
    if (pts.size() < 6) return;
    if ((signedArea(pts) < 0) != hole) {
        const size_t n = pts.size() / 2;
        for (size_t i = 0; i < n / 2; i++) {
            std::swap(pts[2 * i], pts[2 * (n - 1 - i)]);
            std::swap(pts[2 * i + 1], pts[2 * (n - 1 - i) + 1]);
        }
    }
    p.moveTo(static_cast<float>(pts[0]), static_cast<float>(pts[1]));
    for (size_t i = 2; i + 1 < pts.size(); i += 2) p.lineTo(static_cast<float>(pts[i]), static_cast<float>(pts[i + 1]));
    p.close();
}

// The points of an arc of the ellipse about (cx, cy), from `start` through `extent` degrees,
// close enough together that no straight piece strays far from the curve.
void arcPoints(Points& out, double cx, double cy, double rx, double ry, double start, double extent) {
    const double radius = std::max(std::max(rx, ry), 0.0);
    double step = radius > kTolerance ? 2 * fmath::acos(std::max(-1.0, 1 - kTolerance / radius)) : kPi / 2;
    step = std::clamp(step, kPi / 720, kPi / 8);
    const double sweep = extent * kPi / 180;
    const int pieces = std::max(1, static_cast<int>(std::ceil(std::fabs(sweep) / step)));
    const double a0 = start * kPi / 180;
    for (int k = 0; k <= pieces; k++) {
        const double a = a0 + sweep * k / pieces;
        out.push_back(cx + rx * fmath::cos(a));
        out.push_back(cy - ry * fmath::sin(a));
    }
}

// A band `width` across along the line from a to b, its ends carried on by `extend`.
Points band(double ax, double ay, double bx, double by, double width, double extend) {
    const double dx = bx - ax, dy = by - ay, length = fmath::hypot(dx, dy);
    if (length == 0) {
        const double h = width / 2;
        return {ax - h, ay - h, ax + h, ay - h, ax + h, ay + h, ax - h, ay + h};
    }
    const double ux = dx / length, uy = dy / length;
    const double nx = -uy * width / 2, ny = ux * width / 2;
    const double sx = ax - ux * extend, sy = ay - uy * extend, ex = bx + ux * extend, ey = by + uy * extend;
    return {sx + nx, sy + ny, ex + nx, ey + ny, ex - nx, ey - ny, sx - nx, sy - ny};
}

Points disc(double cx, double cy, double radius) {
    Points pts;
    arcPoints(pts, cx, cy, radius, radius, 0, 360);
    pts.resize(pts.size() - 2);  // the last point is the first again
    return pts;
}

}  // namespace

Spread spreadOf(double widthPx, double unitPx) {
    const double extra = std::max(0.0, widthPx - unitPx);
    const double before = std::floor(extra / 2);
    return Spread{before, extra - before};
}

void arcFill(Path& p, double l, double t, double r, double b, double start, double extent, ArcKind kind) {
    if (r <= l || b <= t || extent == 0) return;
    const double cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2;
    extent = std::clamp(extent, -360.0, 360.0);
    Points pts;
    if (kind == kArcPie && std::fabs(extent) < 360) {
        pts.push_back(cx);
        pts.push_back(cy);
    }
    arcPoints(pts, cx, cy, rx, ry, start, extent);
    addPiece(p, pts);
}

void arcStroke(Path& p, double l, double t, double r, double b, double start, double extent, ArcKind kind, double width) {
    if (r < l || b < t || extent == 0 || width <= 0) return;
    const double cx = (l + r) / 2, cy = (t + b) / 2, rx = (r - l) / 2, ry = (b - t) / 2;
    const double h = width / 2;
    extent = std::clamp(extent, -360.0, 360.0);
    const double irx = rx - h, iry = ry - h;
    if (std::fabs(extent) >= 360) {
        // A whole ring: the outer edge, and the inner one cut out of it.
        Points outer, inner;
        arcPoints(outer, cx, cy, rx + h, ry + h, 0, 360);
        outer.resize(outer.size() - 2);
        addPiece(p, outer);
        if (irx > 0 && iry > 0) {
            arcPoints(inner, cx, cy, irx, iry, 0, 360);
            inner.resize(inner.size() - 2);
            addPiece(p, inner, true);
        }
        return;
    }
    // The curve: along the outer edge, then back along the inner one.
    Points curve;
    arcPoints(curve, cx, cy, rx + h, ry + h, start, extent);
    if (irx > 0 && iry > 0) {
        Points inner;
        arcPoints(inner, cx, cy, irx, iry, start, extent);
        for (size_t i = inner.size(); i >= 2; i -= 2) {
            curve.push_back(inner[i - 2]);
            curve.push_back(inner[i - 1]);
        }
    } else {
        curve.push_back(cx);
        curve.push_back(cy);
    }
    addPiece(p, curve);
    const double a0 = start * kPi / 180, a1 = (start + extent) * kPi / 180;
    const double sx = cx + rx * fmath::cos(a0), sy = cy - ry * fmath::sin(a0);
    const double ex = cx + rx * fmath::cos(a1), ey = cy - ry * fmath::sin(a1);
    if (kind == kArcChord) {
        addPiece(p, band(sx, sy, ex, ey, width, h));
    } else if (kind == kArcPie) {
        addPiece(p, band(cx, cy, sx, sy, width, h));
        addPiece(p, band(cx, cy, ex, ey, width, h));
    }
}

void polylineStroke(Path& p, const std::vector<double>& xy, bool closed, double width) {
    const size_t n = xy.size() / 2;
    if (n < 2 || width <= 0) return;
    const double h = width / 2;
    const size_t segments = closed ? n : n - 1;
    for (size_t i = 0; i < segments; i++) {
        const size_t j = (i + 1) % n;
        addPiece(p, band(xy[2 * i], xy[2 * i + 1], xy[2 * j], xy[2 * j + 1], width, h));
    }
    // Round joins, where two lines meet: their outer corners filled in.
    for (size_t i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) addPiece(p, disc(xy[2 * i], xy[2 * i + 1], h));
}

}  // namespace ww::text
