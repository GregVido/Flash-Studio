#include "Binary.hpp"

#include <array>
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
}
