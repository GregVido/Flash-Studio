#include "ActionScript.hpp"
#include "Avm2Code.hpp"
#include "Abc.hpp"

#include <array>
#include <iterator>
#include <map>
#include <set>
#include <Windows.h>

namespace flash::script
{
namespace
{
struct Multiname
{
    unsigned kind{}, string{}, ns{}, base{};
    std::vector<unsigned> types;
};

struct Method
{
    unsigned result{}, debug_name{};
    std::vector<unsigned> types;
    std::vector<std::string> parameters;
    std::vector<std::pair<unsigned, unsigned>> defaults;
    bool rest = false, arguments = false;
    std::string label;
};

struct ActivationSlot
{
    unsigned id, named, type, initial, kind;
};

struct Body
{
    unsigned method, max_stack, locals, initial_scope, max_scope;
    std::size_t begin, traits;
    ByteView code;
    std::vector<ExceptionEntry> exceptions;
    std::vector<ActivationSlot> slots;
};

struct ScriptTraits
{
    std::vector<unsigned> classes;
    bool class_only = true;
};

struct SourceNames
{
    const std::map<std::string, std::string, std::less<>>& symbols;
    std::string_view package;
    std::set<std::string> imports;

    std::string spell(std::string_view text, const std::set<std::string>* local_names = nullptr)
    {
        const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$'; };
        const auto id = [&](char c) { return letter(c) || (c >= '0' && c <= '9'); };
        std::string result;
        for (std::size_t i = 0; i < text.size();)
        {
            const auto begin = i;
            const auto first = text[i++];
            if (first == '"' || first == '\'')
            {
                while (i < text.size())
                {
                    const auto c = text[i++];
                    if (c == first)
                        break;
                    if (c == '\\' && i < text.size())
                        ++i;
                }
            }
            else if (letter(first))
            {
                while (i < text.size() && (id(text[i]) || (text[i] == '.' && i + 1 < text.size() && letter(text[i + 1]))))
                    ++i;
                const auto word = text.substr(begin, i - begin);
                if (const auto period = word.rfind('.'); period != std::string_view::npos)
                {
                    const auto alias = word.substr(period + 1);
                    const auto found = symbols.find(alias);
                    if (found != symbols.end() && found->second == word &&
                        (!local_names || !local_names->contains(std::string(alias))))
                    {
                        if (word.substr(0, period) != package)
                            imports.emplace(word);
                        result += alias;
                        continue;
                    }
                }
            }
            result += text.substr(begin, i - begin);
        }
        return result;
    }
};

unsigned bounded_count(Reader& input, unsigned limit = 100'000)
{
    const auto count = input.u30();
    if (count > limit || count > input.remaining() + 1)
        throw std::runtime_error("Nombre d’entrées ABC invalide.");
    return count;
}

template<typename T, typename Read>
void pool(Reader& input, std::vector<T>& values, Read read)
{
    const auto count = bounded_count(input);
    values.reserve((std::max)(count, 1u));
    values.emplace_back();
    for (unsigned i = 1; i < count; ++i)
        values.push_back(read());
}

template<typename T>
const T& at(const std::vector<T>& values, unsigned index)
{
    if (index >= values.size())
        throw std::runtime_error("Indice de table ABC invalide.");
    return values[index];
}

bool multiname_operand(unsigned op)
{
    switch (op)
    {
    case 0x04: case 0x05: case 0x45: case 0x46: case 0x4A: case 0x4C: case 0x4E: case 0x4F:
    case 0x59: case 0x5B: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x66: case 0x68:
    case 0x6A: case 0x80: case 0x86: case 0xB2: return true;
    default: return false;
    }
}

std::optional<unsigned> load_register(const Instruction& item)
{
    if (item.op == 0x62)
        return static_cast<unsigned>(item.args[0]);
    if (item.op >= 0xD0 && item.op <= 0xD3)
        return item.op & 3;
    return {};
}

std::optional<unsigned> store_register(const Instruction& item)
{
    if (item.op == 0x63)
        return static_cast<unsigned>(item.args[0]);
    if (item.op >= 0xD4 && item.op <= 0xD7)
        return item.op & 3;
    return {};
}

struct EnumerationLoop
{
    unsigned object, variable, coercion, coercion_type;
    std::size_t body;
    bool values, slot, duplicate;
};

std::optional<EnumerationLoop> enumeration_loop(const std::vector<Instruction>& code, std::size_t jump, std::size_t test,
    std::optional<unsigned> activation)
{
    const auto& next = code[test];
    if (next.op != 0x32 || code[test + 1].op != 0x11)
        return {};
    const auto object = static_cast<unsigned>(next.args[0]);
    const auto cursor = static_cast<unsigned>(next.args[1]);
    if (!object || !cursor || object == cursor)
        return {};
    auto start = jump + 1;
    while (start < test && code[start].op == 0x09)
        ++start;
    const auto slot = activation && start < test && code[start].op == 0x65 && code[start].args[0] == 1;
    if (slot)
        ++start;
    if (test - start < 4 || load_register(code[start]) != object || load_register(code[start + 1]) != cursor)
        return {};
    const auto opcode = code[start + 2].op;
    if (opcode != 0x1E && opcode != 0x23)
        return {};
    auto assignment = start + 3;
    unsigned coercion = 0, coercion_type = 0;
    if (code[assignment].op == 0x80 || code[assignment].op == 0x82 || code[assignment].op == 0x85)
    {
        if (code[assignment].op == 0x80)
            coercion_type = static_cast<unsigned>(code[assignment].args[0]);
        coercion = code[assignment++].op;
    }
    if (assignment >= test)
        return {};
    const auto duplicate = !slot && code[assignment].op == 0x2A;
    if (duplicate && ++assignment >= test)
        return {};
    const auto& store = code[assignment];
    const auto variable = slot && store.op == 0x6D ? std::optional{static_cast<unsigned>(store.args[0])} : store_register(store);
    if (!variable || !*variable || (slot && store.op != 0x6D) || (!slot && (*variable == object || *variable == cursor || *variable == activation)))
        return {};
    // The compiler keeps the traversal registers private to this enumeration.
    if (test + 3 >= code.size() || code[test + 2].op != 0x08 || code[test + 3].op != 0x08 ||
        !((code[test + 2].args[0] == object && code[test + 3].args[0] == cursor) ||
          (code[test + 2].args[0] == cursor && code[test + 3].args[0] == object)))
        return {};
    for (auto i = assignment + 1; i < test; ++i)
    {
        const auto& instruction = code[i];
        const auto reading = load_register(instruction), writing = store_register(instruction);
        if (reading == object || reading == cursor || writing == object || writing == cursor)
            return {};
        if (instruction.op == 0x08 || instruction.op == 0x32 || instruction.op == 0x92 || instruction.op == 0x94 ||
            instruction.op == 0xC2 || instruction.op == 0xC3)
            if (std::ranges::find(instruction.args, object) != instruction.args.end() ||
                std::ranges::find(instruction.args, cursor) != instruction.args.end())
                return {};
    }
    const auto begin = jump > 24 ? jump - 24 : 0;
    for (auto i = jump; i > begin + 1;)
    {
        --i;
        if (store_register(code[i]) == cursor)
        {
            const auto& zero = code[i - 1];
            if ((zero.op == 0x24 || zero.op == 0x25) && zero.args[0] == 0)
                return EnumerationLoop{object, *variable, coercion, coercion_type, assignment + 1, opcode == 0x23, slot, duplicate};
            break;
        }
    }
    return {};
}

class Abc
{
public:
    explicit Abc(ByteView bytes);
    std::string render(bool source) const;
    std::string render(unsigned method, bool source) const;
    std::string pcode(unsigned method, bool simplify = false) const;
    std::string flow_graph(unsigned method) const;
    std::optional<std::string> source_problem(unsigned method) const;
    const std::vector<unsigned>* class_script(unsigned method) const;
    Bytes replace_method(unsigned method, std::string_view assembly) const;
    Bytes replace_string(unsigned index, std::string_view text) const;
    std::size_t obfuscated_methods() const;
    AbcDeobfuscation deobfuscate(std::optional<std::size_t> type) const;
    std::vector<AbcMethod> catalog() const;
    AbcClassSource render_class(std::size_t index) const;
    unsigned major() const { return major_; }
    unsigned minor() const { return minor_; }
    const auto& strings() const { return strings_; }
    const auto& classes() const { return classes_; }
private:
    std::string name(unsigned index, bool qualified = true, unsigned depth = 0) const;
    std::string method_name(unsigned index) const;
    std::string literal(unsigned op, unsigned index) const;
    std::string resolved_operand(unsigned op, unsigned index) const;
    std::string method_signature(unsigned method, std::string_view declared_name = {}, bool constructor = false) const;
    std::string access(unsigned multiname) const;
    std::string initial_value(unsigned index, unsigned kind) const;
    void read_traits(Reader& input, AbcClass* type = nullptr, bool static_member = false, ScriptTraits* script = nullptr,
        std::vector<ActivationSlot>* slots = nullptr);
    void label(unsigned method, std::string text);
    void assembly(const Body& body, Output& out) const;
    void source(const Body& body, Output& out, SourceNames* names = nullptr, std::string_view static_owner = {}) const;
    void render_body(const Body& body, bool as_source, Output& out) const;
    void validate(unsigned method, const AssembledMethod& assembled) const;
    std::string render_pcode(const Body& body, std::span<const Instruction> instructions, Output out) const;
    const Body& body(unsigned method) const;

    ByteView bytes_;
    unsigned major_{}, minor_{};
    std::vector<AbcClass> classes_;
    std::vector<unsigned> class_bases_;
    std::map<unsigned, std::vector<unsigned>> class_scripts_;
    std::vector<std::int32_t> integers_;
    std::vector<std::uint32_t> unsigned_, namespaces_;
    std::vector<unsigned> namespace_kinds_{0};
    std::vector<double> doubles_;
    std::vector<float> floats_;
    std::vector<std::array<float, 4>> float4s_;
    std::vector<std::string> strings_;
    std::vector<std::pair<std::size_t, std::size_t>> string_ranges_{{0, 0}};
    std::vector<Multiname> multinames_;
    std::map<std::string, std::string, std::less<>> public_symbols_;
    std::vector<Method> methods_;
    std::vector<Body> bodies_;
    std::vector<std::optional<std::size_t>> body_by_method_;
};

Abc::Abc(ByteView bytes) : bytes_(bytes)
{
    Reader input(bytes);
    minor_ = input.u16();
    major_ = input.u16();
    if ((major_ != 46 || minor_ != 16) && (major_ != 47 || minor_ < 12 || minor_ > 20))
        throw std::runtime_error(std::format("Version ABC {}.{} non prise en charge.", major_, minor_));

    pool(input, integers_, [&] { return std::bit_cast<std::int32_t>(input.v32()); });
    pool(input, unsigned_, [&] { return input.v32(); });
    pool(input, doubles_, [&] { return input.f64(); });
    if (major_ == 47 && minor_ >= 16)
    {
        pool(input, floats_, [&] { return std::bit_cast<float>(input.u32()); });
        pool(input, float4s_, [&]
        {
            std::array<float, 4> value{};
            for (auto& component : value)
                component = std::bit_cast<float>(input.u32());
            return value;
        });
    }
    pool(input, strings_, [&]
    {
        const auto begin = input.position();
        auto value = input.string(input.u30());
        string_ranges_.emplace_back(begin, input.position());
        return value;
    });
    pool(input, namespaces_, [&] { namespace_kinds_.push_back(input.u8()); return input.u30(); });
    const auto sets = bounded_count(input);
    for (unsigned i = 1; i < sets; ++i)
    {
        const auto size = bounded_count(input);
        for (unsigned j = 0; j < size; ++j)
            input.u30();
    }
    pool(input, multinames_, [&]
    {
        Multiname entry{};
        entry.kind = input.u8();
        switch (entry.kind)
        {
        case 0x07: case 0x0D: entry.ns = input.u30(); entry.string = input.u30(); break;
        case 0x0F: case 0x10: entry.string = input.u30(); break;
        case 0x11: case 0x12: break;
        case 0x09: case 0x0E: entry.string = input.u30(); input.u30(); break;
        case 0x1B: case 0x1C: input.u30(); break;
        case 0x1D:
        {
            entry.base = input.u30();
            const auto count = bounded_count(input, 32);
            for (unsigned i = 0; i < count; ++i)
                entry.types.push_back(input.u30());
            break;
        }
        default: throw std::runtime_error("Type de multinom ABC inconnu.");
        }
        return entry;
    });
    for (const auto& entry : multinames_)
    {
        if (entry.kind != 0x07 || entry.ns >= namespaces_.size() || namespace_kinds_[entry.ns] != 0x16 ||
            entry.string >= strings_.size() || namespaces_[entry.ns] >= strings_.size())
            continue;
        const auto& local = strings_[entry.string];
        if (identifier(local, "").empty())
            continue;
        const auto& package = strings_[namespaces_[entry.ns]];
        const auto qualified = package.empty() ? local : package + '.' + local;
        const auto [found, inserted] = public_symbols_.emplace(local, qualified);
        if (!inserted && found->second != qualified)
            found->second.clear();
    }

    methods_.resize(bounded_count(input));
    for (auto& method : methods_)
    {
        const auto params = bounded_count(input, 1024);
        method.result = input.u30();
        for (unsigned i = 0; i < params; ++i)
            method.types.push_back(input.u30());
        method.debug_name = input.u30();
        const auto flags = input.u8();
        method.rest = (flags & 4) != 0;
        method.arguments = (flags & 1) != 0;
        if (flags & 8)
        {
            const auto defaults = bounded_count(input, params);
            for (unsigned i = 0; i < defaults; ++i)
            {
                const auto value = input.u30();
                const auto kind = input.u8();
                method.defaults.emplace_back(value, kind);
            }
        }
        std::set<std::string> used{"arguments", "rest"};
        for (unsigned i = 0; i < params; ++i)
        {
            const auto fallback = std::format("arg{}", i + 1);
            auto parameter = (flags & 128) ? identifier(at(strings_, input.u30()), fallback) : fallback;
            if (!used.insert(parameter).second)
            {
                parameter = fallback;
                while (!used.insert(parameter).second)
                    parameter += '_';
            }
            method.parameters.push_back(std::move(parameter));
        }
    }

    const auto metadata = bounded_count(input);
    for (unsigned i = 0; i < metadata; ++i)
    {
        input.u30();
        const auto count = bounded_count(input);
        for (unsigned j = 0; j < count * 2; ++j)
            input.u30();
    }
    classes_.resize(bounded_count(input));
    for (auto& type : classes_)
    {
        const auto class_name = input.u30();
        type.name = name(class_name, false);
        type.qualified = name(class_name);
        if (const auto separator = type.qualified.rfind('.'); separator != std::string::npos)
            type.package = type.qualified.substr(0, separator);
        const auto base = input.u30();
        class_bases_.push_back(base);
        const auto flags = input.u8();
        type.interface_type = (flags & 4) != 0;
        type.declaration = access(class_name);
        if (type.interface_type)
            type.declaration += "interface ";
        else
        {
            if (flags & 2)
                type.declaration += "final ";
            if (!(flags & 1))
                type.declaration += "dynamic ";
            type.declaration += "class ";
        }
        type.declaration += type.name;
        if (base && !type.interface_type && name(base) != "Object")
            type.declaration += " extends " + name(base);
        if (flags & 8)
            input.u30();
        const auto interfaces = bounded_count(input);
        for (unsigned j = 0; j < interfaces; ++j)
        {
            type.declaration += j ? ", " : type.interface_type ? " extends " : " implements ";
            type.declaration += name(input.u30());
        }
        const auto constructor = input.u30();
        label(constructor, type.qualified + "::<init>");
        read_traits(input, &type);
        if (!type.interface_type)
        {
            const auto first_method = std::ranges::find_if(type.members, [](const AbcMember& member) { return member.method.has_value(); });
            type.members.insert(first_method, {type.name, "public " + method_signature(constructor, type.name, true), constructor});
        }
    }
    for (auto& type : classes_)
    {
        const auto initializer = input.u30();
        label(initializer, type.qualified + "::<class>");
        read_traits(input, &type, true);
        type.members.push_back({"Initialiseur statique", "", initializer});
    }
    const auto scripts = bounded_count(input);
    for (unsigned i = 0; i < scripts; ++i)
    {
        const auto initializer = input.u30();
        label(initializer, std::format("script_{}", i));
        ScriptTraits traits;
        read_traits(input, nullptr, false, &traits);
        if (traits.class_only && !traits.classes.empty())
            class_scripts_.try_emplace(initializer, std::move(traits.classes));
    }
    const auto body_count = bounded_count(input);
    body_by_method_.resize(methods_.size());
    bodies_.reserve(body_count);
    for (unsigned i = 0; i < body_count; ++i)
    {
        Body body{};
        body.method = input.u30();
        at(methods_, body.method);
        if (body_by_method_[body.method])
            throw std::runtime_error("Corps de méthode ABC en double.");
        body.begin = input.position();
        body.max_stack = input.u30();
        body.locals = input.u30();
        body.initial_scope = input.u30();
        body.max_scope = input.u30();
        body.code = input.take(input.u30());
        const auto exception_count = bounded_count(input);
        body.exceptions.reserve(exception_count);
        for (unsigned j = 0; j < exception_count; ++j)
            body.exceptions.push_back({input.u30(), input.u30(), input.u30(), input.u30(), input.u30()});
        body.traits = input.position();
        read_traits(input, nullptr, false, nullptr, &body.slots);
        body_by_method_[body.method] = bodies_.size();
        bodies_.push_back(std::move(body));
    }
    if (input.remaining())
        throw std::runtime_error("Des octets suivent les corps de méthodes ABC.");
}

void Abc::label(unsigned method, std::string text)
{
    at(methods_, method);
    methods_[method].label = std::move(text);
}

std::string Abc::access(unsigned multiname) const
{
    if (!multiname)
        return {};
    switch (at(namespace_kinds_, at(multinames_, multiname).ns))
    {
    case 0x05: return "private ";
    case 0x16: return "public ";
    case 0x17: return "internal ";
    case 0x18: case 0x1A: return "protected ";
    default: return {};
    }
}

std::string Abc::initial_value(unsigned index, unsigned kind) const
{
    switch (kind)
    {
    case 0x00: return "undefined";
    case 0x01: return literal(0x2C, index);
    case 0x03: return literal(0x2D, index);
    case 0x04: return literal(0x2E, index);
    case 0x06: return literal(0x2F, index);
    case 0x0A: return "false";
    case 0x0B: return "true";
    case 0x0C: return "null";
    default: return {};
    }
}

void Abc::read_traits(Reader& input, AbcClass* type, bool static_member, ScriptTraits* script, std::vector<ActivationSlot>* slots)
{
    const auto count = bounded_count(input);
    for (unsigned i = 0; i < count; ++i)
    {
        const auto named = input.u30();
        const auto flags = input.u8();
        if (script && (flags & 15) != 4)
            script->class_only = false;
        const auto slot_id = input.u30();
        const auto modifier = [&] { return access(named) + (static_member ? "static " : ""); };
        switch (flags & 15)
        {
        case 0: case 6:
        {
            const auto field_type = input.u30();
            const auto index = input.u30();
            const auto kind = index ? static_cast<unsigned>(input.u8()) : 0u;
            if (slots)
                slots->push_back({slot_id, named, field_type, index, kind});
            if (type)
            {
                auto member = name(named, false);
                auto declaration = modifier() + ((flags & 15) == 6 ? "const " : "var ") + member + ':' + name(field_type);
                if (index)
                {
                    const auto value = initial_value(index, kind);
                    if (!value.empty())
                        declaration += " = " + value;
                }
                type->members.push_back({std::move(member), std::move(declaration), {}});
            }
            break;
        }
        case 1: case 2: case 3: case 5:
        {
            auto member = name(named, false);
            const auto method = input.u30();
            label(method, type ? type->qualified + "::" + member : member);
            if (type)
            {
                auto declaration = type->interface_type ? std::string{} : modifier();
                // Static traits can carry the AVM2 flag even though AS3 forbids it on static methods.
                if (!static_member && (flags & 16))
                    declaration += "final ";
                if (flags & 32)
                    declaration += "override ";
                const auto kind = flags & 15;
                declaration += method_signature(method, member);
                if (kind == 2 || kind == 3)
                {
                    member.insert(0, kind == 2 ? "get " : "set ");
                    declaration.insert(declaration.find("function ") + 9, kind == 2 ? "get " : "set ");
                }
                type->members.push_back({std::move(member), std::move(declaration), method});
            }
            break;
        }
        case 4:
        {
            const auto index = input.u30();
            if (script)
                script->classes.push_back(index);
            break;
        }
        default: throw std::runtime_error("Trait ABC inconnu.");
        }
        if (flags & 64)
        {
            const auto items = bounded_count(input);
            for (unsigned j = 0; j < items; ++j)
                input.u30();
        }
    }
}

std::string Abc::name(unsigned index, bool qualified, unsigned depth) const
{
    if (!index)
        return "*";
    if (depth > 8)
        throw std::runtime_error("Multinom ABC récursif.");
    const auto& entry = at(multinames_, index);
    if (entry.kind == 0x1D)
    {
        auto result = name(entry.base, qualified, depth + 1) + ".<";
        for (std::size_t i = 0; i < entry.types.size(); ++i)
        {
            if (i)
                result += ", ";
            result += name(entry.types[i], qualified, depth + 1);
            if (result.size() > 1024)
                throw std::runtime_error("Type ABC trop complexe.");
        }
        return result + '>';
    }
    if (!entry.string)
        return "*";
    auto result = at(strings_, entry.string).substr(0, 256);
    if (qualified && entry.ns)
    {
        const auto& prefix = at(strings_, at(namespaces_, entry.ns));
        if (!prefix.empty())
            result = prefix.substr(0, 256) + '.' + result;
    }
    return result;
}

std::string Abc::method_name(unsigned index) const
{
    const auto& method = at(methods_, index);
    if (!method.label.empty())
        return method.label;
    const auto& debug = at(strings_, method.debug_name);
    return debug.empty() ? std::format("method_{}", index) : debug.substr(0, 256);
}

std::string Abc::literal(unsigned op, unsigned index) const
{
    switch (op)
    {
    case 0x22: return std::format("{}", at(floats_, index));
    case 0x25: return std::format("{}", std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(index)));
    case 0x2C: return quote(at(strings_, index));
    case 0x2D: return std::format("{}", at(integers_, index));
    case 0x2E: return std::format("{}", at(unsigned_, index));
    case 0x2F: return std::format("{}", at(doubles_, index));
    case 0x31: return quote(at(strings_, at(namespaces_, index)));
    case 0x54:
    {
        const auto& v = at(float4s_, index);
        return std::format("float4({}, {}, {}, {})", v[0], v[1], v[2], v[3]);
    }
    default: return {};
    }
}

std::string Abc::resolved_operand(unsigned op, unsigned index) const
{
    auto resolved = multiname_operand(op) ? name(index) : literal(op, index);
    if (op == 0x40 || op == 0x44)
        resolved = method_name(index);
    if (op == 0x06 || op == 0xF1)
        resolved = quote(at(strings_, index));
    std::ranges::replace_if(resolved, [](unsigned char c) { return c < 32; }, ' ');
    return resolved;
}

void Abc::assembly(const Body& body, Output& out) const
{
    Reader code(body.code);
    while (code.remaining())
    {
        Instruction item;
        const auto address = code.position();
        try { item = decode(code); }
        catch (const std::runtime_error& error)
        {
            out.add(std::format("    ; {}\n    ; Octets restants à partir de 0x{:04X}\n", error.what(), address));
            out.add(hex_dump(body.code.subspan(address)));
            return;
        }
        const auto op = item.op;
        auto line = std::format("    {:04X}  {}", item.offset, opcodes[op].name);
        for (std::size_t i = 0; i < item.args.size(); ++i)
        {
            const auto value = item.args[i];
            line += i ? ", " : "  ";
            if (op >= 0x0C && op <= 0x1B)
                line += std::format("0x{:04X}", value);
            else
            {
                line += std::to_string(value);
                if (i == 0)
                {
                    try
                    {
                        const auto resolved = resolved_operand(op, static_cast<unsigned>(value));
                        if (!resolved.empty())
                            line += " (" + resolved + ')';
                    }
                    catch (const std::runtime_error&) { line += " (indice hors table)"; }
                }
            }
        }
        out.add(line + '\n');
    }
}

void Abc::source(const Body& body, Output& output, SourceNames* names, std::string_view static_owner) const
{
    constexpr std::string_view lexical_scope = "\x01lexical", global_scope = "\x01global";
    constexpr std::string_view activation_scope = "\x01" "activation";
    const auto validate_expression = [](std::string_view text)
    {
        if (text.find('\x01') != std::string_view::npos)
            throw std::runtime_error("Objet de portée utilisé hors d’un accès ActionScript reconnu.");
    };
    const auto& method = methods_[body.method];
    const auto argument_count = method.parameters.size() + (method.rest || method.arguments);
    std::set<std::string> used_names(method.parameters.begin(), method.parameters.end());
    std::map<unsigned, std::string> local_names;
    const auto local = [&](unsigned number)
    {
        if (number == 0)
            return static_owner.empty() ? std::string("this") : std::string(static_owner);
        if (number <= method.parameters.size())
            return method.parameters[number - 1];
        if (number == argument_count)
            return std::string(method.rest ? "rest" : "arguments");
        auto [it, inserted] = local_names.try_emplace(number);
        if (inserted)
        {
            if (number >= body.locals)
                throw std::runtime_error("Registre local hors limites.");
            it->second = std::format("local{}", number);
            while (!used_names.insert(it->second).second)
                it->second += '_';
        }
        return it->second;
    };
    std::vector<Instruction> instructions;
    Reader code(body.code);
    while (code.remaining())
    {
        if (instructions.size() == 65'536)
            throw std::runtime_error("Trop d’instructions pour reconstruire cette méthode.");
        instructions.push_back(decode(code));
    }
    const bool own_scope = instructions.size() >= 2 && instructions[0].op == 0xD0 && instructions[1].op == 0x30;
    std::optional<unsigned> activation;
    if (own_scope && instructions.size() >= 6 && instructions[2].op == 0x57 && instructions[3].op == 0x2A &&
        instructions[5].op == 0x30 && !body.slots.empty())
    {
        activation = store_register(instructions[4]);
        if (activation && (!*activation || *activation <= argument_count ||
            std::ranges::any_of(instructions, [](const Instruction& item) { return item.op == 0x40; })))
            throw std::runtime_error("Portée d’activation capturée ou partagée avec un argument.");
    }
    const auto body_start = activation ? std::size_t{6} : 0;
    for (const auto& item : instructions)
    {
        switch (item.op)
        {
        case 0x08: case 0x62: case 0x63: case 0x92: case 0x94: case 0xC2: case 0xC3:
            local(static_cast<unsigned>(item.args[0]));
            break;
        case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
            local(item.op & 3);
            break;
        default: break;
        }
    }
    const auto qualified_name = [&](unsigned index)
    {
        const auto original = name(index);
        return names ? names->spell(original, &used_names) : original;
    };
    std::map<unsigned, std::string> local_types;
    std::map<unsigned, std::size_t> inline_stores;
    if (!local_names.empty())
    {
        std::set<unsigned> ambiguous;
        std::map<std::size_t, std::vector<std::size_t>> branches;
        std::set<std::size_t> entries;
        const auto target = [&](std::int64_t offset)
        {
            if (offset < 0)
                return instructions.size();
            const auto address = static_cast<std::size_t>(offset);
            const auto found = std::ranges::lower_bound(instructions, address, {}, &Instruction::offset);
            if (found != instructions.end() && found->offset == address)
                return static_cast<std::size_t>(found - instructions.begin());
            return instructions.size();
        };
        for (std::size_t i = 0; i < instructions.size(); ++i)
        {
            const auto& item = instructions[i];
            if (item.op >= 0x0C && item.op <= 0x1B)
                for (const auto address : item.args)
                {
                    const auto to = target(address);
                    branches[i].push_back(to);
                    entries.insert(to);
                }
        }
        for (const auto& handler : body.exceptions)
            entries.insert(target(handler.target));

        const auto constructed_type = [&](std::size_t position) -> std::string
        {
            const auto& constructor = instructions[position];
            unsigned missing = static_cast<unsigned>(constructor.args.back());
            // Walk over straight-line arguments without assuming the type of a runtime callee.
            for (unsigned steps = 0; missing; ++steps)
            {
                if (!position || entries.contains(position) || steps == 128)
                    return {};
                const auto& operand = instructions[--position];
                unsigned consumed = 0;
                if (!load_register(operand))
                    switch (operand.op)
                    {
                    case 0x20: case 0x21: case 0x22: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
                    case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x31: case 0x54: case 0x60:
                        break;
                    case 0x66:
                    {
                        const auto kind = at(multinames_, static_cast<unsigned>(operand.args[0])).kind;
                        if (kind != 0x07 && kind != 0x09)
                            return {};
                        consumed = 1;
                        break;
                    }
                    default: return {};
                    }
                missing = missing - 1 + consumed;
            }
            if (!position || entries.contains(position))
                return {};
            const auto& lookup = instructions[position - 1];
            const bool direct = constructor.op == 0x42;
            if ((direct && lookup.op != 0x60) ||
                (!direct && (lookup.op != 0x5D || lookup.args[0] != constructor.args[0])))
                return {};
            const auto type = name(static_cast<unsigned>(lookup.args[0]));
            return std::ranges::any_of(classes_, [&](const AbcClass& entry)
            {
                return !entry.interface_type && entry.qualified == type;
            }) ? type : std::string{};
        };
        const auto written_type = [&](std::size_t i) -> std::string
        {
            if (!i || entries.contains(i))
                return {};
            auto previous = i - 1;
            while (instructions[previous].op == 0x2A || instructions[previous].op == 0x82)
            {
                if (!previous || entries.contains(previous))
                    return {};
                --previous;
            }
            const auto& producer = instructions[previous];
            switch (producer.op)
            {
            case 0x24: case 0x25: case 0x2D: case 0x73: case 0x83: case 0xC0: case 0xC1:
            case 0xC4: case 0xC5: case 0xC6: case 0xC7: return "int";
            case 0x2E: case 0x74: case 0x88: return "uint";
            case 0x26: case 0x27: case 0x76: case 0x81: case 0x96: return "Boolean";
            case 0x28: case 0x2F: case 0x75: case 0x84: return "Number";
            case 0x2C: case 0x70: case 0x85: case 0x95: return "String";
            case 0x55: return "Object";
            case 0x56: return "Array";
            case 0x80: case 0x86: return name(static_cast<unsigned>(producer.args[0]));
            case 0x42: case 0x4A: return constructed_type(previous);
            default: break;
            }
            return {};
        };
        std::set<unsigned> locally_accessed;
        for (std::size_t i = 0; i < instructions.size(); ++i)
        {
            const auto& item = instructions[i];
            if (const auto reg = load_register(item); reg && local_names.contains(*reg))
                locally_accessed.insert(*reg);
            if (const auto reg = store_register(item); reg && local_names.contains(*reg))
            {
                if (locally_accessed.insert(*reg).second && !entries.contains(i))
                    inline_stores.emplace(*reg, i);
                const auto type = written_type(i);
                const auto [known, inserted] = local_types.try_emplace(*reg, type);
                if (type.empty() || type == "*" || (!inserted && known->second != type))
                    ambiguous.insert(*reg);
            }
            if (item.op == 0x08 || item.op == 0x32 || item.op == 0x92 || item.op == 0x94 || item.op == 0xC2 || item.op == 0xC3)
                for (const auto reg : item.args)
                {
                    const auto index = static_cast<unsigned>(reg);
                    ambiguous.insert(index);
                    if (local_names.contains(index))
                        locally_accessed.insert(index);
                }
        }

        const auto used_before_assignment = [&](unsigned reg)
        {
            std::vector<bool> visited(instructions.size());
            std::vector<std::size_t> pending{body_start};
            for (const auto& handler : body.exceptions)
                pending.push_back(target(handler.target));
            while (!pending.empty())
            {
                const auto i = pending.back();
                pending.pop_back();
                if (i >= instructions.size() || visited[i])
                    continue;
                visited[i] = true;
                const auto& item = instructions[i];
                if (load_register(item) == reg)
                    return true;
                if (store_register(item) == reg || item.op == 0x03 || item.op == 0x47 || item.op == 0x48)
                    continue;
                if (const auto successors = branches.find(i); successors != branches.end())
                    pending.insert(pending.end(), successors->second.begin(), successors->second.end());
                if (item.op != 0x10 && item.op != 0x1B)
                    pending.push_back(i + 1);
            }
            return false;
        };
        if (local_types.size() > 1'000'000 / (std::max)(std::size_t{1}, instructions.size()))
            local_types.clear();
        else
            std::erase_if(local_types, [&](const auto& type) { return ambiguous.contains(type.first) || used_before_assignment(type.first); });
    }
    if (!static_owner.empty() && (!local_names.empty() || activation))
        throw std::runtime_error("Variables locales dans l’initialiseur statique.");
    std::map<unsigned, std::string> local_declarations;
    for (const auto& [register_id, name] : local_names)
        if (register_id != activation)
        {
            const auto type = local_types.find(register_id);
            const auto annotation = type == local_types.end() ? "*" : names ? names->spell(type->second, &used_names) : type->second;
            local_declarations.emplace(register_id, "var " + name + ':' + annotation);
        }
    std::map<unsigned, std::string> slot_variables;
    std::map<unsigned, unsigned> variable_slots;
    Output slot_declarations;
    if (activation)
        for (const auto& slot : body.slots)
        {
            auto variable = identifier(name(slot.named, false), std::format("slot{}", slot.id));
            while (!used_names.insert(variable).second)
                variable += '_';
            if (!slot.id || !slot_variables.emplace(slot.id, variable).second || !variable_slots.emplace(slot.named, slot.id).second)
                throw std::runtime_error("Emplacements de portée d’activation ambigus.");
        }
    if (activation)
        for (const auto& slot : body.slots)
        {
            auto variable = "    var " + slot_variables.at(slot.id) + ':' + qualified_name(slot.type);
            if (slot.initial)
            {
                const auto value = initial_value(slot.initial, slot.kind);
                if (value.empty())
                    throw std::runtime_error("Valeur initiale de slot non représentée.");
                variable += " = " + value;
            }
            slot_declarations.add(variable + ";\n");
        }
    Output out;
    const auto destination = [&](std::int64_t address)
    {
        if (address == static_cast<std::int64_t>(body.code.size()))
            return instructions.size();
        if (address < 0)
            throw std::runtime_error("Adresse de saut négative.");
        const auto offset = static_cast<std::size_t>(address);
        const auto found = std::ranges::lower_bound(instructions, offset, {}, &Instruction::offset);
        if (found == instructions.end() || found->offset != offset)
            throw std::runtime_error("Saut vers une adresse sans instruction.");
        return static_cast<std::size_t>(found - instructions.begin());
    };
    const auto branch_condition = [&](unsigned op, Expressions& stack, bool taken)
    {
        if (op == 0x11 || op == 0x12)
        {
            auto value = stack.pop();
            validate_expression(value);
            return taken == (op == 0x11) ? std::move(value) : "!(" + value + ')';
        }
        constexpr std::string_view comparisons[]{"<", "<=", ">", ">=", "", "", "", "==", "!=", "<", "<=",
            ">", ">=", "===", "!=="};
        const auto right = stack.pop(), left = stack.pop();
        validate_expression(left);
        validate_expression(right);
        if (!taken && (op == 0x13 || op == 0x14 || op == 0x19 || op == 0x1A))
        {
            constexpr std::string_view inverse[]{"!=", "==", "", "", "", "", "!==", "==="};
            return std::format("{} {} {}", left, inverse[op - 0x13], right);
        }
        auto condition = std::format("{} {} {}", left, comparisons[op - 0x0C], right);
        return taken == (op >= 0x13) ? std::move(condition) : "!(" + condition + ')';
    };
    struct Loop { std::size_t next, exit; };
    struct CatchVariable { std::string name, instance; };
    struct TryRegion
    {
        std::size_t first, last, handler, join;
        std::string type;
        CatchVariable variable;
    };
    std::optional<CatchVariable> caught;
    std::optional<TryRegion> guarded;
    const auto unsupported_try = [] { return std::runtime_error("Structure try/catch ou finally non reconnue."); };
    if (!body.exceptions.empty())
    {
        if (body.exceptions.size() != 1)
            throw unsupported_try();
        const auto& handler = body.exceptions.front();
        const auto first = destination(handler.from), last = destination(handler.to), rescue = destination(handler.target);
        if (first < body_start || first >= last || last >= rescue || rescue + 2 > instructions.size() || instructions[last].op != 0x10 ||
            instructions[rescue].op != 0xD0 || instructions[rescue + 1].op != 0x30)
            throw unsupported_try();
        const auto join = destination(instructions[last].args[0]);
        auto binder = rescue + 2;
        if (activation && binder + 1 < join && load_register(instructions[binder]) == activation && instructions[binder + 1].op == 0x30)
            binder += 2;
        constexpr unsigned prologue[]{0x5A, 0x2A, 0, 0x2A, 0x30, 0x2B, 0x6D};
        constexpr auto setup = std::size(prologue);
        if (join < binder + setup + 2)
            throw unsupported_try();
        for (std::size_t i = 0; i < setup; ++i)
            if (prologue[i] && instructions[binder + i].op != prologue[i])
                throw unsupported_try();
        const auto temporary = store_register(instructions[binder + 2]);
        const auto& cleanup = instructions[join - 1];
        auto catch_name = identifier(name(handler.variable, false), "error");
        if (!temporary || !*temporary || activation == temporary || instructions[binder].args[0] != 0 || instructions[binder + 6].args[0] != 1 ||
            instructions[join - 2].op != 0x1D || cleanup.op != 0x08 || cleanup.args[0] != *temporary || used_names.contains(catch_name))
            throw unsupported_try();
        guarded = TryRegion{first, last, binder + setup, join, qualified_name(handler.type), {std::move(catch_name), local(*temporary)}};
    }
    bool emitted_try = false;
    struct SwitchDispatch
    {
        std::size_t store, dispatch;
        std::vector<std::pair<std::size_t, std::size_t>> values;
    };
    const auto find_switch = [&](std::size_t first, std::size_t tests, std::size_t end) -> std::optional<SwitchDispatch>
    {
        const auto integer = [&](const Instruction& instruction) -> std::int64_t
        {
            return instruction.op == 0x24 || instruction.op == 0x25 ? instruction.args[0] : -1;
        };
        auto store = tests;
        for (; store < end && store - tests < 40; ++store)
            if (store_register(instructions[store]) || (instructions[store].op >= 0x0C && instructions[store].op <= 0x1B))
                break;
        if (store == end || store - tests == 40 || store == tests)
            return {};
        const auto local = store_register(instructions[store]);
        if (!local || !*local)
            return {};
        auto comparison = store + 1;
        while (comparison + 2 < end && comparison - store <= 40 && instructions[comparison].op != 0x1A)
            ++comparison;
        if (comparison + 2 >= end || comparison - store > 40 || instructions[comparison + 2].op != 0x10)
            return {};
        const auto exit = destination(instructions[comparison + 2].args[0]);
        if (exit <= comparison + 2 || exit + 1 >= end || instructions[exit].op != 0x08 || instructions[exit].args[0] != *local ||
            instructions[exit + 1].op != 0x1B)
            return {};
        const auto& table = instructions[exit + 1].args;
        if (table.size() < 3 || table.size() > 257 || !std::ranges::is_sorted(table.begin() + 1, table.end()) ||
            std::ranges::any_of(table, [&](auto address)
        {
            const auto index = destination(address);
            return index < first || index >= tests;
        }))
            return {};

        SwitchDispatch result{store, exit + 1, {}};
        auto at = store + 1;
        for (std::size_t label = 1; label < table.size(); ++label)
        {
            // ASC puts one final default-only index in the table after the explicit comparisons.
            const auto remaining = exit - at;
            if (label + 1 == table.size() && (remaining == 1 || remaining == 4))
            {
                if (integer(instructions[exit - 1]) != static_cast<std::int64_t>(label - 1) || table[label] != table.front() ||
                    (remaining == 4 && (instructions[at].op != 0x10 || destination(instructions[at].args[0]) != exit - 1 ||
                        integer(instructions[at + 1]) != integer(instructions[exit - 1]) || instructions[at + 2].op != 0x10 ||
                        destination(instructions[at + 2].args[0]) != exit)))
                    return {};
                return result;
            }
            auto branch = at;
            for (; branch < exit && branch - at < 40 && instructions[branch].op != 0x1A; ++branch) {}
            if (branch < at + 2 || branch + 3 >= exit || instructions[branch].op != 0x1A ||
                load_register(instructions[branch - 1]) != local || destination(instructions[branch].args[0]) != branch + 3 ||
                integer(instructions[branch + 1]) != static_cast<std::int64_t>(label - 1) || instructions[branch + 2].op != 0x10 ||
                destination(instructions[branch + 2].args[0]) != exit)
                return {};
            result.values.emplace_back(at, branch - 1);
            at = branch + 3;
        }
        return {};
    };
    const auto statement_block = [&](auto&& visit, std::size_t first, std::size_t last, unsigned depth,
        bool expression = false, bool loop_test = false, std::optional<Loop> loop = {}, std::optional<std::string> initial = {}) -> std::string
    {
        if (depth > 24)
            throw std::runtime_error("Trop de blocs ActionScript imbriqués.");
        Expressions stack(256 * 1024);
        if (initial)
            stack.push(std::move(*initial));
        const std::string indent(depth * 4, ' ');
        const auto emit = [&](std::string text)
        {
            if (expression)
                throw std::runtime_error("Ce bloc produit une instruction plutôt qu’une expression.");
            validate_expression(text);
            std::string_view remaining(text);
            while (true)
            {
                const auto end = remaining.find('\n');
                out.add(indent);
                out.add(remaining.substr(0, end));
                if (end == std::string_view::npos)
                    break;
                out.add("\n");
                remaining.remove_prefix(end + 1);
            }
            out.add(";\n");
        };
        for (std::size_t i = first; i < last; ++i)
        {
            if (guarded && i == guarded->first && last >= guarded->join)
            {
                if (expression || !stack.empty() || emitted_try)
                    throw unsupported_try();
                emitted_try = true;
                out.add(indent + "try\n" + indent + "{\n");
                visit(visit, i, guarded->last, depth + 1, false, false, loop);
                caught = guarded->variable;
                out.add(indent + "}\n" + indent + "catch (" + caught->name + ':' + guarded->type + ")\n" + indent + "{\n");
                visit(visit, guarded->handler, guarded->join - 2, depth + 1, false, false, loop);
                caught.reset();
                out.add(indent + "}\n");
                i = guarded->join - 1;
                continue;
            }
            const auto& item = instructions[i];
            const auto op = item.op;
            const auto arg = [&](std::size_t index) { return static_cast<unsigned>(item.args.at(index)); };
            const auto lexical = [&]
            {
                if (const auto slot = variable_slots.find(arg(0)); slot != variable_slots.end())
                    return slot_variables.at(slot->second);
                const auto& entry = at(multinames_, arg(0));
                if (entry.kind != 0x07 && entry.kind != 0x09)
                    throw std::runtime_error("Accès à une propriété calculée à l’exécution.");
                const auto id = identifier(name(arg(0), false), "");
                if (id.empty())
                    throw std::runtime_error("Nom de propriété non représentable.");
                return entry.ns && at(namespace_kinds_, entry.ns) == 0x16 ? qualified_name(arg(0)) : id;
            };
            const auto property = [&]
            {
                const auto& entry = at(multinames_, arg(0));
                const auto descendant = op == 0x59 || op == 0x5B;
                const std::string separator = descendant ? ".." : ".";
                if (entry.kind == 0x1B)
                {
                    if (descendant)
                        throw std::runtime_error("Nom de descendant XML calculé à l’exécution.");
                    return '[' + stack.pop() + ']';
                }
                if (entry.kind == 0x1C)
                    return separator + "@[" + stack.pop() + ']';
                const bool attribute = entry.kind == 0x0D || entry.kind == 0x0E;
                if (!attribute && entry.kind != 0x07 && entry.kind != 0x09)
                    throw std::runtime_error("Espace de noms dynamique.");
                if ((attribute || descendant) && entry.ns && at(namespaces_, entry.ns))
                    throw std::runtime_error("Espace de noms XML qualifié.");
                if ((attribute || descendant) && !entry.string)
                    return separator + (attribute ? "@*" : "*");
                const auto key = name(arg(0), false);
                const auto id = identifier(key, "");
                if (attribute)
                    return separator + (id.empty() ? "@[" + quote(key) + ']' : '@' + id);
                if (descendant)
                {
                    if (id.empty())
                        throw std::runtime_error("Nom de descendant XML non représentable.");
                    return separator + id;
                }
                return id.empty() ? '[' + quote(key) + ']' : '.' + id;
            };
            const auto member = [&](const std::string& object, const std::string& field)
            {
                if (object == lexical_scope || object == global_scope)
                    return lexical();
                if (object.empty() || object == "null" || object == "undefined" || object.starts_with("new ") ||
                    object.starts_with("typeof ") || object.starts_with("delete ") || object.front() == '!' ||
                    object.front() == '~' || object.front() == '-' || (object.front() >= '0' && object.front() <= '9'))
                    return '(' + object + ')' + field;
                return object + field;
            };
            const auto reference = [&]
            {
                const auto field = property();
                return member(stack.pop(), field);
            };
            const auto writable_local = [&](unsigned index)
            {
                if (!index)
                    throw std::runtime_error("L’écriture du registre this ne se traduit pas en ActionScript.");
                if (activation == index)
                    throw std::runtime_error("Écriture dynamique du registre d’activation.");
                return local(index);
            };
            const auto assign_local = [&](unsigned index, std::size_t store)
            {
                auto target = writable_local(index);
                // Keep the initializer at its original execution point.
                if (const auto first_store = inline_stores.find(index);
                    depth == 1 && first_store != inline_stores.end() && first_store->second == store)
                {
                    if (auto declared = local_declarations.extract(index); declared)
                        target = std::move(declared.mapped());
                }
                emit(target + " = " + stack.pop_as3_value());
                return local(index);
            };
            const auto read_local = [&](unsigned index)
            {
                return activation == index ? std::string(activation_scope) : local(index);
            };
            const auto slot_name = [&](const std::string& owner)
            {
                const auto slot = slot_variables.find(arg(0));
                if (owner == activation_scope && slot != slot_variables.end())
                    return slot->second;
                if (caught && arg(0) == 1 && owner == caught->instance)
                    return caught->name;
                throw std::runtime_error("Slot de portée non identifié.");
            };
            if (loop_test && i + 1 == last)
            {
                auto result = branch_condition(op, stack, true);
                if (!stack.empty())
                    throw std::runtime_error("La condition de boucle laisse des valeurs sur la pile.");
                return result;
            }
            if (op == 0x10 && !expression && stack.empty())
            {
                const auto target = destination(item.args[0]);
                if (loop && (target == loop->next || target == loop->exit))
                {
                    emit(target == loop->next ? "continue" : "break");
                    continue;
                }
                if (target == i + 1)
                    continue;
                if (target > i + 1 && target < last)
                {
                    if (const auto selection = find_switch(i + 1, target, last))
                    {
                        const auto value = visit(visit, target, selection->store, depth + 1, true);
                        const auto& addresses = instructions[selection->dispatch].args;
                        std::map<std::size_t, std::vector<std::string>> cases;
                        for (std::size_t label = 0; label < selection->values.size(); ++label)
                        {
                            const auto [start, end] = selection->values[label];
                            cases[destination(addresses[label + 1])].push_back("case " + visit(visit, start, end, depth + 1, true));
                        }
                        cases[destination(addresses.front())].push_back("default");
                        const auto join = selection->dispatch + 1;
                        out.add(indent + "switch (" + value + ")\n" + indent + "{\n");
                        for (auto region = cases.begin(); region != cases.end(); ++region)
                        {
                            for (const auto& label : region->second)
                                out.add(indent + "    " + label + ":\n");
                            const auto next = std::next(region);
                            const auto end = next == cases.end() ? target : next->first;
                            visit(visit, region->first, end, depth + 2, false, false, Loop{loop ? loop->next : SIZE_MAX, join});
                        }
                        out.add(indent + "}\n");
                        i = selection->dispatch;
                        continue;
                    }
                    const auto& edge = instructions[target];
                    if (edge.op == 0x10 && destination(edge.args[0]) == i + 1)
                    {
                        out.add(indent + "while (true)\n" + indent + "{\n");
                        visit(visit, i + 1, target, depth + 1, false, false, Loop{target, target + 1});
                        out.add(indent + "}\n");
                        i = target;
                        continue;
                    }
                    auto test = target;
                    while (test < last && test - target < 256)
                    {
                        const auto& candidate = instructions[test];
                        const auto next = candidate.op;
                        if (next == 0x10 || next == 0x1B || next == 0x03 || next == 0x47 || next == 0x48)
                            break;
                        if (next >= 0x0C && next <= 0x1A)
                        {
                            const auto exit = destination(candidate.args[0]);
                            if (exit <= test || exit >= last)
                                break;
                        }
                        ++test;
                    }
                    if (test < last)
                    {
                        const auto& branch = instructions[test];
                        if (branch.op >= 0x0C && branch.op <= 0x1A && branch.op != 0x10 &&
                            destination(branch.args[0]) == i + 1)
                        {
                            if (test == target + 1)
                                if (const auto enumeration = enumeration_loop(instructions, i, target, activation))
                                {
                                    const auto named = slot_variables.find(enumeration->variable);
                                    if (enumeration->slot && named == slot_variables.end())
                                        throw std::runtime_error("Variable d’énumération dans un slot inconnu.");
                                    const auto variable = enumeration->slot ? named->second : local(enumeration->variable);
                                    auto entry = variable;
                                    const auto cast_string = enumeration->coercion == 0x85;
                                    const auto typed = enumeration->coercion == 0x80;
                                    if (cast_string || typed)
                                    {
                                        entry = "item";
                                        while (!used_names.insert(entry).second)
                                            entry += '_';
                                    }
                                    out.add(indent + (enumeration->values ? "for each (" : "for (") +
                                        (cast_string || typed ? "var " + entry + ":*" : entry) + " in " + local(enumeration->object) +
                                        ")\n" + indent + "{\n");
                                    if (cast_string)
                                        out.add(indent + "    " + variable + " = " + entry + " == null ? null : String(" + entry + ");\n");
                                    if (typed)
                                    {
                                        const auto type = qualified_name(enumeration->coercion_type);
                                        const auto scalar = type == "int" || type == "uint" || type == "Number" || type == "Boolean" || type == "String";
                                        const auto checked = scalar ? type + '(' + entry + ')' : '(' + entry + " as " + type + ')';
                                        out.add(indent + "    " + variable + " = " + checked + ";\n");
                                    }
                                    visit(visit, enumeration->body, target, depth + 1, false, false, Loop{target, test + 1},
                                        enumeration->duplicate ? std::optional{variable} : std::nullopt);
                                    out.add(indent + "}\n");
                                    i = test;
                                    continue;
                                }
                            const auto condition = visit(visit, target, test + 1, depth + 1, true, true);
                            out.add(indent + "while (" + condition + ")\n" + indent + "{\n");
                            visit(visit, i + 1, target, depth + 1, false, false, Loop{target, test + 1});
                            out.add(indent + "}\n");
                            i = test;
                            continue;
                        }
                    }
                }
            }
            if (op >= 0x0C && op <= 0x1A && op != 0x10)
            {
                std::string value;
                auto condition = branch_condition(op, stack, false);
                if (op == 0x11)
                    value = condition.substr(2, condition.size() - 3);
                else if (op == 0x12)
                    value = condition;
                auto end_then = destination(item.args[0]);
                if (loop && !expression && stack.empty() && (end_then == loop->next || end_then == loop->exit) && end_then > last)
                {
                    out.add(indent + "if (!(" + condition + "))\n" + indent + "{\n" + indent + "    " +
                        (end_then == loop->next ? "continue;\n" : "break;\n") + indent + "}\n");
                    continue;
                }
                if (end_then <= i || end_then > last)
                    throw std::runtime_error("Fusion ou saut conditionnel complexe.");
                if ((op == 0x11 || op == 0x12) && !stack.empty() && end_then > i + 2 &&
                    instructions[i + 1].op == 0x29)
                {
                    auto retained = stack.pop();
                    if (retained != value)
                        throw std::runtime_error("Valeurs différentes sur les branches du court-circuit.");
                    const auto right = visit(visit, i + 2, end_then, depth + 1, true);
                    stack.push(value);
                    stack.push(right);
                    stack.as3_binary(op == 0x11 ? "||" : "&&");
                    i = end_then - 1;
                    continue;
                }
                auto end_else = end_then;
                if (end_then > i + 1 && instructions[end_then - 1].op == 0x10)
                {
                    const auto successor = destination(instructions[end_then - 1].args[0]);
                    const bool loop_edge = loop && (successor == loop->next || successor == loop->exit);
                    if (!loop_edge && successor != end_then)
                    {
                        if (successor < end_then || successor > last)
                            throw std::runtime_error("Branchement ActionScript non structuré.");
                        end_else = successor;
                        --end_then;
                    }
                }
                if (end_else != end_then)
                {
                    try
                    {
                        const auto yes = visit(visit, i + 1, end_then, depth + 1, true);
                        const auto no = visit(visit, end_then + 1, end_else, depth + 1, true);
                        stack.as3_conditional(condition, yes, no);
                        i = end_else - 1;
                        continue;
                    }
                    catch (const std::runtime_error&)
                    {
                        if (expression)
                            throw;
                    }
                }
                if (expression || !stack.empty())
                    throw std::runtime_error("Fusion de pile entre des branches non expressionnelles.");
                out.add(indent + "if (" + condition + ")\n" + indent + "{\n");
                visit(visit, i + 1, end_then, depth + 1, false, false, loop);
                if (end_else != end_then)
                {
                    out.add(indent + "}\n" + indent + "else\n" + indent + "{\n");
                    visit(visit, end_then + 1, end_else, depth + 1, false, false, loop);
                }
                out.add(indent + "}\n");
                i = end_else - 1;
                continue;
            }
            if (op == 0x1B && !expression && !loop)
            {
                const auto value = stack.pop();
                validate_expression(value);
                if (!stack.empty())
                    throw std::runtime_error("Le switch partage des valeurs avec la pile du bloc courant.");
                std::map<std::size_t, std::vector<std::size_t>> cases;
                for (std::size_t label = 0; label < item.args.size(); ++label)
                {
                    const auto target = destination(item.args[label]);
                    if (target <= i || target > last)
                        throw std::runtime_error("Cibles de switch non structurées.");
                    cases[target].push_back(label);
                }
                if (cases.empty())
                    throw std::runtime_error("Switch sans destination.");
                auto join = last;
                const auto final_case = cases.rbegin()->first;
                for (auto region = cases.begin(); std::next(region) != cases.end(); ++region)
                {
                    const auto next = std::next(region)->first;
                    if (instructions[next - 1].op != 0x10)
                        continue;
                    const auto exit = destination(instructions[next - 1].args[0]);
                    if (exit == next)
                        continue;
                    if (exit < final_case || exit > last || (join != last && join != exit))
                        throw std::runtime_error("Sorties de switch non structurées.");
                    join = exit;
                }
                out.add(indent + "switch (int(" + value + "))\n" + indent + "{\n");
                for (auto region = cases.begin(); region != cases.end(); ++region)
                {
                    const auto start = region->first;
                    for (const auto label : region->second)
                        out.add(indent + "    " + (label ? "case " + std::to_string(label - 1) : "default") + ":\n");
                    if (start == join)
                    {
                        out.add(indent + "        break;\n");
                        continue;
                    }
                    const auto next = std::next(region);
                    auto end = next == cases.end() ? join : next->first;
                    bool exits = false;
                    if (instructions[end - 1].op == 0x10)
                    {
                        const auto exit = destination(instructions[end - 1].args[0]);
                        if (exit != end && exit != join)
                            throw std::runtime_error("Saut non structuré dans un cas de switch.");
                        exits = exit == join;
                        --end;
                    }
                    visit(visit, start, end, depth + 2);
                    if (exits)
                        out.add(indent + "        break;\n");
                }
                out.add(indent + "}\n");
                i = join - 1;
                continue;
            }
            switch (op)
            {
            case 0x01: case 0x02: case 0x09: case 0x1D: case 0xEF: case 0xF0: case 0xF1: case 0xF2: case 0xF3: break;
            case 0x03: emit("throw " + stack.pop_as3_value()); break;
            case 0x04:
            {
                const auto field = property();
                stack.pop();
                stack.push("super" + field);
                break;
            }
            case 0x05:
            {
                const auto value = stack.pop_as3_value();
                const auto field = property();
                stack.pop();
                emit("super" + field + " = " + value);
                break;
            }
            case 0x06:
            {
                const auto& uri = at(strings_, arg(0));
                emit("default xml namespace = " + quote(uri, uri.size()));
                break;
            }
            case 0x07: emit("default xml namespace = " + stack.pop()); break;
            case 0x08: emit(writable_local(arg(0)) + " = undefined"); break;
            case 0x20: stack.push("null"); break;
            case 0x21: stack.push("undefined"); break;
            case 0x22: case 0x2D: case 0x2E: case 0x2F: case 0x54:
                stack.push(literal(op, arg(0))); break;
            case 0x2C:
            {
                const auto& text = at(strings_, arg(0));
                if (text.size() > 4096)
                    throw std::runtime_error("Chaîne trop longue pour la reconstruction de cette expression.");
                stack.push(quote(text, text.size()));
                break;
            }
            case 0x24: stack.push(std::to_string(item.args[0])); break;
            case 0x25: stack.push(std::to_string(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(arg(0))))); break;
            case 0x26: stack.push("true"); break;
            case 0x27: stack.push("false"); break;
            case 0x28: stack.push("NaN"); break;
            case 0x29: emit(stack.pop()); break;
            case 0x2A:
                if (i + 1 < last)
                    if (const auto saved = store_register(instructions[i + 1]))
                    {
                        stack.push(assign_local(*saved, i + 1));
                        ++i;
                        break;
                    }
                stack.duplicate();
                break;
            case 0x2B:
            {
                auto right = stack.pop(), left = stack.pop();
                stack.push(std::move(right)); stack.push(std::move(left));
                break;
            }
            case 0x30:
                if (stack.pop() != local(0))
                    throw std::runtime_error("Portée construite dynamiquement.");
                break;
            case 0x41:
            {
                const auto arguments = stack.arguments(arg(0), true, true);
                const auto receiver = stack.pop();
                const auto function = stack.pop();
                if (receiver == global_scope)
                    stack.push('(' + function + ")(" + arguments + ')');
                else
                    stack.push('(' + function + ").call(" + receiver + (arguments.empty() ? "" : ", " + arguments) + ')');
                break;
            }
            case 0x42:
            {
                const auto arguments = stack.arguments(arg(0), true, true);
                stack.push("new " + stack.pop() + '(' + arguments + ')');
                break;
            }
            case 0x45: case 0x46: case 0x4A: case 0x4C: case 0x4E: case 0x4F:
            {
                const auto arguments = stack.as3_list(arg(1), "(", ")");
                const auto field = property();
                auto receiver = stack.pop();
                if (op == 0x45 || op == 0x4E)
                    receiver = "super";
                const auto callee = member(receiver, field);
                auto call = std::format("{}{}{}", op == 0x4A ? "new " : "", callee, arguments);
                if (op == 0x4E || op == 0x4F)
                    emit(std::move(call));
                else
                    stack.push(std::move(call));
                break;
            }
            case 0x47:
                if (!static_owner.empty())
                {
                    if (depth != 1 || i + 1 != instructions.size())
                        throw std::runtime_error("Retour conditionnel de l’initialiseur statique.");
                }
                else if (depth != 1 || i + 1 != last || name(method.result) != "void")
                    emit("return");
                break;
            case 0x48: emit("return " + stack.pop_as3_value()); break;
            case 0x49:
            {
                const auto arguments = stack.arguments(arg(0), true, true);
                stack.pop();
                emit("super(" + arguments + ')');
                break;
            }
            case 0x53:
            {
                auto arguments = stack.arguments(arg(0), true);
                if (arguments == "null")
                    arguments = "*";
                stack.push(stack.pop() + ".<" + arguments + '>');
                break;
            }
            case 0x55:
            {
                if (arg(0) > 128)
                    throw std::runtime_error("Objet littéral trop grand.");
                std::vector<std::string> fields;
                for (unsigned field = 0; field < arg(0); ++field)
                {
                    auto value = stack.pop();
                    const auto key = stack.pop();
                    if (!key.starts_with('"'))
                        throw std::runtime_error("Clé d’objet non littérale.");
                    fields.push_back(key + ": " + value);
                }
                std::string object = "({ ";
                for (auto field = fields.rbegin(); field != fields.rend(); ++field)
                {
                    if (field != fields.rbegin())
                        object += ", ";
                    object += *field;
                    if (object.size() > 4096)
                        throw std::runtime_error("Objet littéral trop grand.");
                }
                stack.push(object + " })");
                break;
            }
            case 0x56: stack.push(stack.as3_list(arg(0), "[", "]")); break;
            case 0x59: stack.push(reference()); break;
            case 0x5B: stack.push("delete " + reference()); break;
            case 0x5D: case 0x5E:
                lexical();
                stack.push(std::string(lexical_scope));
                break;
            case 0x60: stack.push(lexical()); break;
            case 0x61: case 0x68:
            {
                const auto value = stack.pop_as3_value();
                emit(reference() + " = " + value);
                break;
            }
            case 0x62: stack.push(read_local(arg(0))); break;
            case 0x63: assign_local(arg(0), i); break;
            case 0x64: stack.push(std::string(global_scope)); break;
            case 0x65:
                if (own_scope && arg(0) == 0)
                    stack.push(local(0));
                else if (activation && arg(0) == 1)
                    stack.push(std::string(activation_scope));
                else
                    throw std::runtime_error("Objet de portée non reconnu.");
                break;
            case 0x66: stack.push(reference()); break;
            case 0x6A: stack.push("delete " + reference()); break;
            case 0x6C: stack.push(slot_name(stack.pop())); break;
            case 0x6D:
            {
                const auto value = stack.pop_as3_value();
                emit(slot_name(stack.pop()) + " = " + value);
                break;
            }
            case 0x70: case 0x85: stack.push("String(" + stack.pop_as3_value() + ')'); break;
            case 0x73: case 0x83: stack.push("int(" + stack.pop_as3_value() + ')'); break;
            case 0x74: case 0x88: stack.push("uint(" + stack.pop_as3_value() + ')'); break;
            case 0x75: case 0x84: stack.push("Number(" + stack.pop_as3_value() + ')'); break;
            case 0x76: case 0x81: stack.push("Boolean(" + stack.pop_as3_value() + ')'); break;
            case 0x78:
                if (i == first || instructions[i - 1].op != 0x59)
                    throw std::runtime_error("Filtre XML sur une valeur non identifiée.");
                break;
            case 0x80:
            {
                const auto value = stack.pop();
                const auto type = qualified_name(arg(0));
                if (type == "int" || type == "uint" || type == "Number" || type == "Boolean" || type == "String")
                    stack.push(type + '(' + value + ')');
                else
                    stack.push('(' + value + " as " + type + ')');
                break;
            }
            case 0x82: break;
            case 0x86: stack.push(qualified_name(arg(0))); stack.as3_binary("as"); break;
            case 0x87: stack.as3_binary("as"); break;
            case 0x90: case 0xC4: stack.push("-(" + stack.pop() + ')'); break;
            case 0x91: case 0xC0: stack.push("(Number(" + stack.pop() + ") + 1)"); break;
            case 0x93: case 0xC1: stack.push("(Number(" + stack.pop() + ") - 1)"); break;
            case 0x92: case 0x94: case 0xC2: case 0xC3:
            {
                const auto variable = writable_local(arg(0));
                const auto operation = op == 0x92 || op == 0xC2 ? " + 1" : " - 1";
                auto value = std::string(op >= 0xC2 ? "int(" : "Number(") + variable + ')' + operation;
                if (op >= 0xC2)
                    value = "int(" + value + ')';
                emit(variable + " = " + value);
                break;
            }
            case 0x95: stack.push("typeof (" + stack.pop() + ')'); break;
            case 0x96: stack.push("!(" + stack.pop() + ')'); break;
            case 0x97: stack.push("~(" + stack.pop() + ')'); break;
            case 0xA0: case 0xC5: stack.as3_binary("+"); break;
            case 0xA1: case 0xC6: stack.as3_binary("-"); break;
            case 0xA2: case 0xC7: stack.as3_binary("*"); break;
            case 0xA3: stack.as3_binary("/"); break;
            case 0xA4: stack.as3_binary("%"); break;
            case 0xA5: stack.as3_binary("<<"); break;
            case 0xA6: stack.as3_binary(">>"); break;
            case 0xA7: stack.as3_binary(">>>"); break;
            case 0xA8: stack.as3_binary("&"); break;
            case 0xA9: stack.as3_binary("|"); break;
            case 0xAA: stack.as3_binary("^"); break;
            case 0xAB: stack.as3_binary("=="); break;
            case 0xAC: stack.as3_binary("==="); break;
            case 0xAD: stack.as3_binary("<"); break;
            case 0xAE: stack.as3_binary("<="); break;
            case 0xAF: stack.as3_binary(">"); break;
            case 0xB0: stack.as3_binary(">="); break;
            case 0xB1: stack.as3_binary("instanceof"); break;
            case 0xB2: stack.push(qualified_name(arg(0))); stack.as3_binary("is"); break;
            case 0xB3: stack.as3_binary("is"); break;
            case 0xB4: stack.as3_binary("in"); break;
            case 0xD0: case 0xD1: case 0xD2: case 0xD3: stack.push(read_local(op - 0xD0)); break;
            case 0xD4: case 0xD5: case 0xD6: case 0xD7: assign_local(op - 0xD4, i); break;
            default:
                throw std::runtime_error(std::format("Instruction {} à 0x{:04X}. Voir le désassemblage.",
                    opcodes[op].name, item.offset));
            }
        }
        std::string result;
        if (expression)
        {
            result = stack.pop();
            validate_expression(result);
        }
        if (!stack.empty())
            throw std::runtime_error("La pile d’expressions traverse une frontière de bloc.");
        return result;
    };
    statement_block(statement_block, body_start, instructions.size(), 1);
    if (guarded && !emitted_try)
        throw unsupported_try();
    for (const auto& [index, declaration] : local_declarations)
        output.add("    " + declaration + ";\n");
    const auto slots = std::move(slot_declarations).take();
    output.add(slots);
    if (!local_declarations.empty() || !slots.empty())
        output.add("\n");
    output.add(std::move(out).take());
}

std::string Abc::method_signature(unsigned index, std::string_view declared_name, bool constructor) const
{
    const auto& method = at(methods_, index);
    const auto original = method_name(index);
    auto short_name = std::string_view(original);
    if (const auto separator = short_name.rfind("::"); separator != std::string_view::npos)
        short_name.remove_prefix(separator + 2);
    if (!declared_name.empty())
        short_name = declared_name;
    auto signature = "function " + identifier(short_name, std::format("method_{}", index)) + '(';
    for (std::size_t i = 0; i < method.types.size(); ++i)
    {
        if (i)
            signature += ", ";
        signature += method.parameters[i];
        signature += ':' + name(method.types[i]);
        const auto first_default = method.types.size() - method.defaults.size();
        if (i >= first_default)
        {
            const auto [default_index, kind] = method.defaults[i - first_default];
            const auto value = initial_value(default_index, kind);
            signature += " = " + (value.empty() ? std::string("undefined") : value);
        }
        if (signature.size() > 8192)
            throw std::runtime_error("Signature ABC trop longue.");
    }
    if (method.rest)
        signature += method.types.empty() ? "...rest" : ", ...rest";
    return constructor ? signature + ')' : signature + "):" + name(method.result);
}

const Body& Abc::body(unsigned method) const
{
    const auto& index = at(body_by_method_, method);
    if (!index)
        throw std::runtime_error("Cette méthode ABC ne définit pas de corps.");
    return bodies_[*index];
}

std::vector<AbcMethod> Abc::catalog() const
{
    std::vector<AbcMethod> result;
    result.reserve(methods_.size());
    for (unsigned i = 0; i < methods_.size(); ++i)
    {
        auto name = method_name(i);
        std::string owner;
        const auto separator = name.rfind("::");
        if (separator != std::string::npos)
        {
            owner = name.substr(0, separator);
            name.erase(0, separator + 2);
        }
        AbcMethod method{i, std::move(owner), std::move(name), method_signature(i), {}, 0};
        if (body_by_method_[i])
        {
            const auto& info = bodies_[*body_by_method_[i]];
            method.code_size = info.code.size();
            method.handlers = info.exceptions.size();
        }
        result.push_back(std::move(method));
    }
    return result;
}

std::string code_label(std::int64_t address)
{
    return std::format("L{:04X}", address);
}

std::optional<std::string> Abc::source_problem(unsigned method) const
{
    try
    {
        if (class_script(method))
            return {};
        Output discarded;
        source(body(method), discarded);
        return {};
    }
    catch (const std::runtime_error& error) { return error.what(); }
}

const std::vector<unsigned>* Abc::class_script(unsigned method) const
{
    const auto recorded = class_scripts_.find(method);
    if (recorded == class_scripts_.end() || !at(body_by_method_, method))
        return nullptr;
    const auto& info = body(method);
    if (!info.exceptions.empty() || info.code.size() > 32 * 1024)
        return nullptr;
    std::vector<Instruction> instructions;
    Reader reader(info.code);
    try
    {
        while (reader.remaining())
        {
            auto item = decode(reader);
            if (item.op != 0x02 && item.op != 0x09 && (item.op < 0xEF || item.op > 0xF3))
                instructions.push_back(std::move(item));
        }
        std::size_t i = 0;
        const auto accept = [&](unsigned op)
        {
            const bool matches = i < instructions.size() && instructions[i].op == op;
            i += matches;
            return matches;
        };
        const auto previous_arg = [&] { return static_cast<unsigned>(instructions[i - 1].args[0]); };
        if (!accept(0xD0) || !accept(0x30))
            return nullptr;
        for (const auto index : recorded->second)
        {
            if (index >= classes_.size())
                return nullptr;
            const auto destination_is_scope = accept(0x65);
            if (destination_is_scope ? previous_arg() != 0 : !accept(0x5D) || name(previous_arg(), false) != classes_[index].name)
                return nullptr;
            unsigned scopes = 0, last_scope = 0;
            while (i + 1 < instructions.size() && instructions[i].op == 0x60 && instructions[i + 1].op == 0x30)
            {
                last_scope = static_cast<unsigned>(instructions[i].args[0]);
                ++scopes;
                i += 2;
            }
            const auto base = class_bases_[index];
            if (base)
            {
                if (!accept(0x60))
                    return nullptr;
                const auto lookup = previous_arg();
                const auto scoped_lookup = at(multinames_, lookup).kind == 0x09 && last_scope &&
                    name(last_scope) == name(base) && name(lookup, false) == name(base, false);
                if (name(lookup) != name(base) && !scoped_lookup)
                    return nullptr;
            }
            else if (!accept(0x20))
                return nullptr;
            if (!accept(0x58) || previous_arg() != index)
                return nullptr;
            for (unsigned popped = 0; popped < scopes; ++popped)
                if (!accept(0x1D))
                    return nullptr;
            if (!accept(0x68) || name(previous_arg()) != classes_[index].qualified)
                return nullptr;
        }
        return accept(0x47) && i == instructions.size() ? &recorded->second : nullptr;
    }
    catch (const std::runtime_error&) { return nullptr; }
}

std::string Abc::flow_graph(unsigned method) const
{
    const auto& info = body(method);
    std::vector<Instruction> instructions;
    Reader reader(info.code);
    while (reader.remaining())
    {
        if (instructions.size() == 65'536)
            throw std::runtime_error("Méthode trop grande pour le graphe de contrôle.");
        instructions.push_back(decode(reader));
    }
    const auto terminal = [](unsigned op) { return op == 0x03 || op == 0x47 || op == 0x48; };
    const auto boundary = [&](std::int64_t offset)
    {
        const auto found = std::ranges::lower_bound(instructions, offset,
            std::ranges::less{}, [](const Instruction& item) { return static_cast<std::int64_t>(item.offset); });
        return found != instructions.end() && static_cast<std::int64_t>(found->offset) == offset;
    };
    std::set<std::size_t> starts{0, info.code.size()};
    std::set<std::int64_t> unusual;
    const auto add_target = [&](std::int64_t target)
    {
        if (boundary(target))
            starts.insert(static_cast<std::size_t>(target));
        else if (target != static_cast<std::int64_t>(info.code.size()))
            unusual.insert(target);
    };
    for (std::size_t i = 0; i < instructions.size(); ++i)
    {
        const auto& instruction = instructions[i];
        const auto branch = instruction.op >= 0x0C && instruction.op <= 0x1B;
        if ((branch || terminal(instruction.op)) && i + 1 < instructions.size())
            starts.insert(instructions[i + 1].offset);
        if (branch)
            for (const auto target : instruction.args)
                add_target(target);
    }
    for (const auto& handler : info.exceptions)
    {
        add_target(handler.from);
        add_target(handler.to);
        add_target(handler.target);
    }
    if (starts.size() > 4096 || (!info.exceptions.empty() && info.exceptions.size() > 32768 / starts.size()))
        throw std::runtime_error("Le graphe de contrôle comporte trop de blocs ou d’exceptions.");

    Output out;
    out.add("digraph AVM2 {\n  node [shape=box,fontname=\"Consolas\"];\n");
    const auto node = [](std::int64_t offset) { return '"' + code_label(offset) + '"'; };
    for (const auto address : unusual)
        out.add(std::format("  {} [label=\"{} (cible non alignee)\",style=dashed];\n", node(address), code_label(address)));
    const auto edge = [&](std::size_t from, std::int64_t to, std::string_view name, bool exception = false)
    {
        out.add(std::format("  {} -> {} [label=\"{}\"{}];\n", node(from), node(to), name, exception ? ",style=dashed" : ""));
    };
    auto current = instructions.begin();
    for (auto block = starts.begin(); std::next(block) != starts.end(); ++block)
    {
        const auto from = *block, to = *std::next(block);
        out.add("  " + node(from) + " [label=\"");
        while (current != instructions.end() && current->offset < to)
        {
            out.add(std::format("{:04X}  {}\\l", current->offset, opcodes[current->op].name));
            ++current;
        }
        out.add("\"];\n");
        const auto& last = *std::prev(current);
        if (last.op == 0x1B)
        {
            edge(from, last.args[0], "default");
            for (std::size_t i = 1; i < last.args.size(); ++i)
                edge(from, last.args[i], std::to_string(i - 1));
        }
        else if (!terminal(last.op))
        {
            if (last.op >= 0x0C && last.op <= 0x1A)
                edge(from, last.args[0], opcodes[last.op].name);
            if (last.op != 0x10)
                edge(from, to, "suite");
        }
        for (const auto& handler : info.exceptions)
            if (from < handler.to && to > handler.from)
                edge(from, handler.target, "exception", true);
    }
    out.add("  " + node(info.code.size()) + " [label=\"Fin\"];\n}\n");
    return std::move(out).take();
}

std::string Abc::pcode(unsigned method, bool simplify) const
{
    const auto& info = body(method);
    std::vector<Instruction> instructions;
    Reader code(info.code);
    while (code.remaining())
        instructions.push_back(decode(code));
    Output out;
    if (simplify)
    {
        if (!info.exceptions.empty())
            throw std::runtime_error("Le nettoyage n’est pas appliqué aux méthodes avec gestionnaires d’exceptions.");
        const auto changes = simplify_flow(instructions, info.code.size());
        out.add(std::format("; Instructions inaccessibles retirées ({}) · destinations redirigées ({})\n",
            changes.removed, changes.redirected));
        if (changes.constant_branches)
            out.add(std::format("; Branchements à booléen constant simplifiés ({})\n", changes.constant_branches));
    }
    return render_pcode(info, instructions, std::move(out));
}

std::string Abc::render_pcode(const Body& info, std::span<const Instruction> instructions, Output out) const
{
    std::set<std::int64_t> targets;
    for (const auto& item : instructions)
        if (item.op >= 0x0C && item.op <= 0x1B)
            targets.insert(item.args.begin(), item.args.end());
    out.add(std::format("; AVM2 · méthode {}\n.stack {}\n.locals {}\n.scope {} {}\n",
        info.method, info.max_stack, info.locals, info.initial_scope, info.max_scope));
    for (const auto& handler : info.exceptions)
    {
        out.add(std::format(".exception {} {} {} {} {}\n", code_label(handler.from), code_label(handler.to),
            code_label(handler.target), handler.type, handler.variable));
        targets.insert({handler.from, handler.to, handler.target});
    }
    for (const auto address : targets)
    {
        if (address < 0 || address > static_cast<std::int64_t>(info.code.size()))
            throw std::runtime_error("Une destination de saut ou d’exception est hors du corps de méthode.");
        if (address == static_cast<std::int64_t>(info.code.size()))
            continue;
        const auto target = static_cast<std::size_t>(address);
        const auto next = std::ranges::upper_bound(instructions, target, {}, &Instruction::offset);
        const auto base = std::prev(next)->offset;
        if (target != base)
            out.add(std::format(".anchor {} {} {}\n", code_label(address), code_label(base), target - base));
    }
    for (const auto& item : instructions)
    {
        out.add(std::format("{}:  {}", code_label(item.offset), opcodes[item.op].name));
        for (const auto arg : item.args)
            out.add(" " + (item.op >= 0x0C && item.op <= 0x1B ? code_label(arg) : std::to_string(arg)));
        if (!item.args.empty() && item.args[0] >= 0)
        {
            try
            {
                const auto note = resolved_operand(item.op, static_cast<unsigned>(item.args[0]));
                if (!note.empty())
                    out.add("  ; " + note);
            }
            catch (const std::runtime_error&) {}
        }
        out.add("\n");
    }
    out.add(code_label(info.code.size()) + ":\n");
    return std::move(out).take();
}

void Abc::validate(unsigned method, const AssembledMethod& assembled) const
{
    if (assembled.locals <= at(methods_, method).types.size())
        throw std::runtime_error("Le nombre de registres est inférieur à celui exigé par la signature.");
    for (const auto& exception : assembled.exceptions)
    {
        at(multinames_, exception.type);
        at(multinames_, exception.variable);
    }
    Reader input(assembled.code);
    while (input.remaining())
    {
        const auto item = decode(input);
        const auto op = item.op;
        const auto arg = [&](std::size_t index = 0) { return static_cast<unsigned>(item.args.at(index)); };
        if (multiname_operand(op))
            at(multinames_, arg());
        const auto local = [&](unsigned index)
        {
            if (index >= assembled.locals)
                throw std::runtime_error(std::format("Registre hors limites à 0x{:04X}.", item.offset));
        };
        switch (op)
        {
        case 0x06: case 0x2C: case 0xF1: at(strings_, arg()); break;
        case 0x08: case 0x62: case 0x63: case 0x92: case 0x94: case 0xC2: case 0xC3: local(arg()); break;
        case 0x22: at(floats_, arg()); break;
        case 0x2D: at(integers_, arg()); break;
        case 0x2E: at(unsigned_, arg()); break;
        case 0x2F: at(doubles_, arg()); break;
        case 0x31: at(namespaces_, arg()); break;
        case 0x32: local(arg()); local(arg(1)); break;
        case 0x40: case 0x44: at(methods_, arg()); break;
        case 0x54: at(float4s_, arg()); break;
        case 0x58:
            if (arg() >= classes_.size())
                throw std::runtime_error("Indice de classe ABC hors limites.");
            break;
        case 0x5A: at(assembled.exceptions, arg()); break;
        case 0x65:
            if (arg() >= assembled.max_scope)
                throw std::runtime_error("Indice de portée ABC hors limites.");
            break;
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
            local(op & 3);
            break;
        case 0xEF: at(strings_, arg(1)); break;
        default: break;
        }
    }
}

namespace
{
void write_method_code(Bytes& result, const AssembledMethod& body)
{
    put_u30(result, body.stack);
    put_u30(result, body.locals);
    put_u30(result, body.initial_scope);
    put_u30(result, body.max_scope);
    put_u30(result, static_cast<unsigned>(body.code.size()));
    append(result, body.code);
    put_u30(result, static_cast<unsigned>(body.exceptions.size()));
    for (const auto& handler : body.exceptions)
        for (const auto value : {handler.from, handler.to, handler.target, handler.type, handler.variable})
            put_u30(result, value);
}
}

Bytes Abc::replace_method(unsigned method, std::string_view assembly) const
{
    const auto& original = body(method);
    const auto new_body = assemble_method(assembly);
    validate(method, new_body);
    Bytes result;
    append(result, bytes_.first(original.begin));
    write_method_code(result, new_body);
    append(result, bytes_.subspan(original.traits));
    return result;
}

std::size_t Abc::obfuscated_methods() const
{
    return static_cast<std::size_t>(std::ranges::count_if(bodies_, [&](const Body& body)
    {
        const auto& method = methods_[body.method];
        const auto first_local = static_cast<unsigned>(method.types.size() + 1 + (method.rest || method.arguments));
        return has_boolean_prelude(body.code, first_local, body.locals);
    }));
}

AbcDeobfuscation Abc::deobfuscate(std::optional<std::size_t> type) const
{
    std::set<unsigned> selected;
    if (type)
    {
        for (const auto& member : classes_.at(*type).members)
            if (member.method)
                selected.insert(*member.method);
        for (const auto& [method, classes] : class_scripts_)
            if (std::ranges::find(classes, *type) != classes.end())
                selected.insert(method);
    }
    AbcDeobfuscation result;
    std::size_t copied = 0;
    for (const auto& body : bodies_)
    {
        if (type && !selected.contains(body.method))
            continue;
        try
        {
            std::vector<Instruction> instructions;
            Reader code(body.code);
            while (code.remaining())
            {
                if (instructions.size() >= 65'536)
                    throw std::runtime_error("Trop d’instructions pour désobfusquer cette méthode.");
                instructions.push_back(decode(code));
            }
            const auto& method = methods_[body.method];
            const auto first_local = static_cast<unsigned>(method.types.size() + 1 + (method.rest || method.arguments));
            const auto changes = deobfuscate_flow(instructions, body.code.size(), body.exceptions, first_local, body.locals);
            if (!changes.removed && !changes.redirected && !changes.constant_branches && !changes.constants)
                continue;
            const auto assembled = assemble_method(render_pcode(body, instructions, Output{}));
            validate(body.method, assembled);
            if (std::ranges::equal(body.code, assembled.code))
                continue;
            append(result.bytes, bytes_.subspan(copied, body.begin - copied));
            write_method_code(result.bytes, assembled);
            copied = body.traits;
            ++result.report.changed_methods;
            result.report.removed_instructions += changes.removed;
            result.report.constant_branches += changes.constant_branches;
        }
        catch (const std::runtime_error&)
        {
            ++result.report.skipped_methods;
        }
    }
    if (copied)
        append(result.bytes, bytes_.subspan(copied));
    return result;
}

Bytes Abc::replace_string(unsigned index, std::string_view text) const
{
    if (!index || text.size() > max_text_size || (!text.empty() &&
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0)))
        throw std::runtime_error("La constante doit être une chaîne UTF-8 valide d’au plus 2 Mio.");
    const auto [begin, end] = at(string_ranges_, index);
    Bytes result;
    append(result, bytes_.first(begin));
    put_u30(result, static_cast<unsigned>(text.size()));
    if (!text.empty())
        append(result, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    append(result, bytes_.subspan(end));
    return result;
}

void Abc::render_body(const Body& body, bool as_source, Output& out) const
{
    if (!as_source)
    {
        out.add(std::format("Méthode [{}] {} · pile {} · {} registres · {} exceptions\n",
            body.method, method_name(body.method), body.max_stack, body.locals, body.exceptions.size()));
        for (const auto& handler : body.exceptions)
            out.add(std::format("    ; try {:04X}–{:04X} → {:04X} ; type #{} ; nom #{}\n",
                handler.from, handler.to, handler.target, handler.type, handler.variable));
        assembly(body, out);
        out.add("\n");
        return;
    }
    out.add(method_signature(body.method) + "\n{\n");
    try
    {
        Output source_text;
        source(body, source_text);
        out.add(std::move(source_text).take());
    }
    catch (const std::runtime_error& error)
    {
        out.add(std::format("    // Reconstruction indisponible : {}\n", error.what()));
        std::string instructions;
        try { instructions = pcode(body.method); }
        catch (const std::runtime_error&)
        {
            Output raw;
            assembly(body, raw);
            instructions = std::move(raw).take();
        }
        std::string_view lines(instructions);
        while (!lines.empty())
        {
            const auto end = lines.find('\n');
            out.add(std::format("    // {}\n", lines.substr(0, end)));
            lines.remove_prefix(end == std::string_view::npos ? lines.size() : end + 1);
        }
        out.add("\n    throw new Error(\"Corps AVM2 non reconstruit.\");\n");
    }
    out.add("}\n\n");
}

std::string Abc::render(unsigned method, bool as_source) const
{
    Output out;
    if (!at(body_by_method_, method))
        return method_signature(method) + ";\n";
    if (as_source)
        if (const auto* declared = class_script(method))
        {
            Output declarations(16 * 1024 * 1024);
            for (const auto index : *declared)
            {
                declarations.add(render_class(index).text);
                declarations.add("\n");
            }
            return std::move(declarations).take();
        }
    render_body(body(method), as_source, out);
    return std::move(out).take();
}

AbcClassSource Abc::render_class(std::size_t index) const
{
    const auto& type = classes_.at(index);
    SourceNames names{public_symbols_, type.package, {}};
    AbcClassSource result;
    Output out(16 * 1024 * 1024);
    std::size_t line = 1;
    const auto add = [&](std::string_view text)
    {
        out.add(text);
        line += std::ranges::count(text, '\n');
    };
    add(type.package.empty() ? "package\n{\n" : "package " + type.package + "\n{\n");
    add("    " + names.spell(type.declaration) + "\n    {\n");
    std::map<std::size_t, std::string> static_values;
    std::vector<std::size_t> static_order;
    std::string static_code;
    if (const auto initializer = std::ranges::find_if(type.members, [](const auto& member) { return member.declaration.empty(); });
        initializer != type.members.end() && initializer->method && at(body_by_method_, *initializer->method))
    {
        try
        {
            Output statements;
            SourceNames context{public_symbols_, type.package, {}};
            source(body(*initializer->method), statements, &context, type.name);
            static_code = std::move(statements).take();
            const auto prefix = type.name + '.';
            std::string_view remaining(static_code);
            bool declarations_only = true;
            while (!remaining.empty())
            {
                const auto end = remaining.find(";\n");
                const auto statement = remaining.substr(0, end == std::string_view::npos ? end : end + 1);
                const auto equal = statement.find(" = ");
                if (!statement.starts_with("    ") || equal == std::string_view::npos || equal < 5 || !statement.ends_with(';'))
                {
                    declarations_only = false;
                    break;
                }
                auto field = statement.substr(4, equal - 4);
                if (field.starts_with(prefix))
                    field.remove_prefix(prefix.size());
                const auto member = std::ranges::find_if(type.members, [&](const auto& value)
                {
                    return !value.method && value.name == field && value.declaration.find("static ") != std::string::npos &&
                        value.declaration.find(" = ") == std::string::npos;
                });
                const auto position = static_cast<std::size_t>(member - type.members.begin());
                if (member == type.members.end() || static_values.contains(position))
                {
                    declarations_only = false;
                    break;
                }
                static_values.emplace(position, std::string(statement.substr(equal + 3, statement.size() - equal - 4)));
                static_order.push_back(position);
                remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 2);
            }
            if (declarations_only)
                static_code.clear();
            else
            {
                static_values.clear();
                for (const auto& member : type.members)
                    if (!member.method && member.declaration.find("static const ") != std::string::npos &&
                        (static_code.find(type.name + '.' + member.name + " = ") != std::string::npos ||
                         static_code.find("    " + member.name + " = ") != std::string::npos))
                        throw std::runtime_error("Affectations de constantes mêlées aux autres instructions.");
            }
            names.imports.merge(context.imports);
        }
        catch (const std::runtime_error&)
        {
            static_values.clear();
            static_code = "    // Initialiseur statique non reconstruit. Consulter Instructions.\n";
        }
    }
    const auto append_body = [&](std::string_view remaining, std::string_view indent = "        ")
    {
        while (!remaining.empty())
        {
            const auto end = remaining.find('\n');
            if (end != 0)
            {
                add(indent);
                add(remaining.substr(0, end));
            }
            add("\n");
            remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 1);
        }
    };
    bool previous_field = false;
    result.member_lines.resize(type.members.size());
    auto next_static = static_order.begin();
    for (std::size_t slot = 0; slot < type.members.size(); ++slot)
    {
        // Initializers may reference fields initialized earlier in the original bytecode.
        const auto position = static_values.contains(slot) ? *next_static++ : slot;
        const auto& member = type.members[position];
        if (previous_field && member.method)
            add("\n");
        result.member_lines[position] = line;
        previous_field = !member.method;
        if (member.declaration.empty())
        {
            if (!static_code.empty())
                append_body("\n" + static_code, "    ");
            continue;
        }
        add("        " + names.spell(member.declaration));
        if (const auto value = static_values.find(position); value != static_values.end())
        {
            add(" = ");
            std::string_view remaining(value->second);
            for (auto end = remaining.find('\n'); end != std::string_view::npos; end = remaining.find('\n'))
            {
                add(remaining.substr(0, end + 1));
                add("    ");
                remaining.remove_prefix(end + 1);
            }
            add(remaining);
        }
        if (!member.method || !at(body_by_method_, *member.method) || type.interface_type)
        {
            add(";\n");
            continue;
        }
        add("\n        {\n");
        try
        {
            Output body_text;
            SourceNames body_names{public_symbols_, type.package, {}};
            source(body(*member.method), body_text, &body_names);
            names.imports.merge(body_names.imports);
            const auto content = std::move(body_text).take();
            append_body(content);
        }
        catch (const std::runtime_error&)
        {
            add("            // Corps non reconstruit. Consulter la méthode dans Instructions.\n"
                "            throw new Error(\"Corps AVM2 non reconstruit.\");\n");
        }
        add("        }\n\n");
    }
    add("    }\n}\n");
    result.text = std::move(out).take();
    if (!names.imports.empty())
    {
        std::string imports;
        for (const auto& symbol : names.imports)
            imports += "    import " + symbol + ";\n";
        imports += '\n';
        result.text.insert(result.text.find("{\n") + 2, imports);
        for (auto& member_line : result.member_lines)
            member_line += names.imports.size() + 1;
    }
    return result;
}

std::string Abc::render(bool as_source) const
{
    Output out(64 * 1024 * 1024);
    out.add(as_source ? "// ActionScript 3 · reconstruction partielle\n" : "AVM2 · instructions\n");
    out.add(std::format("// ABC {}.{} · {} méthodes, {} corps\n\n", major_, minor_, methods_.size(), bodies_.size()));
    for (const auto& method : bodies_)
        try { render_body(method, as_source, out); }
        catch (const std::runtime_error& error)
        {
            out.add(std::format("// Méthode {} : {}\n\n", method.method, error.what()));
        }
    return std::move(out).take();
}
}

std::string avm2(ByteView bytes, bool source) { return Abc(bytes).render(source); }
}

namespace flash
{
struct AbcModule::State
{
    explicit State(ByteView abc) : bytes(abc.begin(), abc.end()), program(bytes), methods(program.catalog()) {}
    Bytes bytes;
    script::Abc program;
    std::vector<AbcMethod> methods;
};

AbcModule::AbcModule(ByteView abc) : state_(std::make_shared<const State>(abc)) {}
unsigned AbcModule::major_version() const noexcept { return state_->program.major(); }
unsigned AbcModule::minor_version() const noexcept { return state_->program.minor(); }
const std::vector<AbcMethod>& AbcModule::methods() const noexcept { return state_->methods; }
const std::vector<AbcClass>& AbcModule::classes() const noexcept { return state_->program.classes(); }
const std::vector<std::string>& AbcModule::strings() const noexcept { return state_->program.strings(); }
std::string AbcModule::decompile(unsigned method) const { return state_->program.render(method, true); }
AbcClassSource AbcModule::decompile_class(std::size_t index) const { return state_->program.render_class(index); }
std::string AbcModule::disassemble(unsigned method) const { return state_->program.render(method, false); }
std::string AbcModule::pcode(unsigned method, bool simplify) const { return state_->program.pcode(method, simplify); }
std::string AbcModule::flow_graph(unsigned method) const { return state_->program.flow_graph(method); }
std::optional<std::string> AbcModule::source_problem(unsigned method) const { return state_->program.source_problem(method); }
bool AbcModule::declares_classes(unsigned method) const { return state_->program.class_script(method) != nullptr; }
Bytes AbcModule::replace_method(unsigned method, std::string_view pcode) const { return state_->program.replace_method(method, pcode); }
Bytes AbcModule::replace_string(unsigned index, std::string_view utf8) const { return state_->program.replace_string(index, utf8); }
std::size_t AbcModule::obfuscated_methods() const { return state_->program.obfuscated_methods(); }
AbcDeobfuscation AbcModule::deobfuscate(std::optional<std::size_t> type) const { return state_->program.deobfuscate(type); }

ByteView abc_bytecode(const Movie& movie, std::size_t index)
{
    const auto tag = movie.tags().at(index).code;
    Reader input(movie.data(index));
    if (tag == 82)
    {
        input.skip(4);
        input.cstring();
    }
    else if (tag != 72)
        throw std::runtime_error("Sélectionnez un tag DoABC ou DoABC2.");
    return input.take(input.remaining());
}

Movie replace_abc(const Movie& movie, std::size_t index, ByteView abc)
{
    const auto original = abc_bytecode(movie, index);
    const script::Abc validated(abc);
    Bytes bytes;
    const auto payload = movie.data(index);
    append(bytes, payload.first(payload.size() - original.size()));
    append(bytes, abc);
    return movie.replace_tag(index, bytes);
}

std::size_t detect_as3_obfuscation(const Movie& movie)
{
    std::size_t total = 0;
    for (std::size_t tag = 0; tag < movie.tags().size(); ++tag)
        if (movie.tags()[tag].code == 72 || movie.tags()[tag].code == 82)
        {
            try { total += AbcModule(abc_bytecode(movie, tag)).obfuscated_methods(); }
            catch (const std::runtime_error&) {}
        }
    return total;
}

SwfDeobfuscation deobfuscate_as3(const Movie& movie, std::optional<std::size_t> selected, std::optional<std::size_t> type)
{
    if (type && !selected)
        throw std::runtime_error("Sélectionnez le bloc ABC de la classe.");
    if (selected)
        (void)abc_bytecode(movie, *selected);
    SwfDeobfuscation result;
    for (std::size_t tag = 0; tag < movie.tags().size(); ++tag)
    {
        if ((selected && *selected != tag) || (movie.tags()[tag].code != 72 && movie.tags()[tag].code != 82))
            continue;
        try
        {
            const auto abc = AbcModule(abc_bytecode(movie, tag)).deobfuscate(type);
            result.report.skipped_methods += abc.report.skipped_methods;
            if (!abc.bytes.empty())
            {
                auto replacement = replace_abc(result.movie ? *result.movie : movie, tag, abc.bytes);
                result.movie = std::move(replacement);
                result.report.changed_methods += abc.report.changed_methods;
                result.report.removed_instructions += abc.report.removed_instructions;
                result.report.constant_branches += abc.report.constant_branches;
            }
        }
        catch (const std::runtime_error&)
        {
            if (selected)
                throw;
        }
    }
    return result;
}
}
