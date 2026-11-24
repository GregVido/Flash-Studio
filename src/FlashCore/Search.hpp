#pragma once

#include "Swf.hpp"

#include <stop_token>

namespace flash
{
enum class SearchArea { all, source, instructions, text, constants, tags };

struct SearchOptions
{
    SearchArea area = SearchArea::all;
    bool match_case = false;
    bool regex = false;
    std::size_t limit = 250;
};

struct SearchHit
{
    SearchArea area;
    std::size_t tag;
    std::optional<std::size_t> type;
    std::optional<unsigned> method, constant;
    std::size_t line = 1, column = 1;
    std::wstring location, excerpt;
    std::size_t highlight = 0, length = 0;
};

struct SearchReport
{
    std::vector<SearchHit> hits;
    std::size_t skipped = 0;
    bool truncated = false;
};

std::wstring_view search_area_name(SearchArea area);
SearchReport search_movie(const Movie& movie, std::wstring_view query, const SearchOptions& options = {},
    std::stop_token stop = {});
}
