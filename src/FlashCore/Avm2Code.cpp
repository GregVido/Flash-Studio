#include "Avm2Code.hpp"

#include <charconv>
#include <format>
#include <limits>
#include <map>
#include <set>
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

FlowChanges simplify_flow(std::vector<Instruction>& instructions, std::size_t end, std::span<const ExceptionEntry> exceptions)
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
    std::vector<bool> boundaries(count);
    for (const auto& exception : exceptions)
    {
        for (const auto boundary : {exception.from, exception.to, exception.target})
            if (const auto index = lookup(boundary); index < count)
                boundaries[index] = true;
        if (const auto target = lookup(exception.target); target < count)
            incoming[target] = true;
    }
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
                const auto destination = address(next);
                // Threading a forward jump must not create an unmarked back edge.
                if (old != next && (destination > instruction.offset ||
                    (next < count && instructions[next].op == 0x09)))
                {
                    target = static_cast<std::int64_t>(destination);
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
    for (const auto& exception : exceptions)
        discover(lookup(exception.target));
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
        if (live[i] || boundaries[i])
        {
            if (!live[i])
            {
                instructions[i].op = 0x02;
                instructions[i].args.clear();
            }
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
constexpr unsigned nop = 0x02, pop = 0x29, dup = 0x2A, swap = 0x2B;
constexpr unsigned jump = 0x10, if_true = 0x11, if_false = 0x12;
constexpr unsigned push_true = 0x26, push_false = 0x27;

bool discarded_closure(std::span<const Instruction> code, std::size_t offset, std::size_t end)
{
    const auto& branch = code[offset + 2];
    return code[offset].op == 0x40 && code[offset + 1].op == pop && branch.op == jump &&
        branch.args[0] > static_cast<std::int64_t>(branch.offset) + 4 &&
        branch.args[0] < static_cast<std::int64_t>(end);
}

bool bool_value(unsigned op) { return op == push_true || op == push_false; }
bool padding(unsigned op) { return op == nop || op == 0x09 || (op >= 0xEF && op <= 0xF3); }

void erase_instruction(Instruction& instruction)
{
    instruction.op = nop;
    instruction.args.clear();
}

std::optional<unsigned> register_read(const Instruction& instruction)
{
    if (instruction.op == 0x62)
        return static_cast<unsigned>(instruction.args[0]);
    if (instruction.op >= 0xD0 && instruction.op <= 0xD3)
        return instruction.op & 3;
    return {};
}

std::optional<unsigned> register_store(const Instruction& instruction)
{
    if (instruction.op == 0x63)
        return static_cast<unsigned>(instruction.args[0]);
    if (instruction.op >= 0xD4 && instruction.op <= 0xD7)
        return instruction.op & 3;
    return {};
}

std::set<std::int64_t> incoming_offsets(std::span<const Instruction> code, std::span<const ExceptionEntry> exceptions)
{
    std::set<std::int64_t> targets;
    for (const auto& instruction : code)
        if (instruction.op >= 0x0C && instruction.op <= 0x1B)
            targets.insert(instruction.args.begin(), instruction.args.end());
    for (const auto& exception : exceptions)
        targets.insert(exception.target);
    return targets;
}

std::size_t remove_discarded_closures(std::vector<Instruction>& code, std::size_t end,
    std::span<const ExceptionEntry> exceptions)
{
    if (std::ranges::none_of(code, [](const Instruction& item) { return item.op == 0x40; }))
        return 0;
    const auto incoming = incoming_offsets(code, exceptions);
    std::size_t removed = 0;
    for (std::size_t i = 0; i + 2 < code.size(); ++i)
        if (discarded_closure(code, i, end) && !incoming.contains(static_cast<std::int64_t>(code[i + 1].offset)))
        {
            erase_instruction(code[i]);
            erase_instruction(code[i + 1]);
            removed += 2;
        }
    return removed;
}

struct ConstantStore { unsigned reg, value; };

struct Prelude
{
    std::vector<ConstantStore> stores;
    std::size_t end{};
};

Prelude boolean_stores(std::span<const Instruction> code, const std::set<std::int64_t>& incoming,
    unsigned first_local, unsigned locals)
{
    Prelude prefix;
    std::vector<ConstantStore> stores;
    std::vector<unsigned> stack;
    for (std::size_t i = 0; i < code.size() && i < 96; ++i)
    {
        const auto& item = code[i];
        if (i && incoming.contains(static_cast<std::int64_t>(item.offset)))
            break;
        if (bool_value(item.op))
            stack.push_back(item.op);
        else if (item.op == swap && stack.size() >= 2)
            std::iter_swap(stack.end() - 1, stack.end() - 2);
        else if (item.op == dup && !stack.empty())
            stack.push_back(stack.back());
        else if (item.op == pop && !stack.empty())
            stack.pop_back();
        else if (const auto local = register_store(item); local && *local >= first_local && *local < locals && !stack.empty())
        {
            stores.push_back({*local, stack.back()});
            stack.pop_back();
        }
        else if (!padding(item.op))
            break;
        if (stack.empty())
        {
            prefix.end = i + 1;
            prefix.stores = stores;
        }
    }
    return prefix;
}

std::size_t flatten_adjacent_jumps(std::vector<Instruction>& code)
{
    std::size_t removed = 0;
    for (std::size_t i = 1; i < code.size(); ++i)
        if (code[i - 1].op == jump && code[i - 1].args[0] == static_cast<std::int64_t>(code[i].offset))
        {
            erase_instruction(code[i - 1]);
            ++removed;
        }
    return removed;
}

std::size_t substitute_locals(std::vector<Instruction>& code, std::span<const ExceptionEntry> exceptions,
    unsigned first_local, unsigned locals)
{
    const auto prefix = boolean_stores(code, incoming_offsets(code, exceptions), first_local, locals);
    if (prefix.stores.empty())
        return 0;
    std::map<unsigned, unsigned> known;
    std::set<unsigned> ambiguous;
    for (const auto& store : prefix.stores)
        if (!known.try_emplace(store.reg, store.value).second)
            ambiguous.insert(store.reg);
    for (std::size_t i = prefix.end; i < code.size(); ++i)
    {
        const auto& item = code[i];
        if (const auto local = register_store(item))
            ambiguous.insert(*local);
        if (item.op == 0x08 || item.op == 0x32 || item.op == 0x92 || item.op == 0x94 || item.op == 0xC2 || item.op == 0xC3)
            for (const auto local : item.args)
                ambiguous.insert(static_cast<unsigned>(local));
    }
    for (const auto local : ambiguous)
        known.erase(local);
    if (known.empty())
        return 0;
    std::size_t substituted = 0;
    for (std::size_t i = prefix.end; i < code.size(); ++i)
        if (const auto local = register_read(code[i]))
            if (const auto found = known.find(*local); found != known.end())
            {
                code[i].op = found->second;
                code[i].args.clear();
                ++substituted;
            }
    if (known.size() == prefix.stores.size())
    {
        for (std::size_t i = 0; i < prefix.end; ++i)
            if (!padding(code[i].op))
                erase_instruction(code[i]);
        return substituted + prefix.stores.size();
    }
    return substituted;
}

FlowChanges fold_boolean_sequences(std::vector<Instruction>& code, std::span<const ExceptionEntry> exceptions)
{
    const auto incoming = incoming_offsets(code, exceptions);
    FlowChanges changes;
    std::optional<std::size_t> value, duplicate;
    for (std::size_t i = 0; i < code.size(); ++i)
    {
        auto& item = code[i];
        if (incoming.contains(static_cast<std::int64_t>(item.offset)))
        {
            value.reset();
            duplicate.reset();
        }
        if (padding(item.op))
            continue;
        if (bool_value(item.op))
        {
            value = i;
            duplicate.reset();
            continue;
        }
        if (value && !duplicate && item.op == dup)
        {
            duplicate = i;
            continue;
        }
        if (value && !duplicate && (item.op == 0x76 || item.op == 0x81 || item.op == 0x96))
        {
            if (item.op == 0x96)
                code[*value].op = code[*value].op == push_true ? push_false : push_true;
            erase_instruction(item);
            ++changes.constants;
            continue;
        }
        if (value && (item.op == if_true || item.op == if_false || item.op == pop))
        {
            const bool taken = item.op != pop && ((code[*value].op == push_true) == (item.op == if_true));
            erase_instruction(code[duplicate.value_or(*value)]);
            if (item.op != pop)
                ++changes.constant_branches;
            if (taken)
                item.op = jump;
            else
            {
                erase_instruction(item);
                if (duplicate)
                {
                    duplicate.reset();
                    continue;
                }
            }
        }
        value.reset();
        duplicate.reset();
    }
    return changes;
}

std::size_t linearize_single_path(std::vector<Instruction>& code)
{
    // A method with only unconditional edges can be emitted in execution order.
    // Keep the original layout if any branch choice, loop or fallthrough remains.
    std::vector<bool> visited(code.size());
    std::vector<Instruction> ordered;
    for (std::size_t i = 0; i < code.size() && !visited[i];)
    {
        visited[i] = true;
        const auto& item = code[i];
        if (item.op == jump)
        {
            const auto offset = static_cast<std::size_t>(item.args[0]);
            const auto target = std::ranges::lower_bound(code, offset, {}, &Instruction::offset);
            if (target == code.end() || target->offset != offset)
                return 0;
            i = static_cast<std::size_t>(target - code.begin());
            continue;
        }
        if (item.op >= 0x0C && item.op <= 0x1B)
            return 0;
        if (item.op != nop && item.op != 0x09)
            ordered.push_back({ordered.size(), item.op, item.args});
        if (item.op == 0x03 || item.op == 0x47 || item.op == 0x48)
        {
            const auto removed = code.size() - ordered.size();
            if (removed)
                code = std::move(ordered);
            return removed;
        }
        ++i;
    }
    return 0;
}

bool order_acyclic_fragments(std::vector<Instruction>& code)
{
    struct Fragment
    {
        std::size_t first, last;
        std::vector<std::size_t> successors;
        std::size_t predecessors{};
    };
    std::vector<Fragment> fragments;
    std::vector<std::size_t> owner(code.size());
    std::size_t first = 0;
    for (std::size_t i = 0; i < code.size(); ++i)
    {
        owner[i] = fragments.size();
        const auto op = code[i].op;
        if (op == jump || op == 0x1B || op == 0x03 || op == 0x47 || op == 0x48)
        {
            fragments.push_back({first, i + 1, {}});
            first = i + 1;
        }
    }
    if (fragments.size() < 2 || first != code.size())
        return false;

    const auto destination = [&](std::int64_t target)
    {
        const auto offset = static_cast<std::size_t>(target);
        const auto item = std::ranges::lower_bound(code, offset, {}, &Instruction::offset);
        return item != code.end() && item->offset == offset ? static_cast<std::size_t>(item - code.begin()) : code.size();
    };
    bool scrambled = false;
    for (std::size_t i = 0; i < code.size(); ++i)
        if (code[i].op >= 0x0C && code[i].op <= 0x1B)
            for (const auto address : code[i].args)
            {
                const auto target = destination(address);
                if (target == code.size() || (owner[target] == owner[i] && target <= i))
                    return false;
                if (owner[target] == owner[i])
                    continue;
                scrambled |= owner[target] < owner[i];
                fragments[owner[i]].successors.push_back(owner[target]);
                ++fragments[owner[target]].predecessors;
            }
    if (!scrambled || fragments[0].predecessors)
        return false;

    std::set<std::size_t> available;
    for (std::size_t i = 0; i < fragments.size(); ++i)
        if (!fragments[i].predecessors)
            available.insert(i);
    std::vector<std::size_t> order;
    while (!available.empty())
    {
        const auto index = *available.begin();
        available.erase(available.begin());
        order.push_back(index);
        for (const auto next : fragments[index].successors)
            if (!--fragments[next].predecessors)
                available.insert(next);
    }
    // Moving a fragment preserves all implicit edges within it. Leave cyclic layouts intact.
    if (order.size() != fragments.size())
        return false;
    std::vector<std::size_t> remapped(code.size());
    std::vector<Instruction> ordered;
    ordered.reserve(code.size());
    for (const auto index : order)
        for (auto i = fragments[index].first; i < fragments[index].last; ++i)
        {
            remapped[i] = ordered.size();
            ordered.push_back({ordered.size(), code[i].op, code[i].args});
        }
    for (auto& item : ordered)
        if (item.op >= 0x0C && item.op <= 0x1B)
            for (auto& target : item.args)
                target = static_cast<std::int64_t>(remapped[destination(target)]);
    code = std::move(ordered);
    return true;
}
}

bool has_boolean_prelude(ByteView bytes, unsigned first_local, unsigned locals)
{
    try
    {
        Reader input(bytes);
        std::vector<Instruction> prefix;
        while (input.remaining() && prefix.size() < 8)
        {
            auto item = decode(input);
            if (item.op == jump && item.args[0] >= static_cast<std::int64_t>(input.position()) + 4 &&
                item.args[0] < static_cast<std::int64_t>(bytes.size()))
            {
                input.skip(static_cast<std::size_t>(item.args[0]) - input.position());
                for (unsigned i = 0; i < 2 && input.remaining(); ++i)
                    prefix.push_back(decode(input));
                const auto stores = boolean_stores(prefix, {}, first_local, locals);
                return stores.stores.size() == 2 && stores.stores[0].reg != stores.stores[1].reg &&
                    stores.stores[0].value != stores.stores[1].value;
            }
            if (!bool_value(item.op) && item.op != swap && !register_store(item) && !padding(item.op))
                return false;
            prefix.push_back(std::move(item));
        }
    }
    catch (const std::runtime_error&) {}
    return false;
}

bool has_discarded_closure_prelude(ByteView bytes)
{
    try
    {
        Reader input(bytes);
        std::array<Instruction, 3> window;
        for (unsigned i = 0; i < 12 && input.remaining(); ++i)
        {
            auto item = decode(input);
            const auto op = item.op;
            if (i == 0 && op != 0xD0)
                return false;
            window[0] = std::move(window[1]);
            window[1] = std::move(window[2]);
            window[2] = std::move(item);
            if (i >= 2 && discarded_closure(window, 0, bytes.size()))
                return true;
            if ((op >= 0x0C && op <= 0x1B) || op == 0x47 || op == 0x48 || op == 0x03)
                break;
        }
    }
    catch (const std::runtime_error&) {}
    return false;
}

FlowChanges deobfuscate_flow(std::vector<Instruction>& code, std::size_t end,
    std::span<const ExceptionEntry> exceptions, unsigned locals_begin, unsigned local_count)
{
    FlowChanges total;
    total.removed = remove_discarded_closures(code, end, exceptions);
    for (unsigned pass = 0; pass < 32; ++pass)
    {
        const auto cleaned = simplify_flow(code, end, exceptions);
        const auto jumps = flatten_adjacent_jumps(code);
        const auto locals = substitute_locals(code, exceptions, locals_begin, local_count);
        const auto folded = fold_boolean_sequences(code, exceptions);
        total.removed += cleaned.removed;
        total.redirected += cleaned.redirected + jumps;
        total.constant_branches += cleaned.constant_branches + folded.constant_branches;
        total.constants += locals + folded.constants;
        if (!cleaned.removed && !cleaned.redirected && !cleaned.constant_branches && !jumps && !locals &&
            !folded.constant_branches && !folded.constants)
            break;
    }
    if (exceptions.empty() && total.constants)
    {
        const auto linear = linearize_single_path(code);
        total.redirected += linear;
        if (!linear && order_acyclic_fragments(code))
            total.redirected += 1 + flatten_adjacent_jumps(code);
    }
    if (total.removed || total.redirected || total.constant_branches || total.constants)
    {
        auto keep = incoming_offsets(code, exceptions);
        for (const auto& exception : exceptions)
        {
            keep.insert(exception.from);
            keep.insert(exception.to);
        }
        std::erase_if(code, [&](const Instruction& item)
        {
            return item.op == nop && !keep.contains(static_cast<std::int64_t>(item.offset));
        });
    }
    return total;
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
