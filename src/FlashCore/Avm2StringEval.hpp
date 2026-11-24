#pragma once

#include "Binary.hpp"

namespace flash::script
{
enum class StringOperand { integer, string, from_char_code, char_code_at, substring, index, length, table };

struct StringInstruction
{
    unsigned op{};
    std::size_t index{};
    unsigned count{};
    StringOperand member{};
    double number{};
    std::wstring text;
};

using StringTable = std::vector<std::vector<double>>;

struct StringDecoder
{
    std::vector<StringInstruction> code;
    std::vector<StringTable> tables;
    unsigned parameters{}, locals{};

    std::optional<std::string> evaluate(std::span<const std::int32_t> arguments) const;
};

std::wstring string_units(std::string_view utf8);
}
