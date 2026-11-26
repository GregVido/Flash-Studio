#include "Binary.hpp"
#include "WebExport.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <format>

namespace flash
{
Asset export_shape(unsigned code, ByteView payload, const Movie* movie);
std::vector<wchar_t> font_glyph_labels(const Movie& movie, unsigned id);

namespace
{
struct GlyphTable
{
    ByteView shapes;
    std::vector<unsigned> offsets;
    unsigned units = 1024;
};

GlyphTable font_glyphs(const Movie& movie, unsigned font)
{
    const auto id = movie.character(font);
    if (!id)
        throw std::runtime_error("Police du texte statique introuvable.");
    const auto code = movie.tags()[*id].code;
    Reader input(movie.data(*id));
    input.skip(2);
    std::vector<unsigned> offsets;
    unsigned base = 2;
    if (code == 10)
    {
        if (!input.remaining())
            return {};
        const auto first = input.u16();
        if (first < 2 || (first & 1))
            throw std::runtime_error("Table de glyphes DefineFont invalide.");
        const unsigned count = first / 2;
        offsets.reserve(count + 1);
        offsets.push_back(first);
        for (unsigned i = 1; i < count; ++i)
            offsets.push_back(input.u16());
        offsets.push_back(static_cast<unsigned>(movie.data(*id).size()) - base);
    }
    else if (code == 48 || code == 75)
    {
        const auto flags = input.u8();
        input.skip(1);
        input.skip(input.u8());
        const auto count = input.u16();
        if (!count)
            return {};
        base = static_cast<unsigned>(input.position());
        offsets.reserve(count + 1);
        for (unsigned i = 0; i <= count; ++i)
            offsets.push_back(flags & 8 ? input.u32() : input.u16());
    }
    else
        throw std::runtime_error("Les glyphes de cette police n’ont pas de tracé SWF exploitable.");

    const auto shapes = movie.data(*id).subspan(base);
    if (offsets.back() > shapes.size() || !std::ranges::is_sorted(offsets) ||
        offsets.front() < input.position() - base)
        throw std::runtime_error("Positions de glyphes SWF invalides.");
    return {shapes, std::move(offsets), code == 75 ? 20480u : 1024u};
}

std::string glyph_path(const GlyphTable& table, unsigned index)
{
    if (table.offsets.empty() || index >= table.offsets.size() - 1)
        throw std::runtime_error("Indice de glyphe SWF hors limites.");
    const auto bytes = table.shapes.subspan(table.offsets[index], table.offsets[index + 1] - table.offsets[index]);
    if (bytes.empty())
        return {};
    const std::array<std::uint8_t, 10> prefix{0, 0, 0, 1, 0, 255, 255, 255, 255, 0};
    Bytes shape(prefix.begin(), prefix.end());
    append(shape, bytes);
    const auto asset = export_shape(32, shape, nullptr);
    const std::string_view svg(reinterpret_cast<const char*>(asset.bytes.data()), asset.bytes.size());
    std::string geometry;
    for (auto start = svg.find(" d=\""); start != std::string_view::npos; start = svg.find(" d=\"", start))
    {
        start += 4;
        const auto end = svg.find('"', start);
        if (end == std::string_view::npos)
            throw std::runtime_error("Tracé de glyphe SVG incomplet.");
        geometry += svg.substr(start, end - start);
        start = end + 1;
    }
    return geometry;
}
}

Asset text_svg(const Movie& movie, std::size_t index)
{
    const auto code = movie.tags().at(index).code;
    if (code != 11 && code != 33)
        throw std::runtime_error("Seuls les textes statiques peuvent être tracés avec leurs glyphes.");
    Reader input(movie.data(index));
    input.skip(2);
    BitReader bits(input);
    const auto width_bits = bits.unsigned_bits(5);
    const auto left = bits.signed_bits(width_bits), right = bits.signed_bits(width_bits);
    const auto top = bits.signed_bits(width_bits), bottom = bits.signed_bits(width_bits);
    const auto bounds_width = std::max(20.0, static_cast<double>(right) - left);
    const auto bounds_height = std::max(20.0, static_cast<double>(bottom) - top);
    const auto matrix = read_matrix(input);
    const auto glyph_bits = input.u8(), advance_bits = input.u8();
    if (glyph_bits > 32 || advance_bits > 32)
        throw std::runtime_error("Taille des entrées de texte SWF invalide.");
    auto svg = std::format("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{:g}\" height=\"{:g}\" "
        "viewBox=\"{} {} {:g} {:g}\">\n<g transform=\"matrix({:g} {:g} {:g} {:g} {} {})\">\n",
        bounds_width / 20, bounds_height / 20, left, top, bounds_width, bounds_height,
        matrix.scale_x, matrix.skew_y, matrix.skew_x, matrix.scale_y, matrix.x, matrix.y);
    std::unordered_map<unsigned, GlyphTable> fonts;
    std::unordered_map<std::uint64_t, std::string> paths;
    std::array<unsigned, 4> color{0, 0, 0, 255};
    unsigned font = 0, height = 0;
    std::int64_t x = 0, y = 0;
    std::size_t glyph_total = 0;
    while (const auto flags = input.u8())
    {
        if (!(flags & 128))
            throw std::runtime_error("Enregistrement de texte SWF invalide.");
        if (flags & 8)
            font = input.u16();
        if (flags & 4)
        {
            color = {input.u8(), input.u8(), input.u8(), code == 33 ? input.u8() : 255u};
        }
        if (flags & 1)
            x = std::bit_cast<std::int16_t>(input.u16());
        if (flags & 2)
            y = std::bit_cast<std::int16_t>(input.u16());
        if (flags & 8)
            height = input.u16();
        const auto count = input.u8();
        if (!count)
            continue;
        if ((glyph_total += count) > 30'000)
            throw std::runtime_error("Ce champ contient trop de glyphes pour un aperçu SVG.");
        auto [table, new_font] = fonts.try_emplace(font);
        if (new_font)
            table->second = font_glyphs(movie, font);
        BitReader entries(input);
        const auto scale = static_cast<double>(height) / table->second.units;
        svg += std::format("<g fill=\"#{:02X}{:02X}{:02X}\" fill-opacity=\"{:g}\">\n",
            color[0], color[1], color[2], color[3] / 255.0);
        for (unsigned i = 0; i < count; ++i)
        {
            const auto glyph = entries.unsigned_bits(glyph_bits);
            const auto advance = entries.signed_bits(advance_bits);
            const auto key = (std::uint64_t{font} << 32) | glyph;
            auto [path, inserted] = paths.try_emplace(key);
            if (inserted)
                path->second = glyph_path(table->second, glyph);
            if (!path->second.empty())
                svg += std::format("<path transform=\"translate({} {}) scale({:g})\" d=\"{}\" fill-rule=\"evenodd\"/>\n",
                    x, y, scale, path->second);
            x += advance;
            if (svg.size() > max_text_size)
                throw std::runtime_error("Le tracé des glyphes dépasse 2 Mio.");
        }
        svg += "</g>\n";
    }
    svg += "</g>\n</svg>\n";
    return {L".svg", Bytes(svg.begin(), svg.end())};
}

Asset font_svg(const Movie& movie, std::size_t index)
{
    const auto& tag = movie.tags().at(index);
    if (!tag.character_id || (tag.code != 10 && tag.code != 48 && tag.code != 75))
        throw std::runtime_error("Cette police ne contient pas de tracés de glyphes SWF.");
    const auto table = font_glyphs(movie, *tag.character_id);
    if (table.offsets.size() < 2)
        throw std::runtime_error("Cette police ne contient aucun glyphe incorporé.");
    constexpr unsigned columns = 8, cell_width = 72, cell_height = 76;
    const auto count = static_cast<unsigned>(std::min<std::size_t>(table.offsets.size() - 1, 512));
    const auto labels = font_glyph_labels(movie, *tag.character_id);
    auto svg = std::format("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"{}\" height=\"{}\">\n<title>{} / {} glyphes</title>\n",
        columns * cell_width, ((count + columns - 1) / columns) * cell_height, count, table.offsets.size() - 1);
    for (unsigned i = 0; i < count; ++i)
    {
        const auto x = i % columns * cell_width, y = i / columns * cell_height;
        svg += std::format("<rect x=\"{}\" y=\"{}\" width=\"66\" height=\"70\" rx=\"4\" fill=\"#262e39\"/>\n", x + 3, y + 3);
        const auto glyph = glyph_path(table, i);
        if (!glyph.empty())
            svg += std::format("<path fill=\"#f2f4f8\" fill-rule=\"evenodd\" transform=\"translate({} {}) scale({:g})\" d=\"{}\"/>\n",
                x + 15, y + 47, 35.0 / table.units, glyph);
        const auto label = i < labels.size() && labels[i] != L'\uFFFD' ?
            std::format("U+{:04X}", static_cast<unsigned>(labels[i])) : std::format("#{}", i);
        svg += std::format("<text x=\"{}\" y=\"{}\" fill=\"#aebdd0\" font-size=\"10\">{}</text>\n", x + 11, y + 65, label);
        if (svg.size() > max_text_size)
            break;
    }
    svg += "</svg>";
    return {L".svg", Bytes(svg.begin(), svg.end())};
}

namespace
{
void big16(Bytes& data, int value)
{
    data.push_back(static_cast<std::uint8_t>(static_cast<unsigned>(value) >> 8));
    data.push_back(static_cast<std::uint8_t>(value));
}

void big32(Bytes& data, std::uint32_t value)
{
    big16(data, static_cast<int>(value >> 16));
    big16(data, static_cast<int>(value & 0xffff));
}

void metric_at(Bytes& data, std::size_t offset, int value)
{
    data[offset] = static_cast<std::uint8_t>(static_cast<unsigned>(value) >> 8);
    data[offset + 1] = static_cast<std::uint8_t>(value);
}

std::uint32_t checksum(ByteView data)
{
    std::uint32_t result = 0, word = 0;
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        word = (word << 8) | data[i];
        if (i % 4 == 3) { result += word; word = 0; }
    }
    return result + (word << ((4 - data.size() % 4) % 4 * 8));
}

struct FontBox
{
    int left{}, bottom{}, right{}, top{};
    void extend(int x, int y)
    {
        left = std::min(left, x); right = std::max(right, x);
        bottom = std::min(bottom, y); top = std::max(top, y);
    }
    void write(Bytes& bytes) const
    {
        for (const auto value : {left, bottom, right, top}) big16(bytes, value);
    }
};

struct FontPoint { int x, y; bool on_curve; };

FontBox true_type_glyph(Bytes& glyf, std::string_view path, unsigned divisor, int& max_points, int& max_contours)
{
    std::vector<FontPoint> points;
    std::vector<int> endpoints;
    const auto number = [&]
    {
        while (path.starts_with(' ') || path.starts_with(',')) path.remove_prefix(1);
        int value{};
        const auto end = path.data() + path.size();
        const auto parsed = std::from_chars(path.data(), end, value);
        if (parsed.ec != std::errc{}) throw std::runtime_error("Coordonnée de glyphe non valide.");
        path.remove_prefix(static_cast<std::size_t>(parsed.ptr - path.data()));
        return std::clamp(static_cast<int>(std::lround(static_cast<double>(value) / divisor)), -16383, 16383);
    };
    const auto point = [&](bool curve)
    {
        const int x = number(), y = -number();
        points.push_back({x, y, curve});
    };
    std::size_t start = 0;
    while (!path.empty())
    {
        const auto command = path.front();
        path.remove_prefix(1);
        switch (command)
        {
        case 'M': start = points.size(); point(true); break;
        case 'L': point(true); break;
        case 'Q': point(false); point(true); break;
        case 'Z':
            if (points.size() > start + 1 && points.back().x == points[start].x && points.back().y == points[start].y)
                points.pop_back();
            if (points.size() > start) endpoints.push_back(static_cast<int>(points.size() - 1));
            break;
        default: throw std::runtime_error("Contour de glyphe non pris en charge.");
        }
        if (points.size() > 32767 || endpoints.size() > 32767) throw std::runtime_error("Glyphe trop complexe.");
    }
    if (endpoints.empty()) return {};
    FontBox box{points.front().x, points.front().y, points.front().x, points.front().y};
    for (const auto& value : points) box.extend(value.x, value.y);
    max_points = std::max(max_points, static_cast<int>(points.size()));
    max_contours = std::max(max_contours, static_cast<int>(endpoints.size()));
    big16(glyf, static_cast<int>(endpoints.size()));
    box.write(glyf);
    for (const int value : endpoints) big16(glyf, value);
    big16(glyf, 0);
    for (const auto& value : points) glyf.push_back(value.on_curve ? 1 : 0);
    for (int previous = 0; const auto& value : points) { big16(glyf, value.x - previous); previous = value.x; }
    for (int previous = 0; const auto& value : points) { big16(glyf, value.y - previous); previous = value.y; }
    if (glyf.size() % 2) glyf.push_back(0);
    return box;
}

Bytes font_names()
{
    constexpr std::array<std::pair<int, std::string_view>, 5> names{{{1, "Flash Studio Embedded"}, {2, "Regular"},
        {3, "FlashStudio:Embedded"}, {4, "Flash Studio Embedded"}, {6, "FlashStudioEmbedded"}}};
    Bytes table, strings;
    big16(table, 0); big16(table, static_cast<int>(names.size())); big16(table, 6 + 12 * static_cast<int>(names.size()));
    for (const auto& [key, name] : names)
    {
        for (const auto value : {3, 1, 0x0409, key, static_cast<int>(name.size() * 2), static_cast<int>(strings.size())}) big16(table, value);
        for (char letter : name) big16(strings, letter);
    }
    append(table, strings);
    return table;
}

Bytes open_type(const std::map<std::string, Bytes>& tables)
{
    Bytes font;
    const auto count = static_cast<unsigned>(tables.size());
    const auto power = std::bit_floor(count);
    big32(font, 0x00010000);
    for (const auto value : {count, power * 16, static_cast<unsigned>(std::bit_width(power) - 1), (count - power) * 16}) big16(font, value);
    auto offset = 12u + 16u * count;
    unsigned adjustment{};
    for (const auto& [name, data] : tables)
    {
        font.insert(font.end(), name.begin(), name.end());
        big32(font, checksum(data));
        big32(font, offset);
        big32(font, static_cast<std::uint32_t>(data.size()));
        if (name == "head") adjustment = offset + 8;
        offset += (static_cast<unsigned>(data.size()) + 3) & ~3u;
    }
    for (const auto& [name, data] : tables)
    {
        append(font, data);
        while (font.size() % 4) font.push_back(0);
    }
    const auto correction = 0xb1b0afbau - checksum(font);
    metric_at(font, adjustment, correction >> 16);
    metric_at(font, adjustment + 2, correction & 0xffff);
    return font;
}
}

std::optional<web::FontAsset> web::web_font(const Movie& movie, std::size_t index)
{
    const auto& tag = movie.tags().at(index);
    if (!tag.character_id || (tag.code != 48 && tag.code != 75)) return std::nullopt;
    Reader descriptor(movie.data(index));
    descriptor.skip(2);
    const auto flags = descriptor.u8();
    descriptor.skip(1);
    const auto raw = descriptor.take(descriptor.u8());
    const std::string_view name(reinterpret_cast<const char*>(raw.data()), raw.size());
    const auto outlines = font_glyphs(movie, *tag.character_id);
    const auto metrics = font_metrics(movie, *tag.character_id);
    if (metrics.codes.empty() || metrics.codes.size() >= 65535 || outlines.offsets.size() != metrics.codes.size() + 1)
        return std::nullopt;
    const auto divisor = outlines.units / 1024;
    const auto scale = [divisor](int value) { return static_cast<int>(std::lround(static_cast<double>(value) / divisor)); };
    std::map<std::string, Bytes> tables;
    auto& glyf = tables["glyf"];
    auto& loca = tables["loca"];
    auto& hmtx = tables["hmtx"];
    big32(loca, 0); big16(hmtx, 512); big16(hmtx, 0);
    int max_points = 0, max_contours = 0, max_advance = 512, min_bearing = 0, min_right_bearing = 0;
    FontBox bounds;
    std::map<unsigned, unsigned> characters;
    for (unsigned i = 0; i < metrics.codes.size(); ++i)
    {
        big32(loca, static_cast<std::uint32_t>(glyf.size()));
        const auto glyph = true_type_glyph(glyf, glyph_path(outlines, i), divisor, max_points, max_contours);
        if (glyf.size() > 16 * 1024 * 1024) throw std::runtime_error("Police incorporée trop grande.");
        const auto advance = i < metrics.advances.size() ? std::clamp(scale(metrics.advances[i]), 0, 32767) :
            metrics.codes[i] == L' ' ? 256 : std::max(128, glyph.right - glyph.left + 60);
        big16(hmtx, advance);
        big16(hmtx, glyph.left);
        max_advance = std::max(max_advance, advance);
        min_bearing = std::min(min_bearing, glyph.left);
        min_right_bearing = std::min(min_right_bearing, advance - glyph.right);
        bounds.extend(glyph.left, glyph.bottom); bounds.extend(glyph.right, glyph.top);
        if (metrics.codes[i] != L'\uFFFD') characters.try_emplace(metrics.codes[i], i + 1);
    }
    if (characters.empty() || glyf.empty()) return std::nullopt;
    big32(loca, static_cast<std::uint32_t>(glyf.size()));
    const auto count = static_cast<int>(metrics.codes.size() + 1);
    const auto ascent = metrics.ascent ? std::abs(scale(metrics.ascent)) : std::max(bounds.top, 800);
    const auto descent = metrics.descent ? std::abs(scale(metrics.descent)) : std::max(-bounds.bottom, 224);
    const auto leading = scale(metrics.leading);
    auto& head = tables["head"];
    for (const auto value : {0x10000u, 0x10000u, 0u, 0x5f0f3cf5u}) big32(head, value);
    big16(head, 3); big16(head, 1024);
    head.resize(36);
    bounds.write(head);
    for (const auto value : {flags & 3, 8, 2, 1, 0}) big16(head, value);
    auto& hhea = tables["hhea"];
    big32(hhea, 0x00010000);
    for (const auto value : {ascent, -descent, leading, max_advance, min_bearing, min_right_bearing, bounds.right, 1, 0, 0}) big16(hhea, value);
    hhea.resize(34); big16(hhea, count);
    auto& maxp = tables["maxp"];
    big32(maxp, 0x00010000);
    for (const auto value : {count, max_points, max_contours, 0, 0, 1}) big16(maxp, value);
    maxp.resize(32);
    auto& os = tables["OS/2"];
    os.resize(78);
    metric_at(os, 2, max_advance / 2); metric_at(os, 4, flags & 1 ? 700 : 400); metric_at(os, 6, 5);
    os[58] = 'F'; os[59] = 'S'; os[60] = 'T'; os[61] = 'U';
    metric_at(os, 62, (flags & 2 ? 1 : 0) | (flags & 1 ? 32 : 0) | (flags & 3 ? 0 : 64));
    metric_at(os, 64, characters.begin()->first); metric_at(os, 66, characters.rbegin()->first);
    metric_at(os, 68, ascent); metric_at(os, 70, -descent); metric_at(os, 72, leading);
    metric_at(os, 74, std::max(ascent, bounds.top)); metric_at(os, 76, std::max(descent, -bounds.bottom));
    big32(tables["post"], 0x00030000); tables["post"].resize(32);
    auto& cmap = tables["cmap"];
    for (const auto value : {0, 2, 0, 4}) big16(cmap, value);
    big32(cmap, 20);
    big16(cmap, 3); big16(cmap, 10); big32(cmap, 20);
    big16(cmap, 12); big16(cmap, 0);
    big32(cmap, static_cast<std::uint32_t>(16 + 12 * characters.size()));
    big32(cmap, 0); big32(cmap, static_cast<std::uint32_t>(characters.size()));
    for (const auto& [character, glyph] : characters)
    {
        big32(cmap, character); big32(cmap, character); big32(cmap, glyph);
    }
    tables["name"] = font_names();
    return FontAsset{{L".ttf", open_type(tables)}, std::string(name.substr(0, name.find('\0'))),
        flags & 1 ? (flags & 2 ? "boldItalic" : "bold") : (flags & 2 ? "italic" : "regular")};
}
}
