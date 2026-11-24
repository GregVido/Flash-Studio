#include "Binary.hpp"
#include "Compression.hpp"

#include <cstdlib>

#define Z_SOLO
#define ZLIB_CONST
#include "zlib.h"
#include "LzmaDec.h"

namespace flash
{
namespace
{
z_stream stream_for(ByteView input)
{
    if (input.size() > max_file_size)
        throw std::runtime_error("Flux zlib trop volumineux.");
    z_stream stream{};
    stream.zalloc = [](void*, unsigned items, unsigned size) { return std::calloc(items, size); };
    stream.zfree = [](void*, void* value) { std::free(value); };
    stream.next_in = input.data();
    stream.avail_in = static_cast<uInt>(input.size());
    return stream;
}
void inflate_checked(ByteView data, std::span<std::uint8_t> output, int window_bits)
{
    if (output.size() > max_file_size)
        throw std::runtime_error("Flux zlib trop volumineux.");
    auto stream = stream_for(data);
    if (inflateInit2(&stream, window_bits) != Z_OK)
        throw std::runtime_error("Impossible d’initialiser zlib.");
    std::uint8_t dummy{};
    stream.next_out = output.empty() ? &dummy : output.data();
    stream.avail_out = static_cast<uInt>(std::max<std::size_t>(1, output.size()));
    const auto result = inflate(&stream, Z_FINISH);
    const auto consumed = stream.total_in, produced = stream.total_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END || consumed != data.size() || produced != output.size())
        throw std::runtime_error("Flux zlib invalide, tronqué ou de taille inattendue.");
}
}

void inflate_zlib(ByteView data, std::span<std::uint8_t> output)
{
    inflate_checked(data, output, MAX_WBITS);
}

Bytes inflate_zip_entry(ByteView data, std::size_t expected_size)
{
    if (expected_size > max_file_size)
        throw std::runtime_error("The SWC library is too large.");
    Bytes output(expected_size);
    inflate_checked(data, output, -MAX_WBITS);
    return output;
}

Bytes inflate_zlib(ByteView data, std::size_t expected_size)
{
    if (expected_size > max_file_size)
        throw std::runtime_error("Flux zlib trop volumineux.");
    Bytes output(expected_size);
    inflate_zlib(data, output);
    return output;
}

void inflate_lzma(ByteView properties, ByteView data, std::span<std::uint8_t> output)
{
    const ISzAlloc allocator{
        [](ISzAllocPtr, std::size_t size) { return std::malloc(size); },
        [](ISzAllocPtr, void* allocation) { std::free(allocation); }
    };
    SizeT actual_size = output.size(), consumed = data.size();
    ELzmaStatus status{};
    const auto result = LzmaDecode(output.data(), &actual_size, data.data(), &consumed,
        properties.data(), static_cast<unsigned>(properties.size()), LZMA_FINISH_END, &status, &allocator);
    if (result != SZ_OK || actual_size != output.size() ||
        (status != LZMA_STATUS_FINISHED_WITH_MARK && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK))
        throw std::runtime_error("Flux LZMA invalide ou taille décompressée incorrecte.");
}

Bytes deflate_zlib(ByteView data)
{
    auto stream = stream_for(data);
    if (deflateInit(&stream, Z_BEST_SPEED) != Z_OK)
        throw std::runtime_error("Impossible d’initialiser zlib.");
    struct Cleanup
    {
        z_stream& stream;
        ~Cleanup() { deflateEnd(&stream); }
    } cleanup{stream};

    Bytes output(deflateBound(&stream, static_cast<uLong>(data.size())));
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END)
        throw std::runtime_error("Impossible de compresser les données zlib.");
    output.resize(stream.total_out);
    return output;
}
}
