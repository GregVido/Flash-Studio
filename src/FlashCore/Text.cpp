#include "Binary.hpp"
#include "WebExport.hpp"

#include <Windows.h>
#include <array>
#include <bit>
#include <format>

namespace flash
{
namespace
{
constexpr unsigned shift_jis = 932;
constexpr unsigned windows_latin = 1252;

std::wstring decode_string(std::string_view value, unsigned page, DWORD flags = 0)
{
    if (value.empty())
        return {};
    const auto size = static_cast<int>(value.size());
    const auto count = MultiByteToWideChar(page, flags, value.data(), size, nullptr, 0);
    if (!count)
        throw std::runtime_error("Encodage de texte invalide.");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(page, flags, value.data(), size, result.data(), count);
    return result;
}

std::string encode_string(std::wstring_view value, unsigned page)
{
    if (value.empty())
        return {};
    BOOL substituted = false;
    const auto flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    auto used_default = page == CP_UTF8 ? nullptr : &substituted;
    const auto size = static_cast<int>(value.size());
    const auto count = WideCharToMultiByte(page, flags, value.data(), size, nullptr, 0, nullptr, used_default);
    if (!count || substituted)
        throw std::runtime_error("Ce texte contient des caractères incompatibles avec son encodage.");
    std::string result(count, '\0');
    WideCharToMultiByte(page, flags, value.data(), size, result.data(), count, nullptr, used_default);
    return result;
}

struct Field
{
    std::size_t flags_offset;
    std::size_t text_offset;
    std::size_t end_offset;
    std::string initial;
    std::array<int, 4> bounds{};
    unsigned font_height = 240;
    unsigned font_id{};
    std::string font_class;
    std::array<unsigned, 4> color{0, 0, 0, 255};
    unsigned alignment{}, left_margin{}, right_margin{}, maximum{}, flags{}, layout{};
    int leading{};
};

Field edit_field(ByteView bytes)
{
    Reader input(bytes);
    input.skip(2);
    BitReader box(input);
    const auto width = box.unsigned_bits(5);
    const std::array bounds{box.signed_bits(width), box.signed_bits(width), box.signed_bits(width), box.signed_bits(width)};
    const auto position = input.position();
    const auto flags = input.u8();
    const auto layout = input.u8();
    Field field{};
    if (flags & 1)
        field.font_id = input.u16();
    if (layout & 128)
        field.font_class = input.cstring();
    field.bounds = bounds;
    field.flags = flags;
    field.layout = layout;
    if (flags & 1)
        field.font_height = input.u16();
    if (flags & 4)
        for (auto& channel : field.color)
            channel = input.u8();
    if (flags & 2)
        field.maximum = input.u16();
    if (layout & 32)
    {
        field.alignment = input.u8();
        field.left_margin = input.u16();
        field.right_margin = input.u16();
        input.skip(2);
        field.leading = std::bit_cast<std::int16_t>(input.u16());
    }
    input.cstring();
    field.flags_offset = position;
    field.text_offset = input.position();
    if (flags & 128)
        field.initial = input.cstring();
    field.end_offset = input.position();
    return field;
}

struct CodeTable
{
    ByteView codes;
    bool wide{};
    unsigned page{};
    ByteView advances;
    unsigned units = 1024;
    int ascent{}, descent{}, leading{};

    wchar_t glyph(unsigned index) const
    {
        constexpr auto replacement = L'\uFFFD';
        const std::size_t width = wide ? 2 : 1;
        if (index >= codes.size() / width)
            return replacement;
        Reader slot(codes.subspan(index * width, width));
        const auto code = wide ? slot.u16() : slot.u8();
        if (!code)
            return replacement;
        if (!page)
            return code >= 0xD800 && code <= 0xDFFF ? replacement : static_cast<wchar_t>(code);
        const char encoded[]{static_cast<char>(code >> 8), static_cast<char>(code & 255)};
        const bool pair = code > 255;
        wchar_t character{};
        return MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, encoded + (pair ? 0 : 1), pair ? 2 : 1,
            &character, 1) == 1 ? character : replacement;
    }
};

CodeTable font_codes(const Movie& movie, unsigned id)
{
    const auto definition = movie.character(id);
    if (!definition)
        return {};
    const auto code = movie.tags()[*definition].code;
    if (code == 48 || code == 75)
    {
        auto payload = movie.data(*definition);
        Reader input(payload);
        input.skip(2);
        const auto flags = input.u8();
        input.skip(1);
        input.skip(input.u8());
        const auto count = input.u16();
        if (!count)
            return {};
        const auto offset_base = input.position();
        const auto wide_offsets = (flags & 8) != 0;
        input.skip(count * (wide_offsets ? 4u : 2u));
        const auto codes_offset = wide_offsets ? input.u32() : input.u16();
        if (codes_offset < input.position() - offset_base)
            throw std::runtime_error("Table de caractères SWF mal positionnée.");
        input.skip(codes_offset - (input.position() - offset_base));
        const auto wide_codes = (flags & 4) != 0;
        const auto page = movie.header().version >= 6 ? 0 : flags & 64 ? shift_jis : windows_latin;
        const auto codes = input.take(count * (wide_codes ? 2u : 1u));
        CodeTable table{codes, wide_codes, page, {}, code == 75 ? 20480u : 1024u};
        if ((flags & 128) && input.remaining() >= 6 + count * 2u)
        {
            table.ascent = std::bit_cast<std::int16_t>(input.u16());
            table.descent = std::bit_cast<std::int16_t>(input.u16());
            table.leading = std::bit_cast<std::int16_t>(input.u16());
            table.advances = input.take(count * 2u);
        }
        return table;
    }
    if (code != 10)
        return {};
    for (std::size_t i = 0; i < movie.tags().size(); ++i)
    {
        const auto info = movie.tags()[i].code;
        if (info != 13 && info != 62)
            continue;
        Reader input(movie.data(i));
        if (input.u16() != id)
            continue;
        input.skip(input.u8());
        const auto flags = input.u8();
        if (info == 62)
            input.skip(1);
        const auto page = movie.header().version >= 6 ? 0 : flags & 16 ? shift_jis : windows_latin;
        return {input.take(input.remaining()), (flags & 1) != 0, page, {}, 1024};
    }
    return {};
}

std::string static_text(const Movie& movie, std::size_t index)
{
    Reader input(movie.data(index));
    input.skip(2);
    skip_rect(input);
    read_matrix(input);
    const auto glyph_bits = input.u8(), advance_bits = input.u8();
    if (glyph_bits > 32 || advance_bits > 32)
        throw std::runtime_error("En-tête de texte SWF invalide.");
    CodeTable table;
    std::unordered_map<unsigned, CodeTable> tables;
    std::int16_t y = 0;
    std::wstring result;
    while (const auto flags = input.u8())
    {
        if (!(flags & 128))
            throw std::runtime_error("Enregistrement de texte SWF invalide.");
        if (flags & 8)
        {
            const auto id = input.u16();
            auto [entry, inserted] = tables.try_emplace(id);
            if (inserted)
                entry->second = font_codes(movie, id);
            table = entry->second;
        }
        if (flags & 4)
            input.skip(movie.tags()[index].code == 33 ? 4 : 3);
        if (flags & 1)
            input.skip(2);
        if (flags & 2)
        {
            const auto baseline = std::bit_cast<std::int16_t>(input.u16());
            if (baseline != y && !result.empty())
                result += L'\n';
            y = baseline;
        }
        if (flags & 8)
            input.skip(2);
        const auto glyphs = input.u8();
        if (result.size() + glyphs > max_text_size / 3)
            throw std::runtime_error("Le texte extrait dépasse la taille maximale.");
        BitReader bits(input);
        for (unsigned i = 0; i < glyphs; ++i)
        {
            result += table.glyph(bits.unsigned_bits(glyph_bits));
            bits.unsigned_bits(advance_bits);
        }
    }
    return encode_string(result, CP_UTF8);
}

class TextBits
{
public:
    explicit TextBits(Bytes& bytes) : bytes_(bytes) {}
    void put(std::uint32_t value, unsigned width)
    {
        while (width)
        {
            if (!used_)
                bytes_.push_back(0);
            --width;
            bytes_.back() |= static_cast<std::uint8_t>(((value >> width) & 1) << (7 - used_));
            used_ = (used_ + 1) % 8;
        }
    }
private:
    Bytes& bytes_;
    unsigned used_ = 0;
};

Bytes edit_static_text(const Movie& movie, std::size_t index, std::wstring_view text)
{
    const auto original = movie.data(index);
    const auto alpha = movie.tags()[index].code == 33;
    Reader input(original);
    const auto character = input.u16();
    std::array<std::int32_t, 4> bounds{};
    {
        BitReader rect(input);
        const auto width = rect.unsigned_bits(5);
        for (auto& coordinate : bounds)
            coordinate = rect.signed_bits(width);
    }
    const auto matrix_start = input.position();
    read_matrix(input);
    const auto matrix_length = input.position() - matrix_start;
    const auto glyph_bits = input.u8(), advance_bits = input.u8();
    if (glyph_bits > 32 || advance_bits > 32)
        throw std::runtime_error("En-tête de texte SWF invalide.");
    struct Style
    {
        unsigned font{}, height{};
        std::array<std::uint8_t, 4> color{0, 0, 0, 255};
        bool operator==(const Style&) const = default;
    };
    Style read_style;
    std::optional<Style> style;
    std::vector<std::pair<std::int64_t, int>> origins;
    std::unordered_map<unsigned, int> advances;
    std::int64_t x = 0;
    int y = 0;
    while (const auto flags = input.u8())
    {
        if (!(flags & 128))
            throw std::runtime_error("Enregistrement de texte SWF invalide.");
        if (flags & 8)
            read_style.font = input.u16();
        if (flags & 4)
        {
            auto color = input.take(alpha ? 4 : 3);
            std::ranges::copy(color, read_style.color.begin());
        }
        if (flags & 1)
            x = std::bit_cast<std::int16_t>(input.u16());
        if (flags & 2)
            y = std::bit_cast<std::int16_t>(input.u16());
        if (flags & 8)
            read_style.height = input.u16();
        const auto count = input.u8();
        if (count)
        {
            if (style && *style != read_style)
                throw std::runtime_error("Ce texte statique utilise plusieurs styles de glyphes.");
            style = read_style;
            if (origins.empty() || origins.back().second != y)
                origins.emplace_back(x, y);
        }
        BitReader entries(input);
        for (unsigned i = 0; i < count; ++i)
        {
            const auto glyph = entries.unsigned_bits(glyph_bits);
            const auto advance = entries.signed_bits(advance_bits);
            advances.try_emplace(glyph, advance);
            x += advance;
        }
    }
    if (!style || !style->font || !style->height)
        throw std::runtime_error("Le texte statique n’a pas de police incorporée.");
    const auto font = font_codes(movie, style->font);
    const auto glyph_count = font.codes.size() / (font.wide ? 2 : 1);
    std::unordered_map<wchar_t, unsigned> glyphs;
    for (unsigned i = 0; i < glyph_count; ++i)
        if (const auto glyph = font.glyph(i); glyph != L'\uFFFD')
            glyphs.try_emplace(glyph, i);
    const auto index_width = std::max(1u, static_cast<unsigned>(
        std::bit_width(static_cast<unsigned>(glyph_count ? glyph_count - 1 : 0))));
    const auto allowed = [](std::int64_t value) { return value >= INT16_MIN && value <= INT16_MAX; };
    const int step = origins.size() > 1 ? origins[1].second - origins[0].second : static_cast<int>(style->height * 6 / 5);
    Bytes records;
    std::size_t line = 0;
    for (std::size_t start = 0; start < text.size(); ++line)
    {
        const auto end = text.find_first_of(L"\r\n", start);
        const auto current = text.substr(start, end == std::wstring_view::npos ? end : end - start);
        auto baseline = line < origins.size() ? std::int64_t{origins[line].second} :
            origins.back().second + static_cast<std::int64_t>(line + 1 - origins.size()) * step;
        const auto origin = line < origins.size() ? origins[line].first : origins.back().first;
        if (!allowed(origin) || !allowed(baseline))
            throw std::runtime_error("Le texte dépasse les coordonnées SWF disponibles.");
        auto right = origin;
        for (std::size_t first = 0; first < current.size() || !first; first += 255)
        {
            records.push_back(first ? 0x80 : 0x8F);
            if (!first)
            {
                put16(records, static_cast<std::uint16_t>(style->font));
                append(records, ByteView{style->color}.first(alpha ? 4 : 3));
                put16(records, static_cast<std::uint16_t>(origin));
                put16(records, static_cast<std::uint16_t>(baseline));
                put16(records, static_cast<std::uint16_t>(style->height));
            }
            const auto part = current.substr(first, 255);
            records.push_back(static_cast<std::uint8_t>(part.size()));
            TextBits entries(records);
            for (const auto letter : part)
            {
                const auto glyph = glyphs.find(letter);
                if (glyph == glyphs.end())
                    throw std::runtime_error(std::format("Le caractère U+{:04X} est absent de la police incorporée.",
                        static_cast<unsigned>(letter)));
                int advance = static_cast<int>(style->height / 2);
                if (const auto previous = advances.find(glyph->second); previous != advances.end())
                    advance = previous->second;
                else if (glyph->second < font.advances.size() / 2)
                {
                    Reader metric(font.advances.subspan(glyph->second * 2));
                    advance = std::bit_cast<std::int16_t>(metric.u16()) * static_cast<int>(style->height) /
                        static_cast<int>(font.units);
                }
                if (!allowed(advance))
                    throw std::runtime_error("Avance de glyphe hors limites.");
                entries.put(glyph->second, index_width);
                entries.put(static_cast<std::uint16_t>(advance), 16);
                right += advance;
                if (right < -(1ll << 30) || right >= (1ll << 30))
                    throw std::runtime_error("Largeur du texte SWF hors limites.");
            }
        }
        bounds[0] = static_cast<std::int32_t>(std::min<std::int64_t>(bounds[0], origin));
        bounds[1] = static_cast<std::int32_t>(std::max<std::int64_t>(bounds[1], right));
        bounds[2] = static_cast<std::int32_t>(std::min<std::int64_t>(bounds[2], baseline - style->height));
        bounds[3] = static_cast<std::int32_t>(std::max<std::int64_t>(bounds[3], baseline + style->height / 4));
        if (end == std::wstring_view::npos)
            break;
        start = end + 1 + (text[end] == L'\r' && end + 1 < text.size() && text[end + 1] == L'\n');
    }
    Bytes payload;
    put16(payload, character);
    unsigned width = 1;
    for (const auto coordinate : bounds)
        width = std::max(width, 1u + std::bit_width(static_cast<unsigned>(coordinate < 0 ? ~coordinate : coordinate)));
    if (width > 31)
        throw std::runtime_error("Bornes de texte SWF hors limites.");
    TextBits rectangle(payload);
    rectangle.put(width, 5);
    for (const auto coordinate : bounds)
        rectangle.put(static_cast<std::uint32_t>(coordinate), width);
    append(payload, original.subspan(matrix_start, matrix_length));
    payload.push_back(static_cast<std::uint8_t>(index_width));
    payload.push_back(16);
    append(payload, records);
    payload.push_back(0);
    return payload;
}
}

std::vector<wchar_t> font_glyph_labels(const Movie& movie, unsigned id)
{
    const auto table = font_codes(movie, id);
    std::vector<wchar_t> labels;
    const auto count = table.codes.size() / (table.wide ? 2 : 1);
    labels.reserve(count);
    for (unsigned i = 0; i < count; ++i)
        labels.push_back(table.glyph(i));
    return labels;
}

web::FontMetrics web::font_metrics(const Movie& movie, unsigned id)
{
    const auto table = font_codes(movie, id);
    FontMetrics result{font_glyph_labels(movie, id), {}, table.ascent, table.descent, table.leading};
    Reader advances(table.advances);
    while (advances.remaining())
        result.advances.push_back(std::bit_cast<std::int16_t>(advances.u16()));
    return result;
}

std::string extract_text(const Movie& movie, std::size_t index)
{
    const auto code = movie.tags().at(index).code;
    if (code == 11 || code == 33)
        return static_text(movie, index);
    if (code == 37)
    {
        const auto text = edit_field(movie.data(index)).initial;
        return movie.header().version >= 6 ? text : encode_string(decode_string(text, windows_latin), CP_UTF8);
    }
    if (code == 77)
        return Reader(movie.data(index)).cstring();
    throw std::runtime_error("Ce tag ne contient pas de texte pris en charge.");
}

std::string web::text_field_json(const Movie& movie, std::size_t index)
{
    const auto field = edit_field(movie.data(index));
    const auto text = movie.header().version >= 6 ? field.initial : encode_string(decode_string(field.initial, windows_latin), CP_UTF8);
    const auto [left, right, top, bottom] = field.bounds;
    return std::format("{{\"text\":{},\"bounds\":[{},{},{},{}],\"height\":{},\"fill\":\"#{:02x}{:02x}{:02x}\","
        "\"alpha\":{:g},\"alignment\":{},\"margins\":[{},{}],\"leading\":{},\"html\":{},"
        "\"wrap\":{},\"multiline\":{},\"editable\":{},\"password\":{},\"selectable\":{},\"border\":{},\"maximum\":{},\"autoSize\":{},"
        "\"fontId\":{},\"fontClass\":{},\"embedFonts\":{}}}",
        json_string(text), left, top, std::max(1, right - left), std::max(1, bottom - top),
        field.font_height, field.color[0], field.color[1], field.color[2], field.color[3] / 255.0,
        field.alignment, field.left_margin, field.right_margin, field.leading, bool(field.layout & 2),
        bool(field.flags & 64), bool(field.flags & 32), !(field.flags & 8), bool(field.flags & 16),
        !(field.layout & 16), bool(field.layout & 8), field.maximum,
        json_string(field.layout & 64 ? "left" : "none"), field.font_id, json_string(field.font_class), bool(field.layout & 1));
}

Movie Movie::replace_text(std::size_t index, std::string_view utf8) const
{
    const auto code = tags_.at(index).code;
    if (code != 11 && code != 33 && code != 37 && code != 77)
        throw std::runtime_error("Ce tag ne contient pas de texte éditable.");
    if (utf8.size() > max_text_size || utf8.find('\0') != std::string_view::npos)
        throw std::runtime_error("Texte trop long ou contenant un caractère nul.");
    if (utf8.starts_with("\xEF\xBB\xBF"))
        utf8.remove_prefix(3);
    const auto decoded = decode_string(utf8, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (code == 11 || code == 33)
        return replace_tag(index, edit_static_text(*this, index, decoded));
    const auto text = code == 37 && header_.version < 6 ? encode_string(decoded, windows_latin) : std::string(utf8);
    Bytes payload;
    if (code == 37)
    {
        const auto original = data(index);
        const auto field = edit_field(original);
        append(payload, original.first(field.text_offset));
        payload[field.flags_offset] |= 128;
        payload.insert(payload.end(), text.begin(), text.end());
        payload.push_back(0);
        append(payload, original.subspan(field.end_offset));
    }
    else
    {
        payload.assign(text.begin(), text.end());
        payload.push_back(0);
    }
    return replace_tag(index, payload);
}
}
