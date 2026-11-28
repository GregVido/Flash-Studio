#pragma once
#include "Binary.hpp"

namespace flash::script
{
struct Avm1Function
{
    std::string name;
    std::vector<std::pair<std::string, unsigned>> parameters;
    unsigned flags{};
    ByteView body;
};

inline Avm1Function avm1_function(Reader& args, Reader& stream, bool extended)
{
    Avm1Function result;
    result.name = args.cstring();
    const auto count = args.u16();
    if (extended)
    {
        args.skip(1);
        result.flags = args.u16();
    }
    if (count > args.remaining() / (extended ? 2 : 1))
        throw std::runtime_error("Paramètres de fonction AVM1 tronqués.");
    result.parameters.reserve(count);
    for (unsigned i = 0; i < count; ++i)
    {
        const auto reg = extended ? args.u8() : 0u;
        result.parameters.emplace_back(args.cstring(), reg);
    }
    const auto size = args.u16();
    result.body = args.remaining() >= size ? args.take(size) : stream.take(size);
    return result;
}

struct Avm1Action
{
    std::size_t offset{}, next{};
    unsigned code{};
    ByteView args, nested;
    std::optional<Avm1Function> function;
};

inline std::vector<Avm1Action> avm1_actions(ByteView bytes)
{
    Reader stream(bytes);
    std::vector<Avm1Action> result;
    while (stream.remaining())
    {
        if (result.size() >= 65536)
            throw std::runtime_error("Trop d’instructions dans un bloc AVM1.");
        Avm1Action action;
        action.offset = stream.position();
        action.code = stream.u8();
        if (action.code >= 0x80)
            action.args = stream.take(stream.u16());
        Reader args(action.args);
        if (action.code == 0x8E || action.code == 0x9B)
        {
            action.function = avm1_function(args, stream, action.code == 0x8E);
            action.nested = action.function->body;
        }
        else if (action.code == 0x94)
        {
            const auto size = args.u16();
            action.nested = args.remaining() >= size ? args.take(size) : stream.take(size);
        }
        action.next = stream.position();
        result.push_back(std::move(action));
        if (!result.back().code)
            break;
    }
    return result;
}

inline std::int64_t avm1_branch(const Avm1Action& action)
{
    return static_cast<std::int64_t>(action.next) + std::bit_cast<std::int16_t>(Reader(action.args).u16());
}
}
