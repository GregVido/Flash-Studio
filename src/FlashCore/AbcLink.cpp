#include "AbcLink.hpp"
#include "Avm2Code.hpp"

#include <map>
#include <utility>

namespace flash::script
{
namespace
{
enum Table { ints, uints, doubles, strings, namespaces, ns_sets, names, methods, metadata, instances, classes, scripts, bodies, table_count };

bool pool_table(Table table) { return table <= names; }

using Tables = std::array<std::vector<Bytes>, table_count>;

struct Indices
{
    std::array<unsigned, table_count> base{}, limits{};
    std::vector<unsigned> method_indices;
    unsigned class_target{};
    bool relocate = false;

    unsigned ref(Table table, unsigned value) const
    {
        if (pool_table(table) && !value)
            return 0;
        const auto index = pool_table(table) ? value - 1 : value;
        if (index >= limits[table])
            throw std::runtime_error("Invalid reference in compiled ABC.");
        if (table == methods)
            return method_indices[value];
        return table == classes ? class_target + value : base[table] + value;
    }
};

Table constant_table(unsigned kind)
{
    switch (kind)
    {
    case 0x01: return strings;
    case 0x03: return ints;
    case 0x04: return uints;
    case 0x06: return doubles;
    case 0x05: case 0x08: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: return namespaces;
    default: return table_count;
    }
}

struct RelocatedCode
{
    Bytes bytes;
    std::map<std::size_t, std::size_t> offsets;

    unsigned offset(std::int64_t original) const
    {
        const auto match = original < 0 ? offsets.end() : offsets.find(static_cast<std::size_t>(original));
        if (match == offsets.end())
            throw std::runtime_error("Invalid branch or exception boundary in compiled ABC.");
        return static_cast<unsigned>(match->second);
    }
};

RelocatedCode relocate_code(ByteView bytes, const Indices& indices)
{
    struct Branch { std::size_t field, base; std::int64_t target; };
    RelocatedCode code;
    std::vector<Branch> branches;
    const auto patch = [&](std::size_t base, std::int64_t target)
    {
        branches.push_back({code.bytes.size(), base, target});
        code.bytes.insert(code.bytes.end(), 3, 0);
    };
    Reader input(bytes);
    while (input.remaining())
    {
        const auto start = input.position();
        const auto item = decode(input);
        const auto base = code.bytes.size();
        code.offsets.emplace(start, base);
        auto table = table_count;
        switch (item.op)
        {
        case 0x04: case 0x05: case 0x45: case 0x46: case 0x4A: case 0x4C: case 0x4E: case 0x4F:
        case 0x59: case 0x5B: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x66:
        case 0x68: case 0x6A: case 0x80: case 0x86: case 0xB2: table = names; break;
        case 0x06: case 0x2C: case 0xF1: table = strings; break;
        case 0x2D: table = ints; break;
        case 0x2E: table = uints; break;
        case 0x2F: table = doubles; break;
        case 0x31: table = namespaces; break;
        case 0x40: case 0x44: table = methods; break;
        case 0x58: table = classes; break;
        case 0x22: case 0x54: throw std::runtime_error("Floating-point pool opcode in ABC 46.16.");
        }
        if (table != table_count)
        {
            code.bytes.push_back(static_cast<std::uint8_t>(item.op));
            put_u30(code.bytes, indices.ref(table, static_cast<unsigned>(item.args[0])));
            for (std::size_t i = 1; i < item.args.size(); ++i)
                put_u30(code.bytes, static_cast<unsigned>(item.args[i]));
        }
        else if (item.op >= 0x0C && item.op <= 0x1B)
        {
            code.bytes.push_back(static_cast<std::uint8_t>(item.op));
            patch(item.op == 0x1B ? base : base + 4, item.args[0]);
            if (item.op == 0x1B)
            {
                put_u30(code.bytes, static_cast<unsigned>(item.args.size() - 2));
                for (std::size_t i = 1; i < item.args.size(); ++i)
                    patch(base, item.args[i]);
            }
        }
        else if (item.op == 0xEF)
        {
            append(code.bytes, bytes.subspan(start, 2));
            put_u30(code.bytes, indices.ref(strings, static_cast<unsigned>(item.args[1])));
            code.bytes.push_back(static_cast<std::uint8_t>(item.args[2]));
            put_u30(code.bytes, static_cast<unsigned>(item.args[3]));
        }
        else
            append(code.bytes, bytes.subspan(start, input.position() - start));
    }
    code.offsets.emplace(bytes.size(), code.bytes.size());
    for (const auto& branch : branches)
    {
        const auto delta = static_cast<std::int64_t>(code.offset(branch.target)) - static_cast<std::int64_t>(branch.base);
        if (delta < -0x800000 || delta > 0x7FFFFF)
            throw std::runtime_error("Compiled branch exceeds the AVM2 offset range.");
        for (unsigned i = 0; i < 3; ++i)
            code.bytes[branch.field + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(delta) >> (8 * i));
    }
    return code;
}

unsigned count(Reader& input)
{
    const auto value = input.u30();
    if (value > 100'000 || value > input.remaining() + 1)
        throw std::runtime_error("Invalid ABC table size.");
    return value;
}

struct Record
{
    Reader& input;
    const Indices& indices;
    Bytes output;
    std::optional<std::pair<unsigned, unsigned>> hidden;

    void emit(unsigned value) { put_u30(output, value); }
    unsigned byte() { const auto value = input.u8(); output.push_back(value); return value; }
    unsigned integer() { const auto value = input.u30(); emit(value); return value; }
    unsigned reference(Table table)
    {
        const auto value = input.u30();
        emit(indices.relocate ? indices.ref(table, value) : value);
        return value;
    }
    unsigned length() { const auto value = count(input); emit(value); return value; }
    void raw(std::size_t size) { append(output, input.take(size)); }

    void constant(bool optional)
    {
        const auto value = input.u30();
        if (!optional && !value)
        {
            emit(0);
            return;
        }
        const auto kind = input.u8();
        const auto table = constant_table(kind);
        emit(indices.relocate && table != table_count ? indices.ref(table, value) : value);
        output.push_back(kind);
    }

    void traits()
    {
        for (unsigned i = length(); i; --i)
        {
            const auto named = input.u30(), flags = static_cast<unsigned>(input.u8()), slot = input.u30();
            const auto kind = flags & 15;
            const auto type = kind == 4 ? std::optional{input.u30()} : std::nullopt;
            emit(type && hidden && *type == hidden->first ? hidden->second :
                indices.relocate ? indices.ref(names, named) : named);
            output.push_back(static_cast<std::uint8_t>(flags));
            emit(slot);
            switch (kind)
            {
            case 0: case 6: reference(names); constant(false); break;
            case 1: case 2: case 3: case 5: reference(methods); break;
            case 4: emit(indices.relocate ? indices.ref(classes, *type) : *type); break;
            default: throw std::runtime_error("Unknown ABC trait kind.");
            }
            if (flags & 0x40)
                for (unsigned j = length(); j; --j)
                    reference(metadata);
        }
    }

    void entry(Table table)
    {
        switch (table)
        {
        case ints: case uints:
        {
            auto value = input.v32();
            while (value > 127)
            {
                output.push_back(static_cast<std::uint8_t>((value & 127) | 128));
                value >>= 7;
            }
            output.push_back(static_cast<std::uint8_t>(value));
            break;
        }
        case doubles: raw(8); break;
        case strings: raw(integer()); break;
        case namespaces: byte(); reference(strings); break;
        case ns_sets:
            for (unsigned i = length(); i; --i) reference(namespaces);
            break;
        case names:
            switch (byte())
            {
            case 0x07: case 0x0D: reference(namespaces); reference(strings); break;
            case 0x0F: case 0x10: reference(strings); break;
            case 0x11: case 0x12: break;
            case 0x09: case 0x0E: reference(strings); reference(ns_sets); break;
            case 0x1B: case 0x1C: reference(ns_sets); break;
            case 0x1D:
                reference(names);
                for (unsigned i = length(); i; --i) reference(names);
                break;
            default: throw std::runtime_error("Unknown ABC multiname kind.");
            }
            break;
        case methods:
        {
            const auto parameters = length();
            for (unsigned i = 0; i <= parameters; ++i) reference(names);
            reference(strings);
            const auto flags = byte();
            if (flags & 8)
                for (unsigned i = length(); i; --i) constant(true);
            if (flags & 128)
                for (unsigned i = 0; i < parameters; ++i) reference(strings);
            break;
        }
        case metadata:
            reference(strings);
            for (unsigned i = length() * 2; i; --i) reference(strings);
            break;
        case instances:
            reference(names);
            reference(names);
            if (byte() & 8) reference(namespaces);
            for (unsigned i = length(); i; --i) reference(names);
            reference(methods);
            traits();
            break;
        case classes: case scripts: reference(methods); traits(); break;
        case bodies:
        {
            reference(methods);
            for (unsigned i = 0; i < 4; ++i) integer();
            const auto code = input.take(input.u30());
            const auto rewritten = indices.relocate ? relocate_code(code, indices) : RelocatedCode{};
            const auto bytes = indices.relocate ? ByteView(rewritten.bytes) : code;
            emit(static_cast<unsigned>(bytes.size()));
            append(output, bytes);
            for (unsigned i = length(); i; --i)
            {
                for (unsigned field = 0; field < 3; ++field)
                {
                    const auto offset = input.u30();
                    emit(indices.relocate ? rewritten.offset(offset) : offset);
                }
                reference(names);
                reference(names);
            }
            traits();
            break;
        }
        default: throw std::logic_error("Invalid ABC section.");
        }
    }
};

Tables read_tables(ByteView bytes, const Indices& indices = {})
{
    Reader input(bytes);
    if (input.u16() != 16 || input.u16() != 46)
        throw std::runtime_error("Class editing currently supports ABC 46.16.");
    Tables tables;
    for (unsigned section = 0; section < table_count; ++section)
    {
        const auto table = static_cast<Table>(section);
        const auto size = table == classes ? static_cast<unsigned>(tables[instances].size()) : count(input);
        const auto entries = pool_table(table) && size ? size - 1 : size;
        for (unsigned i = 0; i < entries; ++i)
        {
            Record record{input, indices, {}, {}};
            const auto start = input.position();
            record.entry(table);
            if (indices.relocate)
                tables[table].push_back(std::move(record.output));
            else
            {
                const auto raw = bytes.subspan(start, input.position() - start);
                tables[table].emplace_back(raw.begin(), raw.end());
            }
        }
    }
    if (input.remaining())
        throw std::runtime_error("Unexpected data after the ABC method bodies.");
    return tables;
}

Bytes write_tables(const Tables& tables)
{
    Bytes result{16, 0, 46, 0};
    for (unsigned section = 0; section < table_count; ++section)
    {
        const auto size = static_cast<unsigned>(tables[section].size());
        if (section != classes)
            put_u30(result, size + (pool_table(static_cast<Table>(section)) && size ? 1 : 0));
        for (const auto& record : tables[section])
            append(result, record);
    }
    return result;
}

unsigned initial_scope(const Tables& tables, unsigned method)
{
    for (const auto& body : tables[bodies])
    {
        Reader fields(body);
        if (fields.u30() == method)
        {
            fields.u30();
            fields.u30();
            return fields.u30();
        }
    }
    throw std::runtime_error("Class initializer has no method body.");
}

unsigned constructor(ByteView instance)
{
    Reader record(instance);
    record.u30(); record.u30();
    if (record.u8() & 8) record.u30();
    for (unsigned i = count(record); i; --i) record.u30();
    return record.u30();
}
}

Bytes hide_class_for_import(ByteView abc, std::size_t type)
{
    auto tables = read_tables(abc);
    auto& instance = tables[instances].at(type);
    Reader old_instance(instance);
    Reader old_name(tables[names].at(old_instance.u30() - 1));
    const auto kind = old_name.u8();
    if (kind != 0x07)
        throw std::runtime_error("The class does not have a qualified ABC name.");
    auto& pool = tables[strings];
    const std::string hidden = "FlashStudio$Import$" + std::to_string(type) + "$" + std::to_string(pool.size());
    Bytes text;
    put_u30(text, static_cast<unsigned>(hidden.size()));
    text.insert(text.end(), hidden.begin(), hidden.end());
    pool.push_back(std::move(text));
    Bytes name{kind};
    put_u30(name, old_name.u30());
    put_u30(name, static_cast<unsigned>(pool.size()));
    tables[names].push_back(std::move(name));
    const auto alias = static_cast<unsigned>(tables[names].size());
    Bytes hidden_instance;
    put_u30(hidden_instance, alias);
    append(hidden_instance, old_instance.take(old_instance.remaining()));
    instance = std::move(hidden_instance);
    for (auto& script : tables[scripts])
    {
        Reader input(script);
        Record record{input, {}, {}, std::pair{static_cast<unsigned>(type), alias}};
        record.entry(scripts);
        script = std::move(record.output);
    }
    return write_tables(tables);
}

Bytes link_class(ByteView destination, std::size_t type, ByteView compiled,
    std::span<const std::pair<unsigned, unsigned>> existing_methods)
{
    auto original = read_tables(destination);
    const auto source = read_tables(compiled);
    if (source[classes].size() != 1 || source[scripts].size() != 1 || type >= original[classes].size())
        throw std::runtime_error("Compile exactly one class per edit.");
    Indices indices;
    indices.class_target = static_cast<unsigned>(type);
    indices.relocate = true;
    for (unsigned table = 0; table < table_count; ++table)
    {
        indices.base[table] = static_cast<unsigned>(original[table].size());
        indices.limits[table] = static_cast<unsigned>(source[table].size());
    }
    const auto unassigned = indices.base[methods];
    indices.method_indices.assign(source[methods].size(), unassigned);
    std::vector<bool> replaced(original[methods].size());
    for (const auto& [from, to] : existing_methods)
    {
        if (from >= indices.method_indices.size() || to >= replaced.size() || replaced[to] ||
            indices.method_indices[from] != unassigned)
            throw std::runtime_error("Conflicting method bindings in the edited class.");
        indices.method_indices[from] = to;
        replaced[to] = true;
    }
    auto next_method = unassigned;
    for (auto& index : indices.method_indices)
        if (index == unassigned)
            index = next_method++;
    auto linked = read_tables(compiled, indices);
    const auto same_scope = [&](unsigned before, unsigned after)
    {
        if (initial_scope(original, before) != initial_scope(linked, after))
            throw std::runtime_error("The compiler changed the class scope layout. The SWF was not written.");
    };
    same_scope(constructor(original[instances][type]), constructor(linked[instances][0]));
    same_scope(Reader(original[classes][type]).u30(), Reader(linked[classes][0]).u30());
    original[instances][type] = std::move(linked[instances][0]);
    original[classes][type] = std::move(linked[classes][0]);
    // Existing scripts continue to initialize the same class index with their original scope and traits.
    for (std::size_t i = 0; i < indices.method_indices.size(); ++i)
    {
        const auto index = indices.method_indices[i];
        if (index < unassigned)
            original[methods][index] = std::move(linked[methods][i]);
        else
            original[methods].push_back(std::move(linked[methods][i]));
    }
    std::erase_if(original[bodies], [&](const Bytes& body) { return replaced[Reader(body).u30()]; });
    const auto imported_script = Reader(linked[scripts][0]).u30();
    std::erase_if(linked[bodies], [&](const Bytes& body) { return Reader(body).u30() == imported_script; });
    for (const auto table : {ints, uints, doubles, strings, namespaces, ns_sets, names, metadata, bodies})
    {
        auto& target = original[table];
        auto& records = linked[table];
        target.insert(target.end(), std::make_move_iterator(records.begin()), std::make_move_iterator(records.end()));
    }
    return write_tables(original);
}
}
