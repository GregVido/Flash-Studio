#pragma once

#include "Swf.hpp"

namespace flash
{
Movie compile_as3_class(const Movie& movie, std::size_t tag, std::string_view qualified_name, std::string_view source,
    const std::filesystem::path& compiler, bool french = false);
Movie add_as3_class(const Movie& movie, std::string_view qualified_name, std::string_view source,
    const std::filesystem::path& compiler, bool french = false);
bool valid_as3_class_name(std::string_view qualified_name);
bool supports_as3_class_creation(const Movie& movie);
}
