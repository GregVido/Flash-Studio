#pragma once

#include "Swf.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace flash
{
inline constexpr std::size_t max_file_size = 256 * 1024 * 1024;
inline constexpr std::size_t max_text_size = 2 * 1024 * 1024;
std::string encode_base64(ByteView bytes);

struct EmbeddedBitmap
{
    unsigned width{}, height{};
    std::string data_uri;
};

EmbeddedBitmap embedded_bitmap(const Movie& movie, std::size_t index);

class Reader
{
public:
    explicit Reader(ByteView bytes) : bytes_(bytes) {}
    std::size_t position() const noexcept { return position_; }
    std::size_t remaining() const noexcept { return bytes_.size() - position_; }

    ByteView take(std::size_t size)
    {
        if (size > remaining())
            throw std::runtime_error("Données tronquées.");
        const auto result = bytes_.subspan(position_, size);
        position_ += size;
        return result;
    }

    void skip(std::size_t size) { take(size); }
    std::uint8_t u8() { return take(1).front(); }
    std::uint16_t u16()
    {
        const auto bytes = take(2);
        return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
    }
    std::uint32_t u32()
    {
        const auto low = u16();
        return low | (static_cast<std::uint32_t>(u16()) << 16);
    }
    std::int32_t s24()
    {
        const auto bytes = take(3);
        const auto value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
        return (value & 0x800000) ? value - 0x1000000 : value;
    }
    std::uint32_t v32()
    {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift <= 28; shift += 7)
        {
            const auto byte = u8();
            if (shift == 28 && byte > 15)
                throw std::runtime_error("Entier ABC mal formé.");
            value |= static_cast<std::uint32_t>(byte & 127) << shift;
            if (byte < 128)
                return value;
        }
        throw std::runtime_error("Entier ABC mal formé.");
    }
    std::uint32_t u30()
    {
        const auto value = v32();
        if (value >= (1u << 30))
            throw std::runtime_error("Entier ABC hors limites.");
        return value;
    }
    double f64()
    {
        const auto low = u32();
        return std::bit_cast<double>((static_cast<std::uint64_t>(u32()) << 32) | low);
    }
    std::string string(std::size_t size)
    {
        const auto bytes = take(size);
        return {bytes.begin(), bytes.end()};
    }
    std::string cstring()
    {
        const auto rest = bytes_.subspan(position_);
        const auto end = std::ranges::find(rest, 0);
        if (end == rest.end())
            throw std::runtime_error("Chaîne SWF sans terminateur.");
        auto value = string(static_cast<std::size_t>(end - rest.begin()));
        skip(1);
        return value;
    }

private:
    ByteView bytes_;
    std::size_t position_ = 0;
};

class BitReader
{
public:
    explicit BitReader(Reader& input) : input_(input) {}
    void align() noexcept { left_ = 0; }
    std::uint32_t unsigned_bits(unsigned count)
    {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            if (left_ == 0)
            {
                current_ = input_.u8();
                left_ = 8;
            }
            value = (value << 1) | ((current_ >> --left_) & 1);
        }
        return value;
    }
    std::int32_t signed_bits(unsigned count)
    {
        if (!count)
            return 0;
        const auto value = unsigned_bits(count);
        const auto sign = std::uint32_t{1} << (count - 1);
        return std::bit_cast<std::int32_t>((value ^ sign) - sign);
    }
private:
    Reader& input_;
    unsigned current_ = 0, left_ = 0;
};

inline void skip_rect(Reader& reader)
{
    BitReader bits(reader);
    const auto count = bits.unsigned_bits(5);
    for (int i = 0; i < 4; ++i)
        bits.unsigned_bits(count);
}

struct Matrix
{
    double scale_x = 1, scale_y = 1, skew_x = 0, skew_y = 0;
    std::int32_t x = 0, y = 0;
};

inline Matrix read_matrix(Reader& reader)
{
    BitReader bits(reader);
    Matrix matrix;
    if (bits.unsigned_bits(1))
    {
        const auto width = bits.unsigned_bits(5);
        matrix.scale_x = bits.signed_bits(width) / 65536.0;
        matrix.scale_y = bits.signed_bits(width) / 65536.0;
    }
    if (bits.unsigned_bits(1))
    {
        const auto width = bits.unsigned_bits(5);
        matrix.skew_y = bits.signed_bits(width) / 65536.0;
        matrix.skew_x = bits.signed_bits(width) / 65536.0;
    }
    const auto width = bits.unsigned_bits(5);
    matrix.x = bits.signed_bits(width);
    matrix.y = bits.signed_bits(width);
    return matrix;
}

inline void put16(Bytes& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

inline void put32(Bytes& bytes, std::uint32_t value)
{
    put16(bytes, static_cast<std::uint16_t>(value));
    put16(bytes, static_cast<std::uint16_t>(value >> 16));
}

inline void append(Bytes& bytes, ByteView suffix)
{
    if (suffix.size() > max_file_size || bytes.size() > max_file_size - suffix.size())
        throw std::runtime_error("La taille maximale prise en charge est de 256 Mio.");
    bytes.insert(bytes.end(), suffix.begin(), suffix.end());
}
}
