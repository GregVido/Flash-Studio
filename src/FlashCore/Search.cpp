#include "Search.hpp"
#include "Abc.hpp"

#include <Windows.h>
#include <algorithm>
#include <format>
#include <regex>

namespace flash
{
namespace
{
std::wstring decode_utf8(std::string_view text)
{
    if (text.empty())
        return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

class Search
{
public:
    Search(std::wstring_view query, SearchOptions options, std::stop_token stop)
        : query_(query), options_(options), stop_(stop)
    {
        if (query.empty() || query.size() > 256 || query.find_first_of(L"\r\n") != std::wstring_view::npos)
            throw std::runtime_error("Saisissez un motif d’une ligne, limité à 256 caractères.");
        options_.limit = std::clamp<std::size_t>(options_.limit, 1, 1000);
        if (options.regex)
        {
            try
            {
                auto flags = std::regex_constants::ECMAScript | std::regex_constants::optimize;
                if (!options.match_case)
                    flags |= std::regex_constants::icase;
                regex_.emplace(query.begin(), query.end(), flags);
            }
            catch (const std::regex_error&) { throw std::runtime_error("Expression régulière invalide."); }
        }
    }

    SearchReport run(const Movie& movie);

private:
    bool includes(SearchArea area) const { return options_.area == SearchArea::all || options_.area == area; }
    bool done() const { return stop_.stop_requested() || report_.truncated; }
    void lines(std::wstring_view text, const SearchHit& origin);
    void abc(const Movie& movie, std::size_t index);

    template<typename Produce>
    void scan(const SearchHit& origin, const Produce& produce)
    {
        std::string text;
        try { text = produce(); }
        catch (const std::runtime_error&) { ++report_.skipped; return; }
        lines(decode_utf8(text), origin);
    }

    std::optional<std::pair<std::size_t, std::size_t>> match(std::wstring_view line) const
    {
        if (regex_)
        {
            std::match_results<std::wstring_view::const_iterator> found;
            try
            {
                if (std::regex_search(line.begin(), line.end(), found, *regex_))
                    return std::pair{static_cast<std::size_t>(found.position()), static_cast<std::size_t>(found.length())};
                return {};
            }
            catch (const std::regex_error&) { throw std::runtime_error("Expression régulière trop complexe pour cette recherche."); }
        }
        if (options_.match_case)
        {
            const auto position = line.find(query_);
            return position == std::wstring_view::npos ? std::nullopt : std::optional{std::pair{position, query_.size()}};
        }
        if (line.empty())
            return {};
        int length = 0;
        const auto position = FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | NORM_IGNORECASE,
            line.data(), static_cast<int>(line.size()), query_.data(), static_cast<int>(query_.size()), &length, nullptr, nullptr, 0);
        if (position < 0)
            return {};
        return std::pair{static_cast<std::size_t>(position), static_cast<std::size_t>(length)};
    }

    std::wstring query_;
    SearchOptions options_;
    std::stop_token stop_;
    std::optional<std::wregex> regex_;
    SearchReport report_;
};

void Search::lines(std::wstring_view text, const SearchHit& origin)
{
    auto number = origin.line;
    while (!text.empty() && !done())
    {
        const auto end = text.find_first_of(L"\r\n");
        const auto line = text.substr(0, end);
        if (const auto found = match(line))
        {
            if (report_.hits.size() == options_.limit)
            {
                report_.truncated = true;
                return;
            }
            const auto [position, length] = *found;
            const auto start = position > 75 ? position - 75 : 0;
            const auto width = std::min<std::size_t>(line.size() - start, 230);
            auto hit = origin;
            hit.line = number;
            hit.column = position + 1;
            hit.excerpt = std::wstring(line.substr(start, width));
            for (auto& c : hit.excerpt)
                if (c < 32 || c == 127)
                    c = L' ';
            if (start)
                hit.excerpt.insert(0, L"…");
            if (start + width < line.size())
                hit.excerpt += L'…';
            hit.highlight = position - start + (start != 0);
            hit.length = std::min(length, width - (position - start));
            report_.hits.push_back(std::move(hit));
        }
        if (end == std::wstring_view::npos)
            break;
        text.remove_prefix(end + (text.substr(end).starts_with(L"\r\n") ? 2 : 1));
        ++number;
    }
}

void Search::abc(const Movie& movie, std::size_t index)
{
    std::optional<AbcModule> parsed;
    try { parsed.emplace(abc_bytecode(movie, index)); }
    catch (const std::runtime_error&) { ++report_.skipped; return; }
    const auto& module = *parsed;
    const auto& types = module.classes();
    std::vector<std::optional<std::size_t>> class_of(module.methods().size());
    for (std::size_t type = 0; type < types.size() && !done(); ++type)
    {
        const auto& declaration = types[type];
        for (const auto& member : declaration.members)
            if (member.method)
                class_of[*member.method] = type;
        if (includes(SearchArea::source))
        {
            SearchHit hit{SearchArea::source, index, type, {}, {}};
            hit.location = decode_utf8(declaration.qualified);
            lines(hit.location, hit);
            scan(hit, [&] { return module.decompile_class(type).text; });
        }
    }
    for (const auto& method : module.methods())
    {
        if (done())
            break;
        if (includes(SearchArea::source) && !class_of[method.id] && !module.declares_classes(method.id))
        {
            SearchHit hit{SearchArea::source, index, {}, method.id, {}};
            hit.location = decode_utf8(method.name);
            scan(hit, [&] { return module.decompile(method.id); });
        }
        if (includes(SearchArea::instructions) && method.code_size && !done())
        {
            const auto type = class_of[method.id];
            SearchHit hit{SearchArea::instructions, index, type, method.id, {}};
            hit.location = decode_utf8(type ? types[*type].qualified + " · " + method.name : method.name);
            scan(hit, [&]
            {
                try { return module.pcode(method.id); }
                catch (const std::runtime_error&) { return module.disassemble(method.id); }
            });
        }
    }
    if (includes(SearchArea::constants))
        for (unsigned id = 1; id < module.strings().size() && !done(); ++id)
        {
            SearchHit hit{SearchArea::constants, index, {}, {}, id};
            hit.location = std::format(L"Constante #{}", id);
            lines(decode_utf8(module.strings()[id]), hit);
        }
}

SearchReport Search::run(const Movie& movie)
{
    for (std::size_t index = 0; index < movie.tags().size() && !done(); ++index)
    {
        const auto& tag = movie.tags()[index];
        const auto code = tag.code;
        const auto type = decode_utf8(tag_name(code));
        if (includes(SearchArea::tags))
        {
            SearchHit hit{SearchArea::tags, index, {}, {}, {}};
            hit.location = type;
            if (tag.character_id)
                hit.location += std::format(L" #{}", *tag.character_id);
            auto title = std::format(L"{}  tag #{}", hit.location, index);
            if (!tag.name.empty())
                title += L"  " + decode_utf8(tag.name);
            lines(title, hit);
        }
        if (done())
            break;
        if ((code == 72 || code == 82) && (includes(SearchArea::source) || includes(SearchArea::instructions) || includes(SearchArea::constants)))
            abc(movie, index);
        else if ((includes(SearchArea::source) || includes(SearchArea::instructions)) && has_avm1_actions(movie, index))
        {
            SearchHit hit{SearchArea::source, index, {}, {}, {}};
            hit.location = type;
            if (includes(SearchArea::source))
                scan(hit, [&] { return decompile(movie, index); });
            if (includes(SearchArea::instructions) && !done())
            {
                hit.area = SearchArea::instructions;
                scan(hit, [&] { return disassemble(movie, index); });
            }
        }
        else if (includes(SearchArea::text) && (code == 11 || code == 33 || code == 37 || code == 77))
        {
            SearchHit hit{SearchArea::text, index, {}, {}, {}};
            hit.location = type;
            scan(hit, [&] { return extract_text(movie, index); });
        }
    }
    return std::move(report_);
}
}

std::wstring_view search_area_name(SearchArea area)
{
    switch (area)
    {
    case SearchArea::all: return L"Tout le SWF";
    case SearchArea::source: return L"ActionScript";
    case SearchArea::instructions: return L"Instructions";
    case SearchArea::text: return L"Textes et métadonnées";
    case SearchArea::constants: return L"Constantes ABC";
    case SearchArea::tags: return L"Tags et ressources";
    }
    return {};
}

SearchReport search_movie(const Movie& movie, std::wstring_view query, const SearchOptions& options, std::stop_token stop)
{
    return Search(query, options, stop).run(movie);
}
}
