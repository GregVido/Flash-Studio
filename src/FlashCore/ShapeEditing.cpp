#include "ShapeEditing.hpp"

#include <cmath>
#include <map>
#include <numeric>

namespace flash
{
namespace
{
unsigned shape_version(unsigned code)
{
    switch (code)
    {
    case 2: return 1;
    case 22: return 2;
    case 32: return 3;
    case 83: return 4;
    default: throw std::runtime_error("Ce tag ne contient pas de dessin vectoriel éditable.");
    }
}

std::array<std::int32_t, 4> read_bounds(Reader& input)
{
    BitReader bits(input);
    const auto size = bits.unsigned_bits(5);
    return {bits.signed_bits(size), bits.signed_bits(size), bits.signed_bits(size), bits.signed_bits(size)};
}

ShapeColor read_color(Reader& input, bool alpha)
{
    return {input.u8(), input.u8(), input.u8(), alpha ? input.u8() : std::uint8_t{255}};
}

bool is_gradient(unsigned type) { return type == 0x10 || type == 0x12 || type == 0x13; }
bool is_texture(unsigned type) { return type >= 0x40 && type <= 0x43; }

ShapePaint read_paint(Reader& input, bool alpha)
{
    ShapePaint paint;
    paint.type = input.u8();
    if (!paint.type)
        paint.color = read_color(input, alpha);
    else if (is_texture(paint.type))
    {
        paint.bitmap = input.u16();
        paint.matrix = read_matrix(input);
    }
    else if (is_gradient(paint.type))
    {
        paint.matrix = read_matrix(input);
        const auto packed = input.u8();
        paint.gradient_flags = packed & 0xF0;
        const auto count = packed & 15;
        if (!count)
            throw std::runtime_error("Dégradé SWF sans points de couleur.");
        for (int i = 0; i < count; ++i)
            paint.stops.push_back({input.u8(), read_color(input, alpha)});
        if (paint.type == 0x13)
            paint.focal = std::bit_cast<std::int16_t>(input.u16());
    }
    else
        throw std::runtime_error("Type de remplissage SWF non pris en charge.");
    return paint;
}

void read_styles(Reader& input, ShapeDrawing& drawing)
{
    const auto count = [&]
    {
        const auto low = input.u8();
        return low == 255 && drawing.version > 1 ? static_cast<unsigned>(input.u16()) : low;
    };
    const auto fill_count = count();
    if (drawing.fills.size() + fill_count > 65536)
        throw std::runtime_error("Trop de styles de remplissage.");
    for (unsigned i = 0; i < fill_count; ++i)
        drawing.fills.push_back(read_paint(input, drawing.version >= 3));
    const auto stroke_count = count();
    if (drawing.strokes.size() + stroke_count > 65536)
        throw std::runtime_error("Trop de styles de contour.");
    for (unsigned i = 0; i < stroke_count; ++i)
    {
        ShapeStroke stroke;
        stroke.width = input.u16();
        if (drawing.version == 4)
        {
            BitReader flags(input);
            stroke.flags = static_cast<std::uint16_t>(flags.unsigned_bits(16));
            if (((stroke.flags >> 12) & 3) == 2)
                stroke.miter = input.u16();
        }
        if (stroke.flags & 0x0800)
            stroke.paint = read_paint(input, true);
        else
            stroke.paint.color = read_color(input, drawing.version >= 3);
        drawing.strokes.push_back(std::move(stroke));
    }
}

std::int32_t coordinate(std::int64_t value)
{
    if (value < -shape_coordinate_limit || value > shape_coordinate_limit)
        throw std::runtime_error("Coordonnées de forme hors limites.");
    return static_cast<std::int32_t>(value);
}

class BitWriter
{
public:
    explicit BitWriter(Bytes& out) : out_(out) {}
    void bits(std::uint32_t value, unsigned count)
    {
        for (unsigned i = count; i != 0; --i)
        {
            if (!offset_)
                out_.push_back(0);
            out_.back() |= static_cast<std::uint8_t>(((value >> (i - 1)) & 1) << (7 - offset_));
            offset_ = (offset_ + 1) % 8;
        }
    }
    void signed_bits(std::int32_t value, unsigned count) { bits(static_cast<std::uint32_t>(value), count); }
    void align() { offset_ = 0; }
private:
    Bytes& out_;
    unsigned offset_{};
};

unsigned signed_width(std::int32_t value)
{
    return static_cast<unsigned>(std::bit_width(value < 0 ? ~static_cast<std::uint32_t>(value) :
        static_cast<std::uint32_t>(value))) + 1;
}

void write_bounds(Bytes& out, const std::array<std::int32_t, 4>& bounds)
{
    BitWriter bits(out);
    unsigned size = 1;
    for (const auto value : bounds)
        size = std::max(size, signed_width(value));
    if (size > 31)
        throw std::runtime_error("Dimensions de forme hors limites.");
    bits.bits(size, 5);
    for (const auto value : bounds)
        bits.signed_bits(value, size);
}

void write_matrix(Bytes& out, const Matrix& matrix)
{
    BitWriter bits(out);
    const auto fixed = [](double value)
    {
        if (!std::isfinite(value) || value < -16384 || value > 1073741823.0 / 65536)
            throw std::runtime_error("Matrice de remplissage hors limites.");
        return static_cast<std::int32_t>(std::lround(value * 65536));
    };
    const auto pair = [&](std::int32_t x, std::int32_t y)
    {
        const auto width = std::max(signed_width(x), signed_width(y));
        if (width > 31)
            throw std::runtime_error("Matrice de remplissage hors limites.");
        bits.bits(width, 5);
        bits.signed_bits(x, width);
        bits.signed_bits(y, width);
    };
    const bool scale = matrix.scale_x != 1 || matrix.scale_y != 1;
    const bool skew = matrix.skew_x != 0 || matrix.skew_y != 0;
    bits.bits(scale, 1);
    if (scale)
        pair(fixed(matrix.scale_x), fixed(matrix.scale_y));
    bits.bits(skew, 1);
    if (skew)
        pair(fixed(matrix.skew_y), fixed(matrix.skew_x));
    pair(matrix.x, matrix.y);
}

void write_color(Bytes& out, const ShapeColor& color, bool alpha)
{
    if (!alpha && color[3] != 255)
        throw std::runtime_error("Cette version de forme utilise uniquement les couleurs opaques.");
    append(out, ByteView(color).first(alpha ? 4 : 3));
}

void write_paint(Bytes& out, const ShapePaint& paint, unsigned version, const Movie& movie)
{
    const bool alpha = version >= 3;
    out.push_back(paint.type);
    if (!paint.type)
        write_color(out, paint.color, alpha);
    else if (is_texture(paint.type))
    {
        if (!movie.character(paint.bitmap))
            throw std::runtime_error("Bitmap de remplissage introuvable.");
        put16(out, paint.bitmap);
        write_matrix(out, paint.matrix);
    }
    else if (is_gradient(paint.type))
    {
        if (paint.stops.empty() || paint.stops.size() > 15)
            throw std::runtime_error("Configuration du dégradé non prise en charge par cette forme.");
        write_matrix(out, paint.matrix);
        out.push_back(static_cast<std::uint8_t>((paint.gradient_flags & 0xF0) | paint.stops.size()));
        for (const auto& stop : paint.stops)
        {
            out.push_back(stop.ratio);
            write_color(out, stop.color, alpha);
        }
        if (paint.type == 0x13)
            put16(out, std::bit_cast<std::uint16_t>(paint.focal));
    }
    else
        throw std::runtime_error("Type de remplissage SWF non pris en charge.");
}

void write_stroke(Bytes& out, const ShapeStroke& stroke, unsigned version, const Movie& movie)
{
    put16(out, stroke.width);
    if (version == 4)
    {
        out.push_back(static_cast<std::uint8_t>(stroke.flags >> 8));
        out.push_back(static_cast<std::uint8_t>(stroke.flags));
        if (((stroke.flags >> 12) & 3) == 2)
            put16(out, stroke.miter);
    }
    if (version == 4 && (stroke.flags & 0x0800))
        write_paint(out, stroke.paint, version, movie);
    else
    {
        if (stroke.paint.type)
            throw std::runtime_error("Cette version de forme utilise uniquement des contours unis.");
        write_color(out, stroke.paint.color, version >= 3);
    }
}

struct Layer
{
    std::map<unsigned, unsigned> fills, strokes;
    unsigned fill_bits{}, stroke_bits{};
};

void write_count(Bytes& out, std::size_t size, unsigned version)
{
    if (size >= 255 && version > 1)
    {
        out.push_back(255);
        put16(out, static_cast<std::uint16_t>(size));
    }
    else if (size <= 255)
        out.push_back(static_cast<std::uint8_t>(size));
    else
        throw std::runtime_error("Trop de styles pour cette version de forme.");
}

void write_styles(Bytes& out, Layer& layer, const ShapeDrawing& drawing, const Movie& movie)
{
    const auto palette = [&](auto& indices, const auto& styles, auto write)
    {
        if (indices.size() > 32767)
            throw std::runtime_error("Trop de styles dans le dessin.");
        write_count(out, indices.size(), drawing.version);
        unsigned index = 0;
        for (auto& [style, local] : indices)
        {
            local = ++index;
            write(out, styles[style], drawing.version, movie);
        }
        return static_cast<unsigned>(std::bit_width(index));
    };
    layer.fill_bits = palette(layer.fills, drawing.fills, write_paint);
    layer.stroke_bits = palette(layer.strokes, drawing.strokes, write_stroke);
}

struct Extent
{
    std::array<std::int32_t, 4> bounds{
        shape_coordinate_limit, -shape_coordinate_limit, shape_coordinate_limit, -shape_coordinate_limit};
    void include(ShapePoint point, std::int32_t padding)
    {
        bounds[0] = std::min(bounds[0], coordinate(std::int64_t{point.x} - padding));
        bounds[1] = std::max(bounds[1], coordinate(std::int64_t{point.x} + padding));
        bounds[2] = std::min(bounds[2], coordinate(std::int64_t{point.y} - padding));
        bounds[3] = std::max(bounds[3], coordinate(std::int64_t{point.y} + padding));
    }
};

void write_edge(BitWriter& bits, ShapePoint from, ShapePoint to, std::optional<ShapePoint> control, unsigned& count)
{
    const auto middle = control.value_or(to);
    const std::array delta{middle.x - from.x, middle.y - from.y, to.x - middle.x, to.y - middle.y};
    unsigned width = 2;
    for (auto value : delta)
        width = std::max(width, signed_width(value));
    if (width > 17)
    {
        const auto midpoint = [](ShapePoint a, ShapePoint b)
        {
            return ShapePoint{std::midpoint(a.x, b.x), std::midpoint(a.y, b.y)};
        };
        if (control)
        {
            const auto left = midpoint(from, *control), right = midpoint(*control, to);
            const auto split = midpoint(left, right);
            write_edge(bits, from, split, left, count);
            write_edge(bits, split, to, right, count);
        }
        else
        {
            const auto split = midpoint(from, to);
            write_edge(bits, from, split, {}, count);
            write_edge(bits, split, to, {}, count);
        }
        return;
    }
    if (++count > shape_edge_limit)
        throw std::runtime_error("Trop d’arêtes dans la forme modifiée.");
    bits.bits(1, 1);
    bits.bits(!control, 1);
    bits.bits(width - 2, 4);
    if (!control)
        bits.bits(1, 1);
    bits.signed_bits(delta[0], width);
    bits.signed_bits(delta[1], width);
    if (control)
    {
        bits.signed_bits(delta[2], width);
        bits.signed_bits(delta[3], width);
    }
}

Bytes encode_drawing(const ShapeDrawing& drawing, const Movie& movie)
{
    if (drawing.edges.size() > shape_edge_limit)
        throw std::runtime_error("Trop d’arêtes dans la forme modifiée.");
    Extent geometry, painted;
    std::map<unsigned, Layer> layers;
    unsigned previous_layer = 0;
    auto flags = drawing.flags;
    for (const auto& edge : drawing.edges)
    {
        if (edge.layer < previous_layer || (edge.layer && drawing.version == 1) ||
            edge.fill0 >= drawing.fills.size() || edge.fill1 >= drawing.fills.size() || edge.stroke >= drawing.strokes.size())
            throw std::runtime_error("Style ou calque de forme invalide.");
        previous_layer = edge.layer;
        auto& layer = layers[edge.layer];
        if (edge.fill0) layer.fills.try_emplace(edge.fill0);
        if (edge.fill1) layer.fills.try_emplace(edge.fill1);
        if (edge.stroke) layer.strokes.try_emplace(edge.stroke);
        const auto& stroke = drawing.strokes[edge.stroke];
        const auto miter = (((stroke.flags >> 12) & 3) == 2) ? std::max(1.0, stroke.miter / 256.0) : 1.0;
        const auto padding = edge.stroke ? static_cast<std::int32_t>(std::ceil(stroke.width * miter / 2)) : 0;
        for (const auto point : {edge.from, edge.to, edge.control.value_or(edge.from)})
        {
            geometry.include(point, 0);
            painted.include(point, padding);
        }
        if (edge.stroke)
            flags |= (stroke.flags & 0x0600) == 0x0600 ? 2 : 1;
    }
    if (layers.empty())
    {
        layers.try_emplace(0);
        geometry.bounds = painted.bounds = {};
    }
    Bytes out;
    put16(out, static_cast<std::uint16_t>(drawing.character));
    write_bounds(out, painted.bounds);
    if (drawing.version == 4)
    {
        write_bounds(out, geometry.bounds);
        out.push_back(flags);
    }
    auto layer = layers.begin();
    write_styles(out, layer->second, drawing, movie);
    BitWriter bits(out);
    bits.bits(layer->second.fill_bits, 4);
    bits.bits(layer->second.stroke_bits, 4);
    std::optional<ShapeEdge> previous;
    unsigned written = 0;
    for (const auto& edge : drawing.edges)
    {
        if (edge.layer != layer->first)
        {
            bits.bits(0, 1);
            bits.bits(16, 5);
            bits.align();
            ++layer;
            write_styles(out, layer->second, drawing, movie);
            bits.bits(layer->second.fill_bits, 4);
            bits.bits(layer->second.stroke_bits, 4);
            previous.reset();
        }
        const bool move = !previous || edge.from != previous->to;
        const auto change = (move ? 1u : 0u) | (!previous || edge.fill0 != previous->fill0 ? 2u : 0u) |
            (!previous || edge.fill1 != previous->fill1 ? 4u : 0u) | (!previous || edge.stroke != previous->stroke ? 8u : 0u);
        if (change)
        {
            bits.bits(0, 1);
            bits.bits(change, 5);
            if (move)
            {
                const auto width = std::max(signed_width(edge.from.x), signed_width(edge.from.y));
                bits.bits(width, 5);
                bits.signed_bits(edge.from.x, width);
                bits.signed_bits(edge.from.y, width);
            }
            if (change & 2)
                bits.bits(edge.fill0 ? layer->second.fills.at(edge.fill0) : 0, layer->second.fill_bits);
            if (change & 4)
                bits.bits(edge.fill1 ? layer->second.fills.at(edge.fill1) : 0, layer->second.fill_bits);
            if (change & 8)
                bits.bits(edge.stroke ? layer->second.strokes.at(edge.stroke) : 0, layer->second.stroke_bits);
        }
        write_edge(bits, edge.from, edge.to, edge.control, written);
        previous = edge;
    }
    bits.bits(0, 6);
    return out;
}
}

bool is_editable_shape(unsigned code) noexcept
{
    return code == 2 || code == 22 || code == 32 || code == 83;
}

namespace
{
void read_edges(Reader& input, ShapeDrawing& drawing, bool morph = false)
{
    BitReader bits(input);
    auto fill_bits = bits.unsigned_bits(4), line_bits = bits.unsigned_bits(4);
    unsigned fill0 = 0, fill1 = 0, line = 0, base_fill = 0, base_line = 0, layer = 0;
    ShapePoint pen;
    const auto index = [&](unsigned size, unsigned base, std::size_t count)
    {
        const auto local = bits.unsigned_bits(size);
        const auto global = local ? base + local : 0;
        if (global >= count)
            throw std::runtime_error("Indice de style de forme invalide.");
        return global;
    };
    const auto relative = [&](unsigned size, bool x, bool y)
    {
        if (x) pen.x = coordinate(std::int64_t{pen.x} + bits.signed_bits(size));
        if (y) pen.y = coordinate(std::int64_t{pen.y} + bits.signed_bits(size));
    };
    for (;;)
    {
        if (bits.unsigned_bits(1))
        {
            if (drawing.edges.size() >= shape_edge_limit)
                throw std::runtime_error("Trop d’arêtes dans la forme SWF.");
            ShapeEdge edge{pen, {}, {}, fill0, fill1, line, layer};
            const auto straight = bits.unsigned_bits(1);
            const auto size = bits.unsigned_bits(4) + 2;
            if (!straight)
            {
                relative(size, true, true);
                edge.control = pen;
                relative(size, true, true);
            }
            else if (bits.unsigned_bits(1))
                relative(size, true, true);
            else
            {
                const auto vertical = bits.unsigned_bits(1);
                relative(size, !vertical, vertical != 0);
            }
            edge.to = pen;
            drawing.edges.push_back(edge);
            continue;
        }
        const auto flags = bits.unsigned_bits(5);
        if (!flags)
            break;
        if (flags & 1)
        {
            const auto size = bits.unsigned_bits(5);
            pen = {coordinate(bits.signed_bits(size)), coordinate(bits.signed_bits(size))};
        }
        if (flags & 2) fill0 = index(fill_bits, base_fill, drawing.fills.size());
        if (flags & 4) fill1 = index(fill_bits, base_fill, drawing.fills.size());
        if (flags & 8) line = index(line_bits, base_line, drawing.strokes.size());
        if (flags & 16)
        {
            if (morph)
                throw std::runtime_error("Nouveaux styles dans les arêtes d’une forme interpolée.");
            ++layer;
            bits.align();
            base_fill = static_cast<unsigned>(drawing.fills.size() - 1);
            base_line = static_cast<unsigned>(drawing.strokes.size() - 1);
            read_styles(input, drawing);
            fill_bits = bits.unsigned_bits(4);
            line_bits = bits.unsigned_bits(4);
        }
    }
}

struct MorphInterpolation
{
    double ratio;

    double number(double start, double end) const { return std::lerp(start, end, ratio); }
    template<class T> T integer(T start, T end) const { return static_cast<T>(std::lround(number(start, end))); }
    ShapePoint point(ShapePoint a, ShapePoint b) const { return {integer(a.x, b.x), integer(a.y, b.y)}; }
    ShapeColor color(Reader& input) const
    {
        auto start = read_color(input, true);
        const auto end = read_color(input, true);
        for (std::size_t i = 0; i < start.size(); ++i)
            start[i] = integer(start[i], end[i]);
        return start;
    }
    Matrix matrix(Reader& input) const
    {
        const auto start = read_matrix(input), end = read_matrix(input);
        return {number(start.scale_x, end.scale_x), number(start.scale_y, end.scale_y),
            number(start.skew_x, end.skew_x), number(start.skew_y, end.skew_y),
            integer(start.x, end.x), integer(start.y, end.y)};
    }
    ShapePaint paint(Reader& input) const
    {
        ShapePaint paint;
        paint.type = input.u8();
        if (!paint.type)
            paint.color = color(input);
        else if (is_texture(paint.type))
        {
            paint.bitmap = input.u16();
            paint.matrix = matrix(input);
        }
        else if (is_gradient(paint.type))
        {
            paint.matrix = matrix(input);
            const auto count = input.u8();
            if (count == 0 || count > 15)
                throw std::runtime_error("Nombre de points du dégradé interpolé invalide.");
            for (unsigned i = 0; i < count; ++i)
            {
                const auto start_ratio = input.u8();
                const auto start_color = read_color(input, true);
                const auto end_ratio = input.u8();
                const auto end_color = read_color(input, true);
                ShapeColor mixed{};
                for (std::size_t channel = 0; channel < mixed.size(); ++channel)
                    mixed[channel] = integer(start_color[channel], end_color[channel]);
                paint.stops.push_back({integer(start_ratio, end_ratio), mixed});
            }
            if (paint.type == 0x13)
            {
                const auto start = std::bit_cast<std::int16_t>(input.u16());
                paint.focal = integer(start, std::bit_cast<std::int16_t>(input.u16()));
            }
        }
        else
            throw std::runtime_error("Type de remplissage interpolé non pris en charge.");
        return paint;
    }
};
}

ShapeDrawing read_shape_drawing(unsigned code, ByteView payload)
{
    ShapeDrawing drawing;
    drawing.version = shape_version(code);
    Reader input(payload);
    drawing.character = input.u16();
    drawing.bounds = read_bounds(input);
    if (drawing.version == 4)
    {
        read_bounds(input);
        drawing.flags = input.u8();
    }
    read_styles(input, drawing);
    read_edges(input, drawing);
    return drawing;
}

ShapeDrawing read_morph_drawing(unsigned code, ByteView payload, unsigned ratio)
{
    const MorphInterpolation mix{std::min(ratio, 65535u) / 65535.0};
    Reader input(payload);
    ShapeDrawing drawing;
    drawing.version = code == 84 ? 4 : 3;
    drawing.character = input.u16();
    drawing.bounds = read_bounds(input);
    const auto end_bounds = read_bounds(input);
    for (std::size_t i = 0; i < drawing.bounds.size(); ++i)
        drawing.bounds[i] = mix.integer(drawing.bounds[i], end_bounds[i]);
    if (drawing.version == 4)
    {
        read_bounds(input);
        read_bounds(input);
        drawing.flags = input.u8() & 3;
    }
    const auto offset = input.u32();
    Reader start(input.take(offset));
    const auto count = [&]()
    {
        const auto value = start.u8();
        return value == 255 ? static_cast<unsigned>(start.u16()) : value;
    };
    const auto fills = count();
    for (unsigned i = 0; i < fills; ++i)
        drawing.fills.push_back(mix.paint(start));
    const auto strokes = count();
    for (unsigned i = 0; i < strokes; ++i)
    {
        const auto start_width = start.u16();
        ShapeStroke stroke;
        stroke.width = mix.integer(start_width, start.u16());
        if (drawing.version == 4)
        {
            BitReader bits(start);
            stroke.flags = static_cast<std::uint16_t>(bits.unsigned_bits(16));
            if (((stroke.flags >> 12) & 3) == 2)
                stroke.miter = start.u16();
        }
        if (stroke.flags & 0x0800)
            stroke.paint = mix.paint(start);
        else
            stroke.paint.color = mix.color(start);
        drawing.strokes.push_back(std::move(stroke));
    }
    read_edges(start, drawing, true);
    if (!ratio)
        return drawing;
    ShapeDrawing end;
    read_edges(input, end, true);
    if (end.edges.size() != drawing.edges.size())
        throw std::runtime_error("Nombre d’arêtes différent entre les états de la forme interpolée.");
    const auto control = [](const ShapeEdge& edge)
    {
        return edge.control.value_or(ShapePoint{std::midpoint(edge.from.x, edge.to.x), std::midpoint(edge.from.y, edge.to.y)});
    };
    for (std::size_t index = 0; index < drawing.edges.size(); ++index)
    {
        auto& a = drawing.edges[index];
        const auto& b = end.edges[index];
        if (a.control || b.control)
            a.control = mix.point(control(a), control(b));
        a.from = mix.point(a.from, b.from);
        a.to = mix.point(a.to, b.to);
    }
    return drawing;
}

Movie replace_shape_drawing(const Movie& movie, std::size_t tag, const ShapeDrawing& drawing)
{
    const auto& original = movie.tags().at(tag);
    if (drawing.version != shape_version(original.code) || original.character_id != drawing.character ||
        drawing.fills.empty() || drawing.strokes.empty())
        throw std::runtime_error("Le dessin modifié ne correspond pas à la forme SWF.");
    const auto payload = encode_drawing(drawing, movie);
    const auto checked = read_shape_drawing(original.code, payload);
    if (checked.character != drawing.character || checked.edges.size() < drawing.edges.size())
        throw std::runtime_error("La forme réencodée est invalide.");
    return movie.replace_tag(tag, payload);
}
}
