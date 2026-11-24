#include "Binary.hpp"

#include <Windows.h>

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
};

Field edit_field(ByteView bytes)
{
    Reader input(bytes);
    input.skip(2);
    skip_rect(input);
    const auto position = input.position();
    const auto flags = input.u8();
    const auto layout = input.u8();
    if (flags & 1)
        input.skip(2);
    if (layout & 128)
        input.cstring();
    if (flags & 1)
        input.skip(2);
    if (flags & 4)
        input.skip(4);
    if (flags & 2)
        input.skip(2);
    if (layout & 32)
        input.skip(9);
    input.cstring();
    const auto start = input.position();
    auto text = flags & 128 ? input.cstring() : std::string{};
    return {position, start, input.position(), std::move(text)};
}

struct CodeTable
{
    ByteView codes;
    bool wide{};
    unsigned page{};

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
        return {input.take(count * (wide_codes ? 2u : 1u)), wide_codes, page};
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
        return {input.take(input.remaining()), (flags & 1) != 0, page};
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

Movie Movie::replace_text(std::size_t index, std::string_view utf8) const
{
    const auto code = tags_.at(index).code;
    if (code != 37 && code != 77)
        throw std::runtime_error("Seuls les champs de texte dynamiques et les métadonnées peuvent être édités ici.");
    if (utf8.size() > max_text_size || utf8.find('\0') != std::string_view::npos)
        throw std::runtime_error("Texte trop long ou contenant un caractère nul.");
    if (utf8.starts_with("\xEF\xBB\xBF"))
        utf8.remove_prefix(3);
    const auto decoded = decode_string(utf8, CP_UTF8, MB_ERR_INVALID_CHARS);
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
