#include "Binary.hpp"

#include <array>
#include <cmath>
#include <format>
#include <map>

namespace flash
{
namespace
{
struct Point
{
    std::int64_t x{}, y{};
    auto operator<=>(const Point&) const = default;
};
struct Edge { Point from, to; std::optional<Point> bezier; };
struct Color { unsigned r, g, b, a; };
struct Texture
{
    unsigned bitmap;
    Matrix matrix;
    bool repeat;
};
struct Paint
{
    Color color{};
    std::string id, gradient;
    std::optional<Texture> texture;
};
struct Fill { Paint paint; std::vector<Edge> edges; };
struct Stroke
{
    Paint paint;
    unsigned width;
    unsigned cap = 0, join = 0;
    double miter = 4.0;
    bool no_close = false;
    std::vector<Edge> edges;
};

std::array<std::int32_t, 4> rect(Reader& input)
{
    BitReader bits(input);
    const auto size = bits.unsigned_bits(5);
    return {bits.signed_bits(size), bits.signed_bits(size), bits.signed_bits(size), bits.signed_bits(size)};
}

Color rgb(Reader& input, bool alpha)
{
    return {input.u8(), input.u8(), input.u8(), alpha ? input.u8() : 255u};
}

Paint read_paint(Reader& input, bool alpha, unsigned character, unsigned& serial)
{
    const auto type = input.u8();
    Paint paint;
    if (type == 0)
    {
        paint.color = rgb(input, alpha);
        return paint;
    }
    if (type >= 0x40 && type <= 0x43)
    {
        const auto id = input.u16();
        paint.id = std::format("clip-texture-{}-{}", character, ++serial);
        paint.texture = Texture{id, read_matrix(input), (type & 1) == 0};
        return paint;
    }
    if (type != 0x10 && type != 0x12 && type != 0x13)
        throw std::runtime_error("Ce type de remplissage (texture ou bitmap) n’est pas pris en charge en SVG.");
    const auto matrix = read_matrix(input);
    const auto flags = input.u8();
    constexpr std::string_view spread[]{"pad", "reflect", "repeat", "pad"};
    const auto count = flags & 15;
    if (!count)
        throw std::runtime_error("Dégradé SWF sans points de couleur.");
    const auto element = type == 0x10 ? "linearGradient" : "radialGradient";
    paint.id = std::format("gradient-{}-{}", character, ++serial);
    paint.gradient = std::format("<{} id=\"{}\" gradientUnits=\"userSpaceOnUse\" "
        "gradientTransform=\"matrix({:g} {:g} {:g} {:g} {} {})\" spreadMethod=\"{}\" color-interpolation=\"{}\"",
        element, paint.id, matrix.scale_x, matrix.skew_y, matrix.skew_x, matrix.scale_y, matrix.x, matrix.y,
        spread[flags >> 6], flags & 16 ? "linearRGB" : "sRGB");
    if (type == 0x10)
        paint.gradient += " x1=\"-16384\" y1=\"0\" x2=\"16384\" y2=\"0\"";
    else
        paint.gradient += " cx=\"0\" cy=\"0\" r=\"16384\"";
    std::string stops;
    for (int i = 0; i < count; ++i)
    {
        const auto ratio = input.u8() / 255.0;
        const auto color = rgb(input, alpha);
        stops += std::format("<stop offset=\"{:g}\" stop-color=\"#{:02X}{:02X}{:02X}\" stop-opacity=\"{:g}\"/>",
            ratio, color.r, color.g, color.b, color.a / 255.0);
    }
    if (type == 0x13)
    {
        const auto focal = std::bit_cast<std::int16_t>(input.u16()) / 256.0;
        paint.gradient += std::format(" fx=\"{:g}\" fy=\"0\"", std::clamp(focal, -0.999, 0.999) * 16384);
    }
    paint.gradient += std::format(">{}</{}>\n", stops, element);
    return paint;
}

std::string paint_attribute(std::string_view attribute, const Paint& paint)
{
    if (!paint.id.empty())
        return std::format("{}=\"url(#{})\"", attribute, paint.id);
    const auto& color = paint.color;
    return std::format("{}=\"#{:02X}{:02X}{:02X}\" {}-opacity=\"{:g}\"",
        attribute, color.r, color.g, color.b, attribute, color.a / 255.0);
}

void styles(Reader& input, unsigned version, unsigned character, unsigned& serial,
    std::vector<Fill>& fills, std::vector<Stroke>& lines)
{
    const auto count = [&]
    {
        const auto low = input.u8();
        return low == 255 && version > 1 ? static_cast<unsigned>(input.u16()) : low;
    };
    const auto fill_count = count();
    if (fills.size() + fill_count > 65536)
        throw std::runtime_error("Trop de styles de remplissage.");
    for (unsigned i = 0; i < fill_count; ++i)
        fills.push_back({read_paint(input, version >= 3, character, serial), {}});
    const auto line_count = count();
    if (lines.size() + line_count > 65536)
        throw std::runtime_error("Trop de styles de contour.");
    for (unsigned i = 0; i < line_count; ++i)
    {
        Stroke line{};
        line.width = input.u16();
        if (version < 4)
            line.paint.color = rgb(input, version == 3);
        else
        {
            BitReader flags(input);
            line.cap = flags.unsigned_bits(2);
            line.join = flags.unsigned_bits(2);
            const auto filled = flags.unsigned_bits(1);
            flags.unsigned_bits(8);
            line.no_close = flags.unsigned_bits(1) != 0;
            flags.unsigned_bits(2);
            if (line.join == 2)
                line.miter = input.u16() / 256.0;
            if (filled)
                line.paint = read_paint(input, true, character, serial);
            else
                line.paint.color = rgb(input, true);
        }
        lines.push_back(std::move(line));
    }
}

std::string coordinates(Point point) { return std::format("{} {}", point.x, point.y); }

std::string texture_id(unsigned shape, unsigned bitmap)
{
    return std::format("texture-{}-{}", shape, bitmap);
}

std::string texture_tiles(const Paint& paint, unsigned shape, const EmbeddedBitmap& bitmap,
    const std::array<std::int32_t, 4>& bounds)
{
    const auto& texture = *paint.texture;
    const auto& matrix = texture.matrix;
    auto output = std::format("<g clip-path=\"url(#{})\"><g transform=\"matrix({:g} {:g} {:g} {:g} {} {})\">\n",
        paint.id, matrix.scale_x, matrix.skew_y, matrix.skew_x, matrix.scale_y, matrix.x, matrix.y);
    int left = 0, top = 0, right = 1, bottom = 1;
    if (texture.repeat)
    {
        const auto determinant = matrix.scale_x * matrix.scale_y - matrix.skew_x * matrix.skew_y;
        if (std::abs(determinant) < 1e-12)
            throw std::runtime_error("La matrice de répétition du bitmap est singulière.");
        auto min_x = std::numeric_limits<double>::infinity(), max_x = -min_x;
        auto min_y = min_x, max_y = -min_y;
        for (const auto px : {bounds[0], bounds[1]})
            for (const auto py : {bounds[2], bounds[3]})
            {
                const auto x = static_cast<double>(px) - matrix.x, y = static_cast<double>(py) - matrix.y;
                const auto tile_x = (matrix.scale_y * x - matrix.skew_x * y) / determinant / bitmap.width;
                const auto tile_y = (-matrix.skew_y * x + matrix.scale_x * y) / determinant / bitmap.height;
                min_x = std::min(min_x, tile_x);
                min_y = std::min(min_y, tile_y);
                max_x = std::max(max_x, tile_x);
                max_y = std::max(max_y, tile_y);
            }
        if (std::max({std::abs(min_x), std::abs(max_x), std::abs(min_y), std::abs(max_y)}) > 1e6)
            throw std::runtime_error("La grille de répétition du bitmap est hors limites.");
        left = static_cast<int>(std::floor(min_x));
        top = static_cast<int>(std::floor(min_y));
        right = static_cast<int>(std::ceil(max_x));
        bottom = static_cast<int>(std::ceil(max_y));
        if (std::int64_t{right - left} * (bottom - top) > 256)
            throw std::runtime_error("Trop de répétitions de la texture pour l’aperçu SVG.");
    }
    const auto image = texture_id(shape, texture.bitmap);
    for (auto y = top; y < bottom; ++y)
        for (auto x = left; x < right; ++x)
            output += std::format("<use xlink:href=\"#{}\" x=\"{}\" y=\"{}\"/>\n", image,
                std::int64_t{x} * bitmap.width, std::int64_t{y} * bitmap.height);
    return output + "</g></g>\n";
}

void svg_edge(std::string& path, const Edge& edge)
{
    if (edge.bezier)
        path += "Q" + coordinates(*edge.bezier) + ' ' + coordinates(edge.to);
    else
        path += "L" + coordinates(edge.to);
}

std::string fill_path(const std::vector<Edge>& edges)
{
    std::multimap<Point, std::size_t> starts;
    std::vector<bool> consumed(edges.size());
    for (std::size_t i = 0; i < edges.size(); ++i)
        starts.emplace(edges[i].from, i);
    std::string path;
    for (std::size_t i = 0; i < edges.size(); ++i)
    {
        if (consumed[i])
            continue;
        const auto start = edges[i].from;
        auto end = start;
        path += "M" + coordinates(start);
        do
        {
            const auto next = starts.find(end);
            if (next == starts.end())
                break;
            const auto index = next->second;
            starts.erase(next);
            svg_edge(path, edges[index]);
            consumed[index] = true;
            end = edges[index].to;
        } while (start != end);
        path += 'Z';
    }
    return path;
}

std::string stroke_path(const Stroke& line)
{
    std::string path;
    std::optional<Point> start, end;
    for (const auto& edge : line.edges)
    {
        if (end != edge.from)
        {
            if (start && start == end && !line.no_close)
                path += 'Z';
            start = edge.from;
            path += "M" + coordinates(edge.from);
        }
        svg_edge(path, edge);
        end = edge.to;
    }
    if (start == end && start && !line.no_close)
        path += 'Z';
    return path;
}
}

Asset export_shape(unsigned code, ByteView payload, const Movie* movie)
{
    const auto version = code == 2 ? 1u : code == 22 ? 2u : code == 32 ? 3u : 4u;
    Reader input(payload);
    const auto character = input.u16();
    const auto bounds = rect(input);
    bool winding = false;
    if (version == 4)
    {
        rect(input);
        winding = (input.u8() & 4) != 0;
    }

    std::vector<Fill> fills(1);
    std::vector<Stroke> lines(1);
    unsigned serial = 0;
    styles(input, version, character, serial, fills, lines);
    BitReader bits(input);
    auto fill_bits = bits.unsigned_bits(4), line_bits = bits.unsigned_bits(4);
    std::size_t fill0 = 0, fill1 = 0, line = 0, base_fill = 0, base_line = 0;
    Point pen;
    unsigned edges = 0;
    const auto index = [&](unsigned size, std::size_t base, std::size_t count)
    {
        const auto local = bits.unsigned_bits(size);
        const auto global = local ? base + local : 0;
        if (global >= count)
            throw std::runtime_error("Indice de style de forme invalide.");
        return global;
    };
    for (;;)
    {
        if (bits.unsigned_bits(1))
        {
            if (++edges > 60'000)
                throw std::runtime_error("Trop d’arêtes pour un export SVG.");
            Edge edge{pen, {}, {}};
            const auto straight = bits.unsigned_bits(1);
            const auto size = bits.unsigned_bits(4) + 2;
            if (!straight)
            {
                pen.x += bits.signed_bits(size);
                pen.y += bits.signed_bits(size);
                edge.bezier = pen;
                pen.x += bits.signed_bits(size);
                pen.y += bits.signed_bits(size);
            }
            else if (bits.unsigned_bits(1))
            {
                pen.x += bits.signed_bits(size);
                pen.y += bits.signed_bits(size);
            }
            else if (bits.unsigned_bits(1))
                pen.y += bits.signed_bits(size);
            else
                pen.x += bits.signed_bits(size);
            edge.to = pen;
            if (line)
                lines[line].edges.push_back(edge);
            if (fill0 != fill1)
            {
                if (fill0)
                    fills[fill0].edges.push_back(edge);
                if (fill1)
                    fills[fill1].edges.push_back({edge.to, edge.from, edge.bezier});
            }
            continue;
        }
        const auto flags = bits.unsigned_bits(5);
        if (!flags)
            break;
        if (flags & 1)
        {
            const auto size = bits.unsigned_bits(5);
            pen = {bits.signed_bits(size), bits.signed_bits(size)};
        }
        if (flags & 2)
            fill0 = index(fill_bits, base_fill, fills.size());
        if (flags & 4)
            fill1 = index(fill_bits, base_fill, fills.size());
        if (flags & 8)
            line = index(line_bits, base_line, lines.size());
        if (flags & 16)
        {
            bits.align();
            base_fill = fills.size() - 1;
            base_line = lines.size() - 1;
            styles(input, version, character, serial, fills, lines);
            fill_bits = bits.unsigned_bits(4);
            line_bits = bits.unsigned_bits(4);
        }
    }
    const auto width = (std::max)(std::int64_t{bounds[1]} - bounds[0], std::int64_t{20});
    const auto height = (std::max)(std::int64_t{bounds[3]} - bounds[2], std::int64_t{20});
    auto svg = std::format("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "width=\"{:g}\" height=\"{:g}\" "
        "viewBox=\"{} {} {} {}\">\n", static_cast<double>(width) / 20.0, static_cast<double>(height) / 20.0,
        bounds[0], bounds[2], width, height);
    const auto append_path = [&](std::string path)
    {
        if (path.size() > max_text_size - svg.size())
            throw std::runtime_error("Le SVG produit dépasse 2 Mio.");
        svg += path;
    };
    std::map<unsigned, EmbeddedBitmap> textures;
    if (serial)
    {
        append_path("<defs>\n");
        for (const auto& fill : fills)
            if (!fill.edges.empty())
            {
                if (const auto& texture = fill.paint.texture)
                {
                    const auto resource = movie ? movie->character(texture->bitmap) : std::nullopt;
                    if (!resource)
                        throw std::runtime_error("Bitmap de remplissage introuvable.");
                    auto [image, inserted] = textures.try_emplace(texture->bitmap);
                    if (inserted)
                    {
                        image->second = embedded_bitmap(*movie, *resource);
                        append_path(std::format("<image id=\"{}\" width=\"{}\" height=\"{}\" xlink:href=\"{}\"/>\n",
                            texture_id(character, texture->bitmap), image->second.width, image->second.height,
                            image->second.data_uri));
                    }
                    append_path(std::format("<clipPath id=\"{}\"><path clip-rule=\"{}\" d=\"{}\"/></clipPath>\n",
                        fill.paint.id, winding ? "nonzero" : "evenodd", fill_path(fill.edges)));
                }
                append_path(fill.paint.gradient);
            }
        for (const auto& outline : lines)
            if (!outline.edges.empty())
            {
                if (outline.paint.texture)
                    throw std::runtime_error("L’aperçu SVG ne prend pas en charge les contours à texture.");
                append_path(outline.paint.gradient);
            }
        append_path("</defs>\n");
    }
    for (const auto& fill : fills)
        if (!fill.edges.empty())
        {
            if (const auto& texture = fill.paint.texture)
                append_path(texture_tiles(fill.paint, character, textures.at(texture->bitmap), bounds));
            else
                append_path(std::format("<path {} fill-rule=\"{}\" d=\"{}\"/>\n",
                    paint_attribute("fill", fill.paint), winding ? "nonzero" : "evenodd",
                    fill_path(fill.edges)));
        }
    constexpr std::string_view caps[]{"round", "butt", "square", "round"};
    constexpr std::string_view joins[]{"round", "bevel", "miter", "round"};
    for (const auto& stroke : lines)
        if (!stroke.edges.empty())
            append_path(std::format("<path fill=\"none\" {} "
                "stroke-width=\"{}\" stroke-linecap=\"{}\" stroke-linejoin=\"{}\" stroke-miterlimit=\"{:g}\" d=\"{}\"/>\n",
                paint_attribute("stroke", stroke.paint), (std::max)(1u, stroke.width),
                caps[stroke.cap], joins[stroke.join], (std::max)(1.0, stroke.miter), stroke_path(stroke)));
    svg += "</svg>\n";
    return {L".svg", Bytes(svg.begin(), svg.end())};
}
}
