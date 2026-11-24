#include "Avm2StringEval.hpp"

#include <cmath>
#include <variant>
#include <Windows.h>

namespace flash::script
{
namespace
{
[[noreturn]] void unsupported()
{
    throw std::runtime_error("Expression de chaîne non prise en charge.");
}

struct Null {};
struct Array
{
    const StringTable* table;
    const std::vector<double>* row;
};
using Value = std::variant<std::monostate, Null, double, std::wstring, Array, StringOperand>;

std::uint32_t uint32(double n)
{
    if (!std::isfinite(n))
        return 0;
    const auto wrapped = std::fmod(std::trunc(n), 4294967296.0);
    return static_cast<std::uint32_t>(wrapped < 0 ? wrapped + 4294967296.0 : wrapped);
}

double int32(double n)
{
    return std::bit_cast<std::int32_t>(uint32(n));
}

double numeric(const Value& value)
{
    if (const auto number = std::get_if<double>(&value))
        return *number;
    if (std::holds_alternative<Null>(value))
        return 0;
    if (std::holds_alternative<std::monostate>(value))
        return std::numeric_limits<double>::quiet_NaN();
    unsupported();
}

std::size_t array_index(double n, std::size_t size)
{
    if (!(n >= 0 && n < static_cast<double>(size) && std::trunc(n) == n))
        unsupported();
    return static_cast<std::size_t>(n);
}

bool equivalent(const Value& a, const Value& b)
{
    if (const auto text = std::get_if<std::wstring>(&a))
        if (const auto other = std::get_if<std::wstring>(&b))
            return *text == *other;
    if (a.index() != b.index())
        unsupported();
    if (std::holds_alternative<std::monostate>(a))
        return true;
    return numeric(a) == numeric(b);
}

std::string string_utf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const auto length = static_cast<int>(text.size());
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    if (!size)
        unsupported();
    std::string output(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, output.data(), size, nullptr, nullptr);
    return output;
}
}

std::wstring string_units(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    if (utf8.size() > 4096)
        unsupported();
    const auto length = static_cast<int>(utf8.size());
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
    if (!size)
        unsupported();
    std::wstring output(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, output.data(), size);
    return output;
}

std::optional<std::string> StringDecoder::evaluate(std::span<const std::int32_t> arguments) const
{
    constexpr std::size_t stack_limit = 64, string_limit = 4096;
    if (arguments.size() != parameters || locals <= parameters || locals > 64)
        return {};
    try
    {
        std::vector<Value> registers(locals), stack;
        for (std::size_t i = 0; i < arguments.size(); ++i)
            registers[i + 1] = double(arguments[i]);
        const auto push = [&](Value value)
        {
            if (stack.size() >= stack_limit)
                unsupported();
            stack.push_back(std::move(value));
        };
        const auto pop = [&]()
        {
            if (stack.empty())
                unsupported();
            auto value = std::move(stack.back());
            stack.pop_back();
            return value;
        };
        for (std::size_t pc = 0, steps = 0; pc < code.size() && steps < 80'000; ++steps)
        {
            const auto& item = code[pc++];
            switch (item.op)
            {
            case 0x02: case 0x09: case 0x82: break;
            case 0x10: pc = item.index; break;
            case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
            {
                const auto right = pop(), left = pop();
                bool taken;
                switch (item.op)
                {
                case 0x13: taken = equivalent(left, right); break;
                case 0x14: taken = !equivalent(left, right); break;
                case 0x15: taken = numeric(left) < numeric(right); break;
                case 0x16: taken = numeric(left) <= numeric(right); break;
                case 0x17: taken = numeric(left) > numeric(right); break;
                default: taken = numeric(left) >= numeric(right); break;
                }
                if (taken)
                    pc = item.index;
                break;
            }
            case 0x20: push(Null{}); break;
            case 0x21: push(std::monostate{}); break;
            case 0x24: case 0x25: case 0x2D: case 0x2E: case 0x2F: push(item.number); break;
            case 0x2C: push(item.text); break;
            case 0x48:
            {
                const auto result = pop();
                if (const auto string = std::get_if<std::wstring>(&result); string && stack.empty())
                    return string_utf8(*string);
                return {};
            }
            case 0x5D: push(item.member); break;
            case 0x60:
                if (item.member == StringOperand::table)
                    push(Array{&tables.at(item.index), nullptr});
                else
                    push(item.member);
                break;
            case 0x62: push(registers.at(item.index)); break;
            case 0x63: registers.at(item.index) = pop(); break;
            case 0x66:
            {
                if (item.member == StringOperand::index)
                {
                    const auto index = numeric(pop());
                    const auto array = std::get<Array>(pop());
                    if (array.row)
                        push(array.row->at(array_index(index, array.row->size())));
                    else
                        push(Array{array.table, &array.table->at(array_index(index, array.table->size()))});
                }
                else
                {
                    const auto receiver = pop();
                    if (const auto string = std::get_if<std::wstring>(&receiver))
                        push(double(string->size()));
                    else
                    {
                        const auto array = std::get<Array>(receiver);
                        push(double(array.row ? array.row->size() : array.table->size()));
                    }
                }
                break;
            }
            case 0x73: case 0x83: push(int32(numeric(pop()))); break;
            case 0x85:
                if (!stack.empty() && std::holds_alternative<std::monostate>(stack.back()))
                    stack.back() = Null{};
                else if (stack.empty() || (!std::holds_alternative<Null>(stack.back()) && !std::holds_alternative<std::wstring>(stack.back())))
                    unsupported();
                break;
            case 0x93: push(numeric(pop()) - 1); break;
            case 0xC0: push(int32(int32(numeric(pop())) + 1)); break;
            case 0xA0:
            {
                const auto right = pop();
                auto left = pop();
                if (auto first = std::get_if<std::wstring>(&left))
                {
                    const auto& second = std::get<std::wstring>(right);
                    if (second.size() > string_limit - first->size())
                        unsupported();
                    *first += second;
                    push(std::move(left));
                }
                else
                    push(numeric(left) + numeric(right));
                break;
            }
            case 0xA1:
            {
                const auto right = numeric(pop());
                push(numeric(pop()) - right);
                break;
            }
            case 0xA5: case 0xA7: case 0xA8: case 0xAA:
            {
                const auto right = uint32(numeric(pop()));
                const auto left = uint32(numeric(pop()));
                if (item.op == 0xA7)
                    push(double(left >> (right & 31)));
                else
                {
                    const auto bits = item.op == 0xA5 ? left << (right & 31) :
                        item.op == 0xA8 ? left & right : left ^ right;
                    push(double(std::bit_cast<std::int32_t>(bits)));
                }
                break;
            }
            case 0x46:
            {
                std::vector<Value> args(item.count);
                for (auto argument = args.rbegin(); argument != args.rend(); ++argument)
                    *argument = pop();
                auto receiver = pop();
                if (item.member == StringOperand::integer && std::get<StringOperand>(receiver) == StringOperand::integer)
                    push(int32(numeric(args[0])));
                else if (item.member == StringOperand::from_char_code && std::get<StringOperand>(receiver) == StringOperand::string)
                {
                    std::wstring text;
                    for (const auto& argument : args)
                        text += static_cast<wchar_t>(uint32(numeric(argument)) & 0xFFFF);
                    push(std::move(text));
                }
                else if (item.member == StringOperand::char_code_at)
                {
                    const auto& text = std::get<std::wstring>(receiver);
                    const auto index = int32(numeric(args[0]));
                    push(index >= 0 && index < double(text.size()) ? double(text[static_cast<std::size_t>(index)]) :
                        std::numeric_limits<double>::quiet_NaN());
                }
                else if (item.member == StringOperand::substring)
                {
                    auto& text = std::get<std::wstring>(receiver);
                    const auto length = double(text.size());
                    const auto first = static_cast<std::size_t>(std::clamp(int32(numeric(args[0])), 0.0, length));
                    const auto last = static_cast<std::size_t>(std::clamp(int32(numeric(args[1])), 0.0, length));
                    push(text.substr(std::min(first, last), std::max(first, last) - std::min(first, last)));
                }
                else
                    unsupported();
                break;
            }
            default: unsupported();
            }
        }
    }
    catch (const std::runtime_error&) {}
    catch (const std::bad_variant_access&) {}
    return {};
}
}
