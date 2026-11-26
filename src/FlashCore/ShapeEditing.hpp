#pragma once

#include "Binary.hpp"

#include <array>

namespace flash
{
struct ShapePoint
{
    std::int32_t x{}, y{};
    auto operator<=>(const ShapePoint&) const = default;
};

using ShapeColor = std::array<std::uint8_t, 4>;

struct ShapeStop
{
    std::uint8_t ratio;
    ShapeColor color;
};

struct ShapePaint
{
    std::uint8_t type = 0;
    ShapeColor color{};
    Matrix matrix;
    std::vector<ShapeStop> stops;
    std::uint8_t gradient_flags{};
    std::int16_t focal{};
    std::uint16_t bitmap{};
};

struct ShapeStroke
{
    std::uint16_t width{}, flags{}, miter = 1024;
    ShapePaint paint;
};

struct ShapeEdge
{
    ShapePoint from, to;
    std::optional<ShapePoint> control;
    unsigned fill0{}, fill1{}, stroke{}, layer{};
};

struct ShapeDrawing
{
    unsigned version{}, character{};
    std::array<std::int32_t, 4> bounds{};
    std::uint8_t flags{};
    std::vector<ShapePaint> fills{1};
    std::vector<ShapeStroke> strokes{1};
    std::vector<ShapeEdge> edges;
};

inline constexpr unsigned shape_edge_limit = 60'000;
inline constexpr std::int32_t shape_coordinate_limit = (1 << 29) - 1;

bool is_editable_shape(unsigned code) noexcept;
ShapeDrawing read_shape_drawing(unsigned code, ByteView payload);
ShapeDrawing read_morph_drawing(unsigned code, ByteView payload, unsigned ratio);
Asset export_shape_drawing(const ShapeDrawing& drawing, const Movie* movie);
Movie replace_shape_drawing(const Movie& movie, std::size_t tag, const ShapeDrawing& drawing);
}
