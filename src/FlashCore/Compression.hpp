#pragma once
#include "Swf.hpp"

namespace flash
{
Bytes inflate_zlib(ByteView data, std::size_t expected_size);
void inflate_zlib(ByteView data, std::span<std::uint8_t> output);
Bytes inflate_zip_entry(ByteView data, std::size_t expected_size);
void inflate_lzma(ByteView properties, ByteView data, std::span<std::uint8_t> output);
Bytes deflate_zlib(ByteView data);
}
