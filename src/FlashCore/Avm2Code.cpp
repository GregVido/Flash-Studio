#include "Avm2Code.hpp"

#include <charconv>
#include <format>
#include <limits>
#include <unordered_set>

namespace flash::script
{
Instruction decode(Reader& code)
{
    Instruction item;
    item.offset = code.position();
    item.op = code.u8();
    const auto op = item.op;
    if (opcodes[op].name.empty())
        throw std::runtime_error(std::format("Opcode AVM2 0x{:02X} inconnu à 0x{:X}.", op, item.offset));
    if (opcodes[op].unknown_operands)
        throw std::runtime_error(std::format("Opcode AVM2 {} (0x{:02X}) : opérandes non documentés à 0x{:X}.",
            opcodes[op].name, op, item.offset));
    if (op >= 0x0C && op <= 0x1A)
    {
        const auto delta = code.s24();
        item.args.push_back(static_cast<std::int64_t>(code.position()) + delta);
    }
    else if (op == 0x1B)
    {
        const auto base = static_cast<std::int64_t>(item.offset);
        item.args.push_back(base + code.s24());
        const auto count = code.u30();
        if (count > 4096 || code.remaining() / 3 < count + 1)
            throw std::runtime_error("Table lookupswitch invalide.");
        for (unsigned i = 0; i <= count; ++i)
            item.args.push_back(base + code.s24());
    }
    else if (op == 0x24 || op == 0x65)
    {
        const auto byte = code.u8();
        item.args.push_back(op == 0x24 ? std::bit_cast<std::int8_t>(byte) : int{byte});
    }
    else if (op == 0xEF)
    {
        item.args.push_back(code.u8());
        item.args.push_back(code.u30());
        item.args.push_back(code.u8());
        item.args.push_back(code.u30());
    }
    else if (op == 0x25)
        item.args.push_back(code.v32());
    else
        for (unsigned i = 0; i < opcodes[op].count; ++i)
            item.args.push_back(code.u30());
    return item;
}

FlowChanges simplify_flow(std::vector<Instruction>& instructions, std::size_t end)
{
    const auto count = instructions.size();
    const auto invalid = count + 1;
    std::vector<std::size_t> redirect(count + 1, invalid);
    std::vector<bool> visiting(count), live(count);
    const auto address = [&](std::size_t index) { return index == count ? end : instructions[index].offset; };
    const auto lookup = [&](std::int64_t target)
    {
        if (target == static_cast<std::int64_t>(end))
            return count;
        if (target < 0)
            throw std::runtime_error("Saut négatif dans le graphe de contrôle.");
        const auto offset = static_cast<std::size_t>(target);
        const auto found = std::ranges::lower_bound(instructions, offset, {}, &Instruction::offset);
        if (found == instructions.end() || found->offset != offset)
            throw std::runtime_error("Le nettoyage exige des cibles alignées sur les instructions.");
        return static_cast<std::size_t>(found - instructions.begin());
    };
    std::vector<std::size_t> chain;
    const auto chase_jump = [&](std::size_t index)
    {
        chain.clear();
        while (index < count && instructions[index].op == 0x10 && redirect[index] == invalid && !visiting[index])
        {
            visiting[index] = true;
            chain.push_back(index);
            index = lookup(instructions[index].args.front());
        }
        const auto cyclic = index < count && visiting[index];
        const auto final = redirect[index] == invalid ? index : redirect[index];
        for (const auto step : chain)
        {
            visiting[step] = false;
            redirect[step] = cyclic ? step : final;
        }
        return final;
    };

    FlowChanges changes;
    std::vector<bool> incoming(count);
    for (const auto& instruction : instructions)
        if (instruction.op >= 0x0C && instruction.op <= 0x1B)
            for (const auto target : instruction.args)
                if (const auto index = lookup(target); index < count)
                    incoming[index] = true;
    for (std::size_t i = 1; i < count; ++i)
    {
        auto& value = instructions[i - 1];
        auto& branch = instructions[i];
        if (incoming[i] || (branch.op != 0x11 && branch.op != 0x12) || (value.op != 0x26 && value.op != 0x27))
            continue;
        const auto taken = (value.op == 0x26) == (branch.op == 0x11);
        value.op = 0x02;
        branch.op = taken ? 0x10 : 0x02;
        if (!taken)
            branch.args.clear();
        ++changes.constant_branches;
    }
    for (auto& instruction : instructions)
        if (instruction.op >= 0x0C && instruction.op <= 0x1B)
            for (auto& target : instruction.args)
            {
                const auto old = lookup(target);
                chase_jump(old);
                const auto next = redirect[old] == invalid ? old : redirect[old];
                if (old != next)
                {
                    target = static_cast<std::int64_t>(address(next));
                    ++changes.redirected;
                }
            }

    std::vector<std::size_t> work;
    const auto discover = [&](std::size_t index)
    {
        if (index < count && !live[index])
        {
            live[index] = true;
            work.push_back(index);
        }
    };
    discover(0);
    while (!work.empty())
    {
        const auto index = work.back();
        work.pop_back();
        const auto& instruction = instructions[index];
        const auto op = instruction.op;
        if (op >= 0x0C && op <= 0x1B)
            for (const auto target : instruction.args)
                discover(lookup(target));
        if (op != 0x03 && op != 0x10 && op != 0x1B && op != 0x47 && op != 0x48)
            discover(index + 1);
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (live[i])
        {
            if (kept != i)
                instructions[kept] = std::move(instructions[i]);
            ++kept;
        }
    instructions.resize(kept);
    changes.removed = count - kept;
    return changes;
}

namespace
{
void put_v32(Bytes& bytes, std::uint32_t value)
{
    while (value >= 128)
    {
        bytes.push_back(static_cast<std::uint8_t>((value & 127) | 128));
        value >>= 7;
    }
    bytes.push_back(static_cast<std::uint8_t>(value));
}
}

void put_u30(Bytes& bytes, unsigned value)
{
    if (value >= (1u << 30))
        throw std::runtime_error("Valeur u30 hors limites.");
    put_v32(bytes, value);
}

namespace
{
std::string_view trim(std::string_view line)
{
    constexpr std::string_view whitespace = " \t\r\n";
    const auto begin = line.find_first_not_of(whitespace);
    if (begin == std::string_view::npos)
        return {};
    return line.substr(begin, line.find_last_not_of(whitespace) - begin + 1);
}

std::vector<std::string_view> words(std::string_view text)
{
    constexpr std::string_view separators = " \t\r,";
    std::vector<std::string_view> result;
    for (auto pos = text.find_first_not_of(separators); pos != std::string_view::npos;)
    {
        const auto end = text.find_first_of(separators, pos);
        result.push_back(text.substr(pos, end == std::string_view::npos ? end : end - pos));
        pos = end == std::string_view::npos ? end : text.find_first_not_of(separators, end);
    }
    return result;
}

bool label_name(std::string_view text)
{
    const auto is_letter = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    return !text.empty() && text.size() <= 128 && is_letter(text.front()) &&
        std::ranges::all_of(text, [&](char c) { return is_letter(c) || (c >= '0' && c <= '9'); });
}

std::int64_t number(std::string_view text, std::int64_t minimum = 0, std::int64_t maximum = (1u << 30) - 1)
{
    const bool negative = text.starts_with('-');
    if (negative)
        text.remove_prefix(1);
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.remove_prefix(2);
        base = 16;
    }
    std::uint32_t value{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value, base);
    const auto signed_value = negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || signed_value < minimum || signed_value > maximum)
        throw std::runtime_error("Opérande numérique invalide.");
    return signed_value;
}

void expected(std::span<const std::string_view> operands, std::size_t count)
{
    if (operands.size() != count)
        throw std::runtime_error(std::format("{} opérandes attendu(s).", count));
}

struct Fixup
{
    std::string_view label;
    std::size_t offset, relative_to, line;
};

struct PendingException
{
    std::string_view from, to, target;
    unsigned type, variable;
    std::size_t line;
};

struct Anchor
{
    std::string_view label, instruction;
    unsigned displacement;
    std::size_t line;
};

[[noreturn]] void line_error(std::size_t line, std::string_view message)
{
    throw std::runtime_error(std::format("Ligne {} : {}", line, message));
}

void branch(Bytes& code, std::vector<Fixup>& fixups, std::string_view target,
    std::size_t relative_to, std::size_t line)
{
    if (!label_name(target))
        throw std::runtime_error("Étiquette de destination invalide.");
    fixups.push_back({target, code.size(), relative_to, line});
    code.insert(code.end(), 3, 0);
}
}

AssembledMethod assemble_method(std::string_view text)
{
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    if (text.size() > max_text_size)
        throw std::runtime_error("Le P-code d’une méthode dépasse 2 Mio.");
    static const auto by_name = []
    {
        std::unordered_map<std::string_view, unsigned> table;
        for (const auto& opcode : opcodes)
            if (!opcode.name.empty() && !opcode.unknown_operands)
                table.emplace(opcode.name, opcode.value);
        return table;
    }();

    AssembledMethod method;
    std::unordered_map<std::string_view, unsigned> labels;
    std::vector<Fixup> fixups;
    std::vector<PendingException> handlers;
    std::vector<Anchor> anchors;
    std::vector<unsigned> addresses;
    unsigned directives = 0;
    std::size_t line = 0;
    while (!text.empty())
    {
        ++line;
        const auto end = text.find_first_of("\r\n");
        auto content = text.substr(0, end);
        if (end == std::string_view::npos)
            text = {};
        else
        {
            const auto newline = text.substr(end).starts_with("\r\n") ? 2 : 1;
            text.remove_prefix(end + newline);
        }
        content = trim(content.substr(0, content.find(';')));
        if (content.empty())
            continue;
        try
        {
            if (const auto colon = content.find(':'); colon != std::string_view::npos)
            {
                const auto label = trim(content.substr(0, colon));
                if (!label_name(label) || !labels.emplace(label, static_cast<unsigned>(method.code.size())).second)
                    throw std::runtime_error("Étiquette invalide ou déjà définie.");
                content = trim(content.substr(colon + 1));
            }
            const auto tokens = words(content);
            if (tokens.empty())
                continue;
            const auto command = tokens.front();
            const auto args = std::span(tokens).subspan(1);
            const auto u30 = [&](std::size_t i) { return static_cast<unsigned>(number(args[i])); };
            if (command.starts_with('.'))
            {
                if (command == ".exception")
                {
                    expected(args, 5);
                    handlers.push_back({args[0], args[1], args[2], u30(3), u30(4), line});
                }
                else if (command == ".anchor")
                {
                    expected(args, 3);
                    anchors.push_back({args[0], args[1], u30(2), line});
                }
                else
                {
                    const auto bit = command == ".stack" ? 1u : command == ".locals" ? 2u : command == ".scope" ? 4u : 0u;
                    if (!bit || (directives & bit))
                        throw std::runtime_error("Directive inconnue ou déjà définie.");
                    directives |= bit;
                    expected(args, bit == 4 ? 2 : 1);
                    if (bit == 1)
                        method.stack = u30(0);
                    else if (bit == 2)
                        method.locals = u30(0);
                    else
                    {
                        method.initial_scope = u30(0);
                        method.max_scope = u30(1);
                    }
                }
                continue;
            }
            const auto found = by_name.find(command);
            if (found == by_name.end())
                throw std::runtime_error(std::format("Instruction AVM2 inconnue : {}.", command));
            const auto op = found->second;
            const auto address = method.code.size();
            addresses.push_back(static_cast<unsigned>(address));
            method.code.push_back(static_cast<std::uint8_t>(op));
            if (op >= 0x0C && op <= 0x1A)
            {
                expected(args, 1);
                branch(method.code, fixups, args[0], address + 4, line);
            }
            else if (op == 0x1B)
            {
                if (args.size() < 2 || args.size() > 4098)
                    throw std::runtime_error("lookupswitch attend une destination par défaut et au moins un cas.");
                branch(method.code, fixups, args[0], address, line);
                put_u30(method.code, static_cast<unsigned>(args.size() - 2));
                for (const auto target : args.subspan(1))
                    branch(method.code, fixups, target, address, line);
            }
            else
            {
                expected(args, opcodes[op].count);
                for (std::size_t i = 0; i < args.size(); ++i)
                {
                    if (op == 0x24)
                        method.code.push_back(static_cast<std::uint8_t>(number(args[i], -128, 127)));
                    else if (op == 0x25)
                        put_v32(method.code, static_cast<std::uint32_t>(number(args[i], -32768, UINT32_MAX)));
                    else if (op == 0x65 || (op == 0xEF && (i == 0 || i == 2)))
                        method.code.push_back(static_cast<std::uint8_t>(number(args[i], 0, 255)));
                    else
                        put_u30(method.code, u30(i));
                }
            }
        }
        catch (const std::runtime_error& error)
        {
            line_error(line, error.what());
        }
    }
    if (directives != 7 || method.code.empty() || method.max_scope < method.initial_scope)
        throw std::runtime_error("Un corps non vide et les directives .stack, .locals et .scope sont nécessaires.");

    for (const auto& anchor : anchors)
    {
        const auto instruction = labels.find(anchor.instruction);
        if (instruction == labels.end() || !std::ranges::binary_search(addresses, instruction->second))
            line_error(anchor.line, "Instruction d’ancrage introuvable.");
        const auto address = instruction->second;
        const auto following = std::ranges::upper_bound(addresses, address);
        const auto end = following == addresses.end() ? method.code.size() : *following;
        if (!anchor.displacement || anchor.displacement >= end - address ||
            !label_name(anchor.label) || !labels.emplace(anchor.label, address + anchor.displacement).second)
            line_error(anchor.line, "L’ancre doit désigner l’intérieur d’une instruction et porter un nom unique.");
    }
    const auto target = [&](std::string_view name, std::size_t source_line, bool allow_end = false)
    {
        const auto item = labels.find(name);
        if (item == labels.end() || (!allow_end && item->second == method.code.size()))
            line_error(source_line, std::format("Étiquette sans instruction valide : {}.", name));
        return item->second;
    };
    for (const auto& fixup : fixups)
    {
        const auto delta = static_cast<std::int64_t>(target(fixup.label, fixup.line, true)) -
            static_cast<std::int64_t>(fixup.relative_to);
        if (delta < -8388608 || delta > 8388607)
            line_error(fixup.line, "Saut hors limites.");
        for (unsigned i = 0; i < 3; ++i)
            method.code[fixup.offset + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(delta) >> (8 * i));
    }
    for (const auto& handler : handlers)
    {
        const auto from = target(handler.from, handler.line);
        const auto to = target(handler.to, handler.line, true);
        if (from >= to)
            line_error(handler.line, "Plage d’exception vide ou inversée.");
        method.exceptions.push_back({from, to, target(handler.target, handler.line), handler.type, handler.variable});
    }
    return method;
}
}
