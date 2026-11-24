#pragma once
#include "Swf.hpp"
#include <utility>

namespace flash::script
{
Bytes hide_class_for_import(ByteView abc, std::size_t type);
Bytes link_class(ByteView destination, std::size_t type, ByteView compiled,
    std::span<const std::pair<unsigned, unsigned>> existing_methods);
}
