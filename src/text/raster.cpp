// Turning an outline into a picture: see raster.h.

#include "raster.h"
#include "../fmath.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "../section_window.h"  // last: this file's own code goes in its section (D161)

namespace ww::text {

namespace {

// Exact area coverage: each edge adds, along its length, how much of every pixel lies to its
// right. A running sum along each row then gives the coverage of every pixel. (The method of
// font-rs. Nothing here depends on which way a contour winds.)
//
// An edge, followed down a row at a time from its first row: where it has got to (x), and how
// it adds to a row's cells. Both ways of filling below step edges with this alone, so they give
// the same numbers to the last bit.
struct Edge {
    float x = 0, dxdy = 0, y0 = 0, y1 = 0, dir = 1;
    int first = 0, last = 0;  // the rows it crosses, last not included

    // The edge from (x0, y0) to (x1, y1) in a box `h` rows high, rows from `to` on not wanted;
    // false where it crosses no row.
    bool make(float x0, float y0Given, float x1, float y1Given, int h, int to) {
        y0 = y0Given;
        y1 = y1Given;
        if (y0 == y1) return false;
        dir = 1.0f;
        if (y0 > y1) {
            std::swap(x0, x1);
            std::swap(y0, y1);
            dir = -1.0f;
        }
        dxdy = (x1 - x0) / (y1 - y0);
        x = x0;
        if (y0 < 0) x -= y0 * dxdy;
        first = std::max(0, static_cast<int>(std::floor(y0)));
        last = std::min({h, to, static_cast<int>(std::ceil(y1))});
        return first < last;
    }

    // Row `row`, the next of the edge's: what it adds to the row's cells, by add(cell, amount),
    // where `keep` (above the rows kept, only where the edge gets to); x moved on either way.
    template <class Add>
    void step(int row, bool keep, Add&& add) {
        const float dy = std::min(static_cast<float>(row + 1), y1) - std::max(static_cast<float>(row), y0);
        const float xNext = x + dxdy * dy;
        if (!keep) {
            x = xNext;
            return;
        }
        const float d = dy * dir;
        const float left = std::min(x, xNext), right = std::max(x, xNext);
        const float leftFloor = std::floor(left);
        const int leftCell = static_cast<int>(leftFloor);
        const float rightCeil = std::ceil(right);
        const int rightCell = static_cast<int>(rightCeil);

        if (rightCell <= leftCell + 1) {
            const float mid = 0.5f * (x + xNext) - leftFloor;
            add(leftCell, d - d * mid);
            add(leftCell + 1, d * mid);
        } else {
            const float s = 1.0f / (right - left);
            const float leftPart = left - leftFloor;
            const float a0 = 0.5f * s * (1.0f - leftPart) * (1.0f - leftPart);
            const float rightPart = right - rightCeil + 1.0f;
            const float am = 0.5f * s * rightPart * rightPart;
            add(leftCell, d * a0);
            if (rightCell == leftCell + 2) {
                add(leftCell + 1, d * (1.0f - a0 - am));
            } else {
                const float a1 = s * (1.5f - leftPart);
                add(leftCell + 1, d * (a1 - a0));
                for (int cell = leftCell + 2; cell < rightCell - 1; cell++) add(cell, d * s);
                const float a2 = a1 + static_cast<float>(rightCell - leftCell - 3) * s;
                add(rightCell - 1, d * (1.0f - a2 - am));
            }
            add(rightCell, d * am);
        }
        x = xNext;
    }
};

// A pixel's coverage from the running sum, how many times the outline goes round it, a part
// of a time at an edge: non-zero takes any of it, up to once; even-odd takes how far it is from
// an even number.
inline uint8_t coverageOf(float sum, FillRule rule) {
    if (rule == FillRule::EvenOdd) {
        const float turns = std::fmod(std::fabs(sum), 2.0f);
        const float coverage = std::min(turns > 1.0f ? 2.0f - turns : turns, 1.0f);
        return static_cast<uint8_t>(coverage * 255.0f + 0.5f);
    }
    const float coverage = std::min(std::fabs(sum), 1.0f);
    return static_cast<uint8_t>(coverage * 255.0f + 0.5f);
}

// The coverage of a whole box, every edge added in turn.
//
// Only the rows from `from` up to `to` are kept: a shape drawn inside a clip is worked out for
// the clip's rows alone. An edge is still followed from its first row, so every row kept comes
// out exactly as it would with all of them. Each row has a cell past its last pixel for what
// lands to the right of the picture, and its sum starts from nothing.
class Accumulator {
public:
    Accumulator(int width, int height, int from, int to)
        : w_(width), h_(height), from_(from), to_(to), a_(static_cast<size_t>(width + 1) * static_cast<size_t>(to - from), 0.0f) {}

    void line(float x0, float y0, float x1, float y1) {
        Edge e;
        if (e.make(x0, y0, x1, y1, h_, to_)) edge(e);
    }

    // An edge made for this box (by Edge::make with its height and last row kept).
    void edge(Edge e) {
        for (int row = e.first; row < e.last; row++) {
            const size_t start = row >= from_ ? static_cast<size_t>(row - from_) * static_cast<size_t>(w_ + 1) : 0;
            e.step(row, row >= from_, [&](int cell, float amount) { add(start, cell, amount); });
        }
    }

    // The kept rows' coverage, a byte a pixel, row after row.
    void finish(std::vector<uint8_t>& out, FillRule rule) const {
        const size_t w = static_cast<size_t>(w_);
        out.resize(w * static_cast<size_t>(to_ - from_));
        for (size_t row = 0; row < static_cast<size_t>(to_ - from_); row++) finishRow(row, out.data() + row * w, rule);
    }

    // One kept row's coverage (counted from the first kept), its pixels' bytes into .
    void finishRow(size_t row, uint8_t* to, FillRule rule) const {
        const size_t w = static_cast<size_t>(w_);
        const float* in = a_.data() + row * (w + 1);
        float sum = 0.0f;
        for (size_t i = 0; i < w; i++) {
            sum += in[i];
            to[i] = coverageOf(sum, rule);
        }
    }

private:
    int w_, h_, from_, to_;
    std::vector<float> a_;

    void add(size_t rowStart, int cell, float amount) {
        if (cell < 0) cell = 0;
        if (cell > w_) cell = w_;
        a_[rowStart + static_cast<size_t>(cell)] += amount;
    }
};

// Walks an outline as straight pieces, curves cut into as many as their bend needs, times
// `fineness` squared. Every contour is closed, whether or not the outline says so.
template <class Line>
void flatten(const Path& path, float fineness, Line&& line) {
    const float most = std::sqrt(fineness);
    float startX = 0, startY = 0, x = 0, y = 0;
    bool open = false;
    size_t p = 0;
    auto closeContour = [&] {
        if (open && (x != startX || y != startY)) line(x, y, startX, startY);
        open = false;
    };
    for (uint8_t verb : path.verbs) {
        const float* pt = path.points.data() + p;
        switch (verb) {
            case Path::Move:
                closeContour();
                startX = x = pt[0];
                startY = y = pt[1];
                open = true;
                p += 2;
                break;
            case Path::Line:
                line(x, y, pt[0], pt[1]);
                x = pt[0];
                y = pt[1];
                p += 2;
                break;
            case Path::Quad: {
                const float cx = pt[0], cy = pt[1], ex = pt[2], ey = pt[3];
                const float bend = static_cast<float>(fmath::hypot(x - 2 * cx + ex, y - 2 * cy + ey));
                const int pieces = std::clamp(static_cast<int>(std::ceil(std::sqrt(bend * 3.0f * fineness))), 1, static_cast<int>(32 * most));
                float px = x, py = y;
                for (int i = 1; i <= pieces; i++) {
                    const float t = static_cast<float>(i) / static_cast<float>(pieces), u = 1.0f - t;
                    const float nx = u * u * x + 2 * u * t * cx + t * t * ex;
                    const float ny = u * u * y + 2 * u * t * cy + t * t * ey;
                    line(px, py, nx, ny);
                    px = nx;
                    py = ny;
                }
                x = ex;
                y = ey;
                p += 4;
                break;
            }
            case Path::Cubic: {
                const float ax = pt[0], ay = pt[1], bx = pt[2], by = pt[3], ex = pt[4], ey = pt[5];
                const float bend = static_cast<float>(std::max(fmath::hypot(x - 2 * ax + bx, y - 2 * ay + by), fmath::hypot(ax - 2 * bx + ex, ay - 2 * by + ey)));
                const int pieces = std::clamp(static_cast<int>(std::ceil(std::sqrt(bend * 4.5f * fineness))), 1, static_cast<int>(48 * most));
                float px = x, py = y;
                for (int i = 1; i <= pieces; i++) {
                    const float t = static_cast<float>(i) / static_cast<float>(pieces), u = 1.0f - t;
                    const float nx = u * u * u * x + 3 * u * u * t * ax + 3 * u * t * t * bx + t * t * t * ex;
                    const float ny = u * u * u * y + 3 * u * u * t * ay + 3 * u * t * t * by + t * t * t * ey;
                    line(px, py, nx, ny);
                    px = nx;
                    py = ny;
                }
                x = ex;
                y = ey;
                p += 6;
                break;
            }
            case Path::Close:
                closeContour();
                x = startX;
                y = startY;
                break;
        }
    }
    closeContour();
}

}  // namespace

void rasterizeInto(const Path& path, int left, int top, int width, int height, std::vector<uint8_t>& coverage, float fineness, FillRule rule) {
    coverage.assign(static_cast<size_t>(std::max(width, 0)) * static_cast<size_t>(std::max(height, 0)), 0);
    if (path.empty() || width <= 0 || height <= 0) return;
    Accumulator acc(width, height, 0, height);
    const float originX = static_cast<float>(left), originY = static_cast<float>(top);
    flatten(path, fineness, [&](float x0, float y0, float x1, float y1) { acc.line(x0 - originX, originY - y0, x1 - originX, originY - y1); });
    acc.finish(coverage, rule);
}

GlyphImage rasterize(const Path& path, float fineness, FillRule rule) {
    GlyphImage image;
    float minX, minY, maxX, maxY;
    if (!path.bounds(minX, minY, maxX, maxY)) return image;
    image.left = static_cast<int>(std::floor(minX));
    image.top = static_cast<int>(std::ceil(maxY));
    image.width = static_cast<int>(std::ceil(maxX)) - image.left + 1;
    image.height = image.top - static_cast<int>(std::floor(minY)) + 1;
    if (image.width <= 0 || image.height <= 0 || image.width > 8192 || image.height > 8192) return GlyphImage{};
    rasterizeInto(path, image.left, image.top, image.width, image.height, image.pixels, fineness, rule);
    return image;
}

GlyphImage rasterize(const Path& path, float fineness, int fromRow, int toRow, FillRule rule) {
    GlyphImage image;
    float minX, minY, maxX, maxY;
    if (!path.bounds(minX, minY, maxX, maxY)) return image;
    // Far beyond any picture: nothing to draw, and nothing to overflow an int.
    const float far = 1 << 24;
    if (!(minX > -far && maxX < far && minY > -far && maxY < far)) return image;
    image.left = static_cast<int>(std::floor(minX));
    image.top = static_cast<int>(std::ceil(maxY));
    image.width = static_cast<int>(std::ceil(maxX)) - image.left + 1;
    const int height = image.top - static_cast<int>(std::floor(minY)) + 1;
    // The rows kept, counted from the image's top: row r lands on the picture's row r - top.
    const int from = std::max(0, fromRow + image.top), to = std::min(height, toRow + image.top);
    if (image.width <= 0 || image.width > 8192 || from >= to || to - from > 8192) return GlyphImage{};
    Accumulator acc(image.width, height, from, to);
    const float originX = static_cast<float>(image.left), originY = static_cast<float>(image.top);
    flatten(path, fineness, [&](float x0, float y0, float x1, float y1) { acc.line(x0 - originX, originY - y0, x1 - originX, originY - y1); });
    acc.finish(image.pixels, rule);
    image.top -= from;
    image.height = to - from;
    return image;
}

bool rasterizesRows(const Path& path, int fromRow, int toRow) {
    float minX, minY, maxX, maxY;
    if (!path.bounds(minX, minY, maxX, maxY)) return true;
    for (float p : path.points)
        if (!std::isfinite(p)) return true;
    const float far = 1 << 24;
    if (!(minX > -far && maxX < far && minY > -far && maxY < far)) return false;
    const int left = static_cast<int>(std::floor(minX));
    const int top = static_cast<int>(std::ceil(maxY));
    const int width = static_cast<int>(std::ceil(maxX)) - left + 1;
    const int height = top - static_cast<int>(std::floor(minY)) + 1;
    const int from = std::max(0, fromRow + top), to = std::min(height, toRow + top);
    return !(width > 8192 || (from < to && to - from > 8192));
}

Path clipped(const Path& path, float fineness, float minX, float minY, float maxX, float maxY) {
    Path out;
    bool open = false, any = false;
    float lastX = 0, lastY = 0, endX = 0, endY = 0;
    auto put = [&](double x, double y) {
        const float fx = static_cast<float>(std::clamp(x, static_cast<double>(minX), static_cast<double>(maxX)));
        const float fy = static_cast<float>(std::clamp(y, static_cast<double>(minY), static_cast<double>(maxY)));
        if (!open) {
            out.moveTo(fx, fy);
            open = true;
        } else if (fx != lastX || fy != lastY) {
            out.lineTo(fx, fy);
        }
        lastX = fx;
        lastY = fy;
    };
    flatten(path, fineness, [&](float x0, float y0, float x1, float y1) {
        // A piece not starting where the last ended: the next contour.
        if (!any || x0 != endX || y0 != endY) {
            if (open) out.close();
            open = false;
            put(x0, y0);
        }
        any = true;
        endX = x1;
        endY = y1;
        // Where along it the piece crosses a side, in order; between two of them it is all on one
        // side of each, and its ends pulled onto the box make a straight piece of what it covers.
        const double ax = x0, ay = y0, dx = static_cast<double>(x1) - x0, dy = static_cast<double>(y1) - y0;
        double at[6];
        int n = 0;
        for (double side : {static_cast<double>(minX), static_cast<double>(maxX)})
            if ((ax - side) * (ax + dx - side) < 0) at[n++] = (side - ax) / dx;
        for (double side : {static_cast<double>(minY), static_cast<double>(maxY)})
            if ((ay - side) * (ay + dy - side) < 0) at[n++] = (side - ay) / dy;
        std::sort(at, at + n);
        for (int k = 0; k < n; k++) put(ax + dx * at[k], ay + dy * at[k]);
        put(x1, y1);
    });
    if (open) out.close();
    return out;
}

void rasterizeRows(const Path& path, float fineness, int fromRow, int toRow, FillRule rule, const RowSpan& span) {
    // The box, the rows kept and the limits exactly as rasterize's above.
    float minX, minY, maxX, maxY;
    if (!path.bounds(minX, minY, maxX, maxY)) return;
    const float far = 1 << 24;
    if (!(minX > -far && maxX < far && minY > -far && maxY < far)) return;
    const int left = static_cast<int>(std::floor(minX));
    const int top = static_cast<int>(std::ceil(maxY));
    const int width = static_cast<int>(std::ceil(maxX)) - left + 1;
    const int height = top - static_cast<int>(std::floor(minY)) + 1;
    const int from = std::max(0, fromRow + top), to = std::min(height, toRow + top);
    if (width <= 0 || width > 8192 || from >= to || to - from > 8192) return;

    const float originX = static_cast<float>(left), originY = static_cast<float>(top);
    auto eachEdge = [&](auto&& use) {
        flatten(path, fineness, [&](float x0, float y0, float x1, float y1) {
            Edge e;
            if (e.make(x0 - originX, originY - y0, x1 - originX, originY - y1, height, to)) use(e);
        });
    };

    // Which way costs less: the whole box, as rasterize works (a cell's three passes), or the
    // edges a row at a time (a step of each edge in each row it crosses, the edges ordered by
    // row). A shape whose edges are many for its box, text and fine curves, goes the first way;
    // one whose box is mostly empty, a slanted line or an outline across a picture, the second.
    // Counted in a pass of their own, which keeps nothing; not where the box is small, or small
    // for the outline's points alone (each point an edge at least, once cut).
    const size_t area = static_cast<size_t>(width) * static_cast<size_t>(to - from);
    size_t count = 0, crossings = 0;
    const bool boxWay = area <= 4096 || area <= 16 * path.points.size();
    if (!boxWay) {
        eachEdge([&](const Edge& e) {
            count++;
            crossings += static_cast<size_t>(e.last - e.first);
        });
        if (count == 0) return;
    }
    if (boxWay || area <= 4 * crossings + 32 * count) {
        Accumulator acc(width, height, from, to);
        eachEdge([&](const Edge& e) { acc.edge(e); });
        std::vector<uint8_t> row(static_cast<size_t>(width));
        for (int r = from; r < to; r++) {
            acc.finishRow(static_cast<size_t>(r - from), row.data(), rule);
            span(r - top, left, width, row.data());
        }
        return;
    }

    // The edges, in the outline's order, then by the row each starts at (in the outline's order
    // among those starting at one row).
    std::vector<Edge> edges;
    edges.reserve(count);
    eachEdge([&](const Edge& e) { edges.push_back(e); });
    std::vector<uint32_t> starts(static_cast<size_t>(to) + 1, 0);
    for (const Edge& e : edges) starts[static_cast<size_t>(e.first) + 1]++;
    for (size_t r = 1; r < starts.size(); r++) starts[r] += starts[r - 1];
    std::vector<uint32_t> order(edges.size());
    {
        std::vector<uint32_t> at(starts.begin(), starts.end() - 1);
        for (size_t i = 0; i < edges.size(); i++) order[at[static_cast<size_t>(edges[i].first)]++] = static_cast<uint32_t>(i);
    }

    // A row at a time: the edges crossing it added to its cells in the outline's order, as the
    // Accumulator adds them (so each cell's sum is the same to the bit); then only the cells they
    // reached summed, handed on, and cleared. Left of them the sum is 0; right of them it stays
    // as it is, which is nothing for a closed outline but is handed on where it is not.
    std::vector<float> cells(static_cast<size_t>(width) + 1, 0.0f);
    std::vector<uint8_t> out(static_cast<size_t>(width));
    std::vector<uint32_t> active, merged, starting;
    size_t next = 0;
    for (int row = edges[order[0]].first; row < to; row++) {
        starting.clear();
        while (next < order.size() && edges[order[next]].first == row) starting.push_back(order[next++]);
        if (!starting.empty()) {
            merged.clear();
            std::merge(active.begin(), active.end(), starting.begin(), starting.end(), std::back_inserter(merged));
            active.swap(merged);
        }
        if (active.empty()) {
            if (next == order.size()) break;
            row = edges[order[next]].first - 1;  // on to the next edge's first row
            continue;
        }
        const bool keep = row >= from;
        int lo = width + 1, hi = -1;
        for (uint32_t k : active)
            edges[k].step(row, keep, [&](int cell, float amount) {
                if (cell < 0) cell = 0;
                if (cell > width) cell = width;
                cells[static_cast<size_t>(cell)] += amount;
                lo = std::min(lo, cell);
                hi = std::max(hi, cell);
            });
        active.erase(std::remove_if(active.begin(), active.end(), [&](uint32_t k) { return edges[k].last <= row + 1; }), active.end());
        if (!keep || hi < 0) continue;
        float sum = 0.0f;
        const int end = std::min(hi, width - 1);
        for (int i = lo; i <= end; i++) {
            sum += cells[static_cast<size_t>(i)];
            out[static_cast<size_t>(i)] = coverageOf(sum, rule);
        }
        int count = end - lo + 1;
        if (hi < width - 1) {
            const uint8_t rest = coverageOf(sum, rule);
            if (rest != 0) {
                std::fill(out.begin() + hi + 1, out.end(), rest);
                count = width - lo;
            }
        }
        if (count > 0) span(row - top, left + lo, count, out.data() + lo);
        std::fill(cells.begin() + lo, cells.begin() + hi + 1, 0.0f);
    }
}

}  // namespace ww::text
