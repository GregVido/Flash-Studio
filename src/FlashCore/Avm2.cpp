#include "ActionScript.hpp"
#include "Avm2Code.hpp"
#include "Avm2StringEval.hpp"
#include "Abc.hpp"

#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <set>
#include <utility>
#include <variant>
#include <Windows.h>

namespace flash::script
{
namespace
{
struct Multiname
{
    unsigned kind{}, string{}, ns{}, base{}, set{};
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

using CapturedVariables = std::map<unsigned, std::string>;
using DecoderName = std::pair<std::string, std::string>;
using SymbolNames = std::set<std::string, std::less<>>;
using NameReplacements = std::map<std::string, std::string, std::less<>>;

bool obfuscated_identifier(std::string_view word)
{
    if (word.empty())
        return false;
    const auto pattern = word.starts_with("_e_") ? word.substr(3) : word;
    return word.size() >= 96 || std::ranges::any_of(word, [](unsigned char c) { return c < 32 || c == 127; }) ||
        (pattern.find('-') != std::string_view::npos && pattern.find_first_not_of("-_") == std::string_view::npos);
}

struct StringDecoders
{
    std::map<DecoderName, StringDecoder> methods;
    std::map<std::pair<DecoderName, std::vector<std::int32_t>>, std::optional<std::string>> cache;

    const std::optional<std::string>& evaluate(const DecoderName& name, const std::vector<std::int32_t>& arguments)
    {
        static const std::optional<std::string> unresolved;
        const auto candidate = methods.find(name);
        if (candidate == methods.end())
            return unresolved;
        const auto key = std::pair{name, arguments};
        if (const auto cached = cache.find(key); cached != cache.end())
            return cached->second;
        if (cache.size() >= 4096)
            return unresolved;
        return cache.try_emplace(key, candidate->second.evaluate(arguments)).first->second;
    }
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
const T& at(const std::vector<T>& values, std::size_t index)
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

struct DirectCalls
{
    std::vector<bool> skipped, rewritten;
};

DirectCalls direct_receiver_calls(std::span<const Instruction> code, std::span<const Multiname> names,
    const std::set<std::size_t>& entries, unsigned first_local)
{
    struct Call { std::size_t duplicate, receiver, call, kill; unsigned temporary; };
    std::vector<Call> calls;
    std::map<unsigned, unsigned> accesses, covered;
    const auto member_kind = [&](const Instruction& item)
    {
        const auto index = static_cast<std::size_t>(item.args[0]);
        return index < names.size() ? names[index].kind : 0;
    };
    const auto argument_inputs = [&](const Instruction& item, unsigned temporary) -> std::optional<unsigned>
    {
        if (const auto local = load_register(item))
            return *local == temporary ? std::nullopt : std::optional{0u};
        switch (item.op)
        {
        case 0x20: case 0x21: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x5D: case 0x5E: case 0x60:
            return 0;
        case 0x66:
            if (member_kind(item) == 0x07 || member_kind(item) == 0x09)
                return 1;
            break;
        case 0x46:
            if ((member_kind(item) == 0x07 || member_kind(item) == 0x09) && item.args[1] < 128)
                return static_cast<unsigned>(item.args[1]) + 1;
            break;
        default:
            if (item.op >= 0xA0 && item.op <= 0xAA)
                return 2;
        }
        return {};
    };
    for (std::size_t i = 0; i + 7 < code.size(); ++i)
    {
        if (code[i].op != 0x2A)
            continue;
        const auto temporary = store_register(code[i + 1]);
        if (!temporary || *temporary < first_local)
            continue;
        auto property = i + 2;
        if (code[property].op == 0x2C)
            ++property;
        else if (load_register(code[property]) == 0u && code[property + 1].op == 0x66 &&
            (member_kind(code[property + 1]) == 0x07 || member_kind(code[property + 1]) == 0x09))
            property += 2;
        else
            continue;
        const auto receiver = property + 1;
        if (code[property].op != 0x66 || member_kind(code[property]) != 0x1B ||
            load_register(code[receiver]) != temporary)
            continue;
        const auto next_entry = entries.upper_bound(code[i].offset);
        unsigned arguments = 0;
        for (auto j = receiver + 1; j + 2 < code.size() && j - i < 64; ++j)
        {
            if (next_entry != entries.end() && *next_entry <= code[j + 2].offset)
                break;
            if (code[j].op == 0x41 && code[j].args[0] == arguments && code[j + 1].op == 0x29 &&
                code[j + 2].op == 0x08 && code[j + 2].args[0] == *temporary)
            {
                calls.push_back({i, receiver, j, j + 2, *temporary});
                covered[*temporary] += 3;
                i = j + 2;
                break;
            }
            const auto consumed = argument_inputs(code[j], *temporary);
            if (!consumed || *consumed > arguments)
                break;
            arguments = arguments + 1 - *consumed;
        }
    }
    DirectCalls result{std::vector<bool>(code.size()), std::vector<bool>(code.size())};
    if (calls.empty())
        return result;
    for (const auto& item : code)
    {
        if (const auto local = load_register(item))
            ++accesses[*local];
        if (const auto local = store_register(item))
            ++accesses[*local];
        if (item.op == 0x08 || item.op == 0x32 || item.op == 0x92 || item.op == 0x94 || item.op == 0xC2 || item.op == 0xC3)
            for (const auto local : item.args)
                ++accesses[static_cast<unsigned>(local)];
    }
    for (const auto& call : calls)
        if (accesses.at(call.temporary) == covered.at(call.temporary))
        {
            result.skipped[call.duplicate] = result.skipped[call.duplicate + 1] =
                result.skipped[call.receiver] = result.skipped[call.kill] = true;
            result.rewritten[call.call] = true;
        }
    return result;
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
    const auto conversion = code[assignment].op;
    if ((conversion >= 0x73 && conversion <= 0x76) || conversion == 0x80 || conversion == 0x82 || conversion == 0x85)
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
    void collect_obfuscated_names(SymbolNames& symbols, SymbolNames* classes = nullptr) const;
    void collect_renamed_classes(const NameReplacements& symbols, NameReplacements& links) const;
    void suggest_field_names(NameReplacements& suggestions) const;
    Bytes rename_identifiers(const NameReplacements& symbols) const;
    AbcDeobfuscation deobfuscate(std::optional<std::size_t> type, StringDecoders* decoders = nullptr) const;
    void register_string_decoders(StringDecoders& decoders, const std::map<std::string, std::size_t>& class_counts) const;
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
    void source(const Body& body, Output& out, SourceNames* names = nullptr, std::string_view static_owner = {},
        const CapturedVariables* captured = nullptr, unsigned nesting = 0) const;
    void render_body(const Body& body, bool as_source, Output& out) const;
    void validate(unsigned method, const AssembledMethod& assembled, std::size_t added_strings = 0) const;
    std::string render_pcode(const Body& body, std::span<const Instruction> instructions, Output out) const;
    const Body& body(unsigned method) const;
    bool public_name(unsigned index, std::string_view package, std::string_view local) const;
    std::optional<double> numeric_constant(const Instruction& instruction) const;
    std::map<unsigned, StringTable> string_tables(unsigned cinit, const AbcClass& type) const;
    StringDecoder string_decoder(unsigned method, std::map<unsigned, StringTable> tables) const;
    std::size_t rewrite_string_calls(std::vector<Instruction>& code, const Body& body, StringDecoders& decoders,
        std::map<std::string, unsigned, std::less<>>& pool, std::vector<std::string>& added) const;

    ByteView bytes_;
    unsigned major_{}, minor_{};
    std::vector<AbcClass> classes_;
    std::vector<unsigned> class_names_, class_bases_;
    std::map<unsigned, std::vector<unsigned>> class_scripts_;
    std::vector<std::int32_t> integers_;
    std::vector<std::uint32_t> unsigned_, namespaces_;
    std::vector<unsigned> namespace_kinds_{0};
    std::vector<std::vector<unsigned>> namespace_sets_{1};
    std::vector<double> doubles_;
    std::vector<float> floats_;
    std::vector<std::array<float, 4>> float4s_;
    std::vector<std::string> strings_;
    std::set<unsigned> parameter_names_;
    std::map<unsigned, unsigned> property_keys_;
    std::map<unsigned, std::string> field_names_;
    std::size_t strings_begin_{}, strings_end_{};
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
    strings_begin_ = input.position();
    pool(input, strings_, [&]
    {
        const auto begin = input.position();
        auto value = input.string(input.u30());
        string_ranges_.emplace_back(begin, input.position());
        return value;
    });
    strings_end_ = input.position();
    pool(input, namespaces_, [&] { namespace_kinds_.push_back(input.u8()); return input.u30(); });
    const auto sets = bounded_count(input);
    for (unsigned i = 1; i < sets; ++i)
    {
        const auto size = bounded_count(input);
        auto& namespaces = namespace_sets_.emplace_back();
        namespaces.reserve(size);
        for (unsigned j = 0; j < size; ++j)
            namespaces.push_back(input.u30());
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
        case 0x09: case 0x0E: entry.string = input.u30(); entry.set = input.u30(); break;
        case 0x1B: case 0x1C: entry.set = input.u30(); break;
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
            const auto index = flags & 128 ? input.u30() : 0;
            const auto& raw = at(strings_, index);
            if (obfuscated_identifier(raw))
                parameter_names_.insert(index);
            auto parameter = identifier(raw, fallback);
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
        class_names_.push_back(class_name);
        type.name = name(class_name, false);
        type.qualified = name(class_name);
        if (const auto separator = type.qualified.rfind('.'); separator != std::string::npos)
            type.package = type.qualified.substr(0, separator);
        const auto base = input.u30();
        class_bases_.push_back(base);
        if (base)
            type.base = name(base);
        const auto flags = input.u8();
        type.interface_type = (flags & 4) != 0;
        type.sealed = (flags & 1) != 0;
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
            type.interfaces.push_back(name(input.u30()));
            type.declaration += type.interfaces.back();
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

    if (!property_keys_.empty())
    {
        try
        {
            for (const auto& body : bodies_)
            {
                Reader code(body.code);
                while (!property_keys_.empty() && code.remaining())
                {
                    const auto item = decode(code);
                    if (item.op == 0x6D || item.op == 0x6F || item.op == 0x6B)
                    {
                        property_keys_.clear();
                        break;
                    }
                    if (item.op != 0x05 && item.op != 0x61 && item.op != 0x68 && item.op != 0x6A)
                        continue;
                    const auto& target = at(multinames_, static_cast<unsigned>(item.args[0]));
                    if (!target.string)
                    {
                        if (target.kind != 0x1B)
                            property_keys_.clear();
                        else
                            std::erase_if(property_keys_, [&](const auto& key)
                            {
                                const auto ns = at(multinames_, key.first).ns;
                                const auto& namespaces = at(namespace_sets_, target.set);
                                return std::ranges::find(namespaces, ns) != namespaces.end();
                            });
                        continue;
                    }
                    std::erase_if(property_keys_, [&](const auto& key)
                    {
                        return at(strings_, at(multinames_, key.first).string) == at(strings_, target.string);
                    });
                }
            }
        }
        catch (const std::runtime_error&) { property_keys_.clear(); }
    }
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
                const auto& raw = at(strings_, at(multinames_, named).string);
                const auto string_key = kind == 0x01 && public_name(field_type, "", "String") ?
                    identifier(at(strings_, index), "") : std::string{};
                if (!static_member && access(named) == "private " && !string_key.empty())
                    property_keys_.try_emplace(named, index);
                if (obfuscated_identifier(raw))
                {
                    std::string hint;
                    if (!string_key.empty()) hint = string_key + "Name";
                    else if (public_name(field_type, "flash.display", "Loader")) hint = "loader";
                    else if (public_name(field_type, "flash.system", "LoaderContext")) hint = "loaderContext";
                    else if (public_name(field_type, "flash.utils", "ByteArray")) hint = "byteArray";
                    else if (public_name(field_type, "", "Class")) hint = "assetClass";
                    if (!hint.empty())
                        field_names_.try_emplace(named, std::move(hint));
                }
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

namespace
{
void order_between_handlers(std::vector<Instruction>& instructions, const Body& body)
{
    if (body.exceptions.size() < 2 || body.exceptions.size() > 128)
        return;
    const auto position = [&](std::size_t offset)
    {
        if (offset == body.code.size())
            return instructions.size();
        const auto found = std::ranges::lower_bound(instructions, offset, {}, &Instruction::offset);
        return found != instructions.end() && found->offset == offset ?
            static_cast<std::size_t>(found - instructions.begin()) : SIZE_MAX;
    };
    std::set<std::size_t> boundaries{0, instructions.size()};
    for (const auto& entry : body.exceptions)
    {
        const auto last = position(entry.to);
        if (last >= instructions.size() || instructions[last].op != 0x10)
            return;
        boundaries.insert({position(entry.from), last, position(entry.target),
            position(static_cast<std::size_t>(instructions[last].args[0]))});
    }
    if (*boundaries.rbegin() != instructions.size())
        return;
    std::map<std::size_t, std::pair<std::size_t, std::size_t>> incoming;
    for (std::size_t i = 0; i < instructions.size(); ++i)
        if (instructions[i].op >= 0x0C && instructions[i].op <= 0x1B)
            for (const auto address : instructions[i].args)
            {
                const auto target = position(static_cast<std::size_t>(address));
                if (target == SIZE_MAX)
                    return;
                const auto [entry, inserted] = incoming.try_emplace(target, i, i);
                if (!inserted)
                    entry->second.second = i;
            }
    for (auto begin = boundaries.begin(); std::next(begin) != boundaries.end(); ++begin)
    {
        const auto first = *begin, last = *std::next(begin);
        bool external = false;
        for (auto entry = incoming.upper_bound(first); entry != incoming.end() && entry->first < last; ++entry)
            external |= entry->second.first < first || entry->second.second >= last;
        if (external)
            continue;
        std::vector<Instruction> ordered;
        std::vector<bool> visited(last - first);
        std::size_t next = first;
        bool rearranged = false;
        while (next >= first && next < last && !visited[next - first])
        {
            visited[next - first] = true;
            const auto& item = instructions[next];
            if (item.op == 0x10)
            {
                const auto target = position(static_cast<std::size_t>(item.args[0]));
                rearranged |= target != next + 1;
                next = target;
                continue;
            }
            if ((item.op >= 0x0C && item.op <= 0x1B) || item.op == 0x03 || item.op == 0x47 || item.op == 0x48)
                break;
            if (item.op != 0x02 && item.op != 0x09)
                ordered.push_back(item);
            ++next;
        }
        if (!rearranged || next != last || !std::ranges::all_of(visited, [](bool value) { return value; }))
            continue;
        for (auto i = first; i < last; ++i)
        {
            const auto offset = instructions[i].offset;
            instructions[i] = i - first < ordered.size() ? std::move(ordered[i - first]) : Instruction{};
            instructions[i].offset = offset;
            if (i - first >= ordered.size())
                instructions[i].op = 0x02;
        }
    }
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

void Abc::source(const Body& body, Output& output, SourceNames* names, std::string_view static_owner,
    const CapturedVariables* captured, unsigned nesting) const
{
    if (nesting > 8)
        throw std::runtime_error("Trop de fonctions ActionScript imbriquées.");
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
    if (captured)
        for (const auto& [index, name] : *captured)
            if (!used_names.insert(name).second)
                throw std::runtime_error("Paramètre masquant une variable capturée.");
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
    if (body.exceptions.empty())
        simplify_source_flow(instructions, body.code.size(), static_cast<unsigned>(argument_count + 1), body.locals);
    order_between_handlers(instructions, body);
    std::set<std::size_t> control_entries;
    std::set<std::size_t> shared_entries;
    for (const auto& item : instructions)
        if (item.op >= 0x0C && item.op <= 0x1B)
            for (const auto target : item.args)
                if (target >= 0 && !control_entries.insert(static_cast<std::size_t>(target)).second)
                    shared_entries.insert(static_cast<std::size_t>(target));
    for (const auto& exception : body.exceptions)
        control_entries.insert({exception.from, exception.to, exception.target});
    auto direct_calls = direct_receiver_calls(instructions, multinames_, control_entries,
        static_cast<unsigned>(argument_count + 1));
    const bool own_scope = instructions.size() >= 2 && instructions[0].op == 0xD0 && instructions[1].op == 0x30;
    const auto scope_start = own_scope ? std::size_t{2} : 0;
    std::optional<unsigned> activation;
    if (instructions.size() >= scope_start + 4 && instructions[scope_start].op == 0x57 &&
        instructions[scope_start + 1].op == 0x2A && instructions[scope_start + 3].op == 0x30)
    {
        activation = store_register(instructions[scope_start + 2]);
        if (activation && (!*activation || *activation <= argument_count))
            throw std::runtime_error("Portée d’activation partagée avec un argument.");
    }
    const auto body_start = activation ? scope_start + 4 : 0;
    for (std::size_t i = 0; i < instructions.size(); ++i)
    {
        if (direct_calls.skipped[i])
            continue;
        const auto& item = instructions[i];
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
            case 0x40: return "Function";
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
    auto first_statement = body_start;
    if (activation && body.exceptions.empty())
        while (first_statement + 2 < instructions.size())
        {
            const auto& scope = instructions[first_statement];
            const auto& value = instructions[first_statement + 1];
            auto store = first_statement + 2;
            const auto& conversion = instructions[store];
            const auto op = conversion.op;
            const bool coerced = (op >= 0x73 && op <= 0x76) || (op >= 0x80 && op <= 0x85) || op == 0x88;
            if (coerced)
                ++store;
            if (scope.op != 0x65 || scope.args[0] != static_cast<unsigned>(own_scope) ||
                store >= instructions.size() || instructions[store].op != 0x6D)
                break;
            const auto entry = control_entries.lower_bound(scope.offset);
            if (entry != control_entries.end() && *entry <= instructions[store].offset)
                break;
            const auto slot = std::ranges::find(body.slots, static_cast<unsigned>(instructions[store].args[0]), &ActivationSlot::id);
            if (slot == body.slots.end() || slot->initial)
                break;
            const auto type = name(slot->type);
            const auto default_value =
                (type == "*" && value.op == 0x21 && (!coerced || op == 0x82)) ||
                (type == "Number" && value.op == 0x28 && (!coerced || op == 0x75 || op == 0x84)) ||
                (type == "Boolean" && value.op == 0x27 && (!coerced || op == 0x76 || op == 0x81)) ||
                (type == "int" && value.op == 0x24 && value.args[0] == 0 && (!coerced || op == 0x73 || op == 0x83)) ||
                (type == "uint" && value.op == 0x24 && value.args[0] == 0 && (!coerced || op == 0x74 || op == 0x88)) ||
                (type == "String" && value.op == 0x20 && (!coerced || op == 0x85)) ||
                (value.op == 0x20 && op == 0x80 && conversion.args[0] == slot->type &&
                    type != "*" && type != "int" && type != "uint" && type != "Number" && type != "Boolean" &&
                    type != "float" && type != "float4");
            if (!default_value)
                break;
            first_statement = store + 1;
        }
    CapturedVariables lexical_variables = captured ? *captured : CapturedVariables{};
    for (const auto& [index, slot] : variable_slots)
        lexical_variables.insert_or_assign(index, slot_variables.at(slot));
    const auto lexical_variable = [&](unsigned index) -> const std::string*
    {
        if (const auto found = lexical_variables.find(index); found != lexical_variables.end())
            return &found->second;
        const auto& lookup = at(multinames_, index);
        if (lookup.kind != 0x07 && lookup.kind != 0x09)
            return nullptr;
        const std::string* match = nullptr;
        for (const auto& [key, value] : lexical_variables)
        {
            const auto& original = at(multinames_, key);
            if (original.kind != 0x07 || at(strings_, original.string) != at(strings_, lookup.string))
                continue;
            const bool same_namespace = lookup.kind == 0x07 ? original.ns == lookup.ns :
                std::ranges::find(at(namespace_sets_, lookup.set), original.ns) != at(namespace_sets_, lookup.set).end();
            if (!same_namespace)
                continue;
            if (match)
                throw std::runtime_error("Variable capturée ambiguë.");
            match = &value;
        }
        return match;
    };
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
            auto value = stack.pop_as3_value();
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
    const auto shared_return = [&](std::size_t index)
    {
        return body.exceptions.empty() && static_owner.empty() && index < instructions.size() &&
            instructions[index].op == 0x47 && shared_entries.contains(instructions[index].offset);
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
    std::vector<TryRegion> regions;
    const auto unsupported_try = [] { return std::runtime_error("Structure try/catch ou finally non reconnue."); };
    if (!body.exceptions.empty())
    {
        for (std::size_t exception = 0; exception < body.exceptions.size(); ++exception)
        {
            const auto& handler = body.exceptions[exception];
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
            if (!temporary || !*temporary || activation == temporary || instructions[binder].args[0] != static_cast<std::int64_t>(exception) ||
                instructions[binder + 6].args[0] != 1 ||
                instructions[join - 2].op != 0x1D || cleanup.op != 0x08 || cleanup.args[0] != *temporary || used_names.contains(catch_name))
                throw unsupported_try();
            regions.push_back({first, last, binder + setup, join, qualified_name(handler.type), {std::move(catch_name), local(*temporary)}});
        }
        std::ranges::sort(regions, {}, &TryRegion::first);
        for (std::size_t i = 1; i < regions.size(); ++i)
            if (regions[i - 1].join > regions[i].first)
                throw unsupported_try();
    }
    std::set<std::size_t> emitted_try;
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
    unsigned snapshot_count = 0;
    const auto snapshot_name = [&]
    {
        auto snapshot = std::format("savedValue{}", ++snapshot_count);
        while (used_names.contains(snapshot))
            snapshot += '_';
        return snapshot;
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
                const auto line = remaining.substr(0, end);
                if (line.find_first_not_of(" \t") != std::string_view::npos)
                {
                    out.add(indent);
                    out.add(line);
                }
                if (end == std::string_view::npos)
                    break;
                out.add("\n");
                remaining.remove_prefix(end + 1);
            }
            out.add(";\n");
        };
        for (std::size_t i = first; i < last; ++i)
        {
            const auto guarded = std::ranges::lower_bound(regions, i, {}, &TryRegion::first);
            if (guarded != regions.end() && i == guarded->first && last >= guarded->join)
            {
                if (expression || !stack.empty() || !emitted_try.insert(i).second)
                    throw unsupported_try();
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
            if (direct_calls.skipped[i])
                continue;
            const auto& item = instructions[i];
            const auto op = item.op;
            if (i + 3 < last && instructions[i + 3].op == 0x29 &&
                ((instructions[i + 2].op >= 0xA1 && instructions[i + 2].op <= 0xAA) ||
                    (instructions[i + 2].op >= 0xC5 && instructions[i + 2].op <= 0xC7)) &&
                numeric_constant(item) && numeric_constant(instructions[i + 1]))
            {
                const auto next_entry = control_entries.upper_bound(item.offset);
                if (next_entry == control_entries.end() || *next_entry > instructions[i + 3].offset)
                {
                    i += 3;
                    continue;
                }
            }
            const auto arg = [&](std::size_t index) { return static_cast<unsigned>(item.args.at(index)); };
            const auto lexical = [&]
            {
                if (const auto variable = lexical_variable(arg(0)))
                    return *variable;
                const auto& entry = at(multinames_, arg(0));
                if (entry.kind != 0x07 && entry.kind != 0x09)
                    throw std::runtime_error("Accès à une propriété calculée à l’exécution.");
                const auto id = identifier(name(arg(0), false), "");
                if (id.empty())
                    throw std::runtime_error("Nom de propriété non représentable.");
                return entry.ns && at(namespace_kinds_, entry.ns) == 0x16 ? qualified_name(arg(0)) : id;
            };
            if ((op == 0x5D || op == 0x5E) && i + 10 < last && instructions[i + 1].op == 0x2A &&
                instructions[i + 3].op == 0x66 && instructions[i + 3].args[0] == arg(0) &&
                (instructions[i + 4].op != 0x2A || i + 11 < last))
            {
                const bool postfix = instructions[i + 4].op == 0x2A;
                const auto offset = i + static_cast<unsigned>(postfix);
                const auto receiver = store_register(instructions[i + 2]), temporary = store_register(instructions[offset + 5]);
                const auto arithmetic = instructions[offset + 4].op;
                const auto& store = instructions[offset + 8];
                const auto& release_value = instructions[offset + 9];
                const auto& release_receiver = instructions[offset + 10];
                const auto entry = control_entries.upper_bound(item.offset);
                if (receiver && temporary && *receiver != *temporary &&
                    (arithmetic == 0x91 || arithmetic == 0x93 || arithmetic == 0xC0 || arithmetic == 0xC1) &&
                    load_register(instructions[offset + 6]) == receiver && load_register(instructions[offset + 7]) == temporary &&
                    (store.op == 0x61 || store.op == 0x68) && store.args[0] == arg(0) &&
                    release_value.op == 0x08 && release_value.args[0] == *temporary &&
                    release_receiver.op == 0x08 && release_receiver.args[0] == *receiver &&
                    (entry == control_entries.end() || *entry > release_receiver.offset))
                {
                    // AVM2 saves the lexical property receiver in a temporary register for compound updates.
                    const auto property = lexical();
                    auto before = property;
                    if (postfix)
                    {
                        before = snapshot_name();
                        used_names.insert(before);
                        emit("var " + before + ":* = " + property);
                        stack.push(before);
                    }
                    const auto operation = arithmetic == 0x91 || arithmetic == 0xC0 ? " + 1" : " - 1";
                    auto value = "Number(" + before + ')' + operation;
                    if (arithmetic >= 0xC0)
                        value = "int(" + value + ')';
                    emit(property + " = " + value);
                    i = offset + 10;
                    continue;
                }
            }
            const auto property = [&]
            {
                const auto& entry = at(multinames_, arg(0));
                const auto descendant = op == 0x59 || op == 0x5B;
                const std::string separator = descendant ? ".." : ".";
                if (entry.kind == 0x1B)
                {
                    if (descendant)
                        throw std::runtime_error("Nom de descendant XML calculé à l’exécution.");
                    const auto value = stack.pop();
                    if (value.size() > 2 && value.front() == '"' && value.back() == '"')
                        if (const auto key = identifier(std::string_view(value).substr(1, value.size() - 2), ""); !key.empty())
                            return '.' + key;
                    return '[' + value + ']';
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
            const auto read_property = [&]
            {
                const auto field = property();
                const auto owner = stack.pop();
                if (owner == "this")
                    if (const auto key = property_keys_.find(arg(0)); key != property_keys_.end())
                        return quote(at(strings_, key->second));
                return member(owner, field);
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
                if (i + 1 == last && shared_return(target))
                {
                    emit("return");
                    continue;
                }
                if (loop && (target == loop->next || target == loop->exit))
                {
                    emit(target == loop->next ? "continue" : "break");
                    while (i + 1 < last && !control_entries.contains(instructions[i + 1].offset))
                        ++i;
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
                                    std::string type;
                                    switch (enumeration->coercion)
                                    {
                                    case 0x73: type = "int"; break;
                                    case 0x74: type = "uint"; break;
                                    case 0x75: type = "Number"; break;
                                    case 0x76: type = "Boolean"; break;
                                    case 0x80: type = qualified_name(enumeration->coercion_type); break;
                                    }
                                    const auto typed = !type.empty();
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
                            auto body_end = target;
                            const auto& tail = instructions[target - 1];
                            if (target > i + 2 && (tail.op == 0xC2 || tail.op == 0xC3) && control_entries.contains(tail.offset))
                            {
                                const auto counter = local(static_cast<unsigned>(tail.args[0]));
                                const auto delta = tail.op == 0xC2 ? " + 1" : " - 1";
                                out.add(indent + "for (; " + condition + "; " + counter + " = int(" + counter + delta + "))\n");
                                --body_end;
                            }
                            else
                                out.add(indent + "while (" + condition + ")\n");
                            out.add(indent + "{\n");
                            visit(visit, i + 1, body_end, depth + 1, false, false, Loop{body_end, test + 1});
                            out.add(indent + "}\n");
                            i = test;
                            continue;
                        }
                    }
                }
            }
            if (op >= 0x0C && op <= 0x1A && op != 0x10)
            {
                auto end_then = destination(item.args[0]);
                const bool guard = !expression && end_then > i && shared_return(end_then) &&
                    stack.size() == (op == 0x11 || op == 0x12 ? 1 : 2);
                if (guard)
                {
                    const auto condition = branch_condition(op, stack, true);
                    out.add(indent + "if (" + condition + ")\n" + indent + "{\n" + indent + "    return;\n" + indent + "}\n");
                    if (depth == 1 && i + 1 < last)
                        out.add("\n");
                    continue;
                }
                std::string value;
                auto condition = branch_condition(op, stack, false);
                if (op == 0x11)
                    value = condition.substr(2, condition.size() - 3);
                else if (op == 0x12)
                    value = condition;
                if (loop && !expression && stack.empty() && (end_then == loop->next || end_then == loop->exit) && end_then > last)
                {
                    out.add(indent + "if (!(" + condition + "))\n" + indent + "{\n" + indent + "    " +
                        (end_then == loop->next ? "continue;\n" : "break;\n") + indent + "}\n");
                    continue;
                }
                if (end_then <= i || end_then > last)
                    throw std::runtime_error(std::format("Fusion ou saut conditionnel complexe à 0x{:04X} vers 0x{:04X}.",
                        item.offset, item.args[0]));
                if ((op == 0x11 || op == 0x12) && !stack.empty() && end_then > i + 2 &&
                    instructions[i + 1].op == 0x29)
                {
                    const auto retained = stack.as3_top_value();
                    const auto coerces_duplicate = i >= 2 && instructions[i - 2].op == 0x2A &&
                        (instructions[i - 1].op == 0x76 || instructions[i - 1].op == 0x81) &&
                        value == std::format("Boolean({})", retained);
                    if (retained != value && !coerces_duplicate)
                        throw std::runtime_error(std::format("Valeurs différentes sur les branches du court-circuit à 0x{:04X}.",
                            item.offset));
                    const auto right = visit(visit, i + 2, end_then, depth + 1, true);
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
                    if (!loop_edge && successor != end_then && !shared_return(successor))
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
                if (depth == 1 && end_else < last)
                    out.add("\n");
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
            case 0x08:
            {
                const auto variable = writable_local(arg(0));
                if (!stack.empty())
                {
                    const auto snapshot = snapshot_name();
                    if (stack.replace_value(variable, snapshot))
                    {
                        used_names.insert(snapshot);
                        emit("var " + snapshot + ":* = " + variable);
                    }
                }
                emit(variable + " = undefined");
                break;
            }
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
                if (i > first && i + 2 < last)
                {
                    const auto numeric_conversion = [](unsigned code)
                    {
                        return (code >= 0x73 && code <= 0x75) || code == 0x83 || code == 0x84 || code == 0x88;
                    };
                    const auto operation = instructions[i + 1].op;
                    const bool update = operation == 0x91 || operation == 0x93 || operation == 0xC0 || operation == 0xC1;
                    const auto load = numeric_conversion(instructions[i - 1].op) && i > first + 1 ? i - 2 : i - 1;
                    const auto store = i + 2 + numeric_conversion(instructions[i + 2].op);
                    if (const auto reg = load_register(instructions[load]); update && reg && store < last &&
                        store_register(instructions[store]) == reg)
                    {
                        const auto entry = control_entries.lower_bound(item.offset);
                        if (entry == control_entries.end() || *entry > instructions[store].offset)
                        {
                            const auto snapshot = snapshot_name();
                            used_names.insert(snapshot);
                            // The retained operand still contains the value from before the write.
                            emit("var " + snapshot + ":* = " + stack.pop_as3_value());
                            stack.push(snapshot);
                        }
                    }
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
            case 0x40:
            {
                const auto index = arg(0);
                Output closure;
                try { source(this->body(index), closure, names, {}, &lexical_variables, nesting + 1); }
                catch (const std::runtime_error& error)
                {
                    throw std::runtime_error(std::format("Fonction imbriquée {} · {}", index, error.what()));
                }
                const auto signature = method_signature(index);
                const auto parameters = signature.substr(signature.find('('));
                stack.push("(function" + (names ? names->spell(parameters) : parameters) + "\n{\n" +
                    std::move(closure).take() + "})");
                break;
            }
            case 0x41:
            {
                const auto arguments = stack.arguments(arg(0), true, true);
                if (direct_calls.rewritten[i])
                {
                    stack.push(stack.pop() + '(' + arguments + ')');
                    break;
                }
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
                auto constructor = stack.pop();
                if (constructor.find('(') != std::string::npos)
                    constructor = '(' + constructor + ')';
                stack.push("new " + constructor + '(' + arguments + ')');
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
                else if (depth != 1 || i + 1 != last ||
                    (name(method.result) != "void" && !method.label.ends_with("::<init>")))
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
                    auto key = stack.pop();
                    if (key.starts_with("String(") && key.ends_with(')'))
                        key = key.substr(7, key.size() - 8);
                    const auto digits = std::string_view(key).substr(key.starts_with('-') ? 1 : 0);
                    if (!digits.empty() && std::ranges::all_of(digits, [](char digit) { return digit >= '0' && digit <= '9'; }))
                        key = quote(key == "-0" ? "0" : key);
                    if (!key.starts_with('"'))
                        throw std::runtime_error("Clé d’objet non littérale.");
                    fields.push_back(key + ": " + value);
                }
                std::size_t width = 0;
                for (const auto& field : fields)
                    width += field.size() + 2;
                if (width > 4096)
                    throw std::runtime_error("Objet littéral trop grand.");
                const bool multiline = fields.size() > 1 && width > 100;
                std::string object = multiline ? "({\n    " : "({ ";
                for (auto field = fields.rbegin(); field != fields.rend(); ++field)
                {
                    if (field != fields.rbegin())
                        object += multiline ? ",\n    " : ", ";
                    object += *field;
                }
                stack.push(object + (multiline ? "\n})" : " })"));
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
                else if (activation && arg(0) == static_cast<unsigned>(own_scope))
                    stack.push(std::string(activation_scope));
                else
                    throw std::runtime_error("Objet de portée non reconnu.");
                break;
            case 0x66: stack.push(read_property()); break;
            case 0x6A: stack.push("delete " + reference()); break;
            case 0x6C: stack.push(slot_name(stack.pop())); break;
            case 0x6D:
            {
                const auto value = stack.pop_as3_value();
                emit(slot_name(stack.pop()) + " = " + value);
                break;
            }
            case 0x70: case 0x85:
            {
                const auto value = stack.pop();
                stack.push(value.starts_with('"') && value.ends_with('"') ? value : "String(" + value + ')');
                break;
            }
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
                if (i > first && !control_entries.contains(item.offset))
                {
                    const auto& prior = instructions[i - 1];
                    if ((prior.op == 0x4A && prior.args[0] == arg(0)) ||
                        (prior.op == 0x40 && public_name(arg(0), "", "Function")))
                        break;
                }
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
    statement_block(statement_block, first_statement, instructions.size(), 1);
    if (emitted_try.size() != regions.size())
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
        const auto getlex = [&]() -> std::optional<unsigned>
        {
            if (accept(0x60))
                return previous_arg();
            if (i + 1 < instructions.size() && instructions[i].op == 0x5D && instructions[i + 1].op == 0x66 &&
                instructions[i].args == instructions[i + 1].args)
            {
                i += 2;
                return previous_arg();
            }
            return {};
        };
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
            while (i < instructions.size())
            {
                const auto start = i;
                const auto lookup = getlex();
                if (!lookup || !accept(0x30))
                {
                    i = start;
                    break;
                }
                last_scope = *lookup;
                ++scopes;
            }
            const auto base = class_bases_[index];
            if (base)
            {
                const auto binding = getlex();
                if (!binding)
                    return nullptr;
                const auto lookup = *binding;
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

void Abc::validate(unsigned method, const AssembledMethod& assembled, std::size_t added_strings) const
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
        case 0x2C:
            if (arg() >= strings_.size() + added_strings)
                throw std::runtime_error("Indice de chaîne ABC invalide.");
            break;
        case 0x06: case 0xF1: at(strings_, arg()); break;
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
        return has_boolean_prelude(body.code, first_local, body.locals) || has_discarded_closure_prelude(body.code);
    }));
}

void Abc::collect_obfuscated_names(SymbolNames& symbols, SymbolNames* classes) const
{
    const auto collect = [this](SymbolNames& names, unsigned index)
    {
        if (index < strings_.size() && obfuscated_identifier(strings_[index]))
            names.insert(strings_[index]);
    };
    for (const auto& entry : multinames_)
        if (entry.kind == 0x07 || entry.kind == 0x09 || entry.kind == 0x0F)
            collect(symbols, entry.string);
    for (const auto index : parameter_names_)
        collect(symbols, index);
    if (classes)
        for (const auto index : class_names_)
            collect(*classes, at(multinames_, index).string);
}

void Abc::collect_renamed_classes(const NameReplacements& symbols, NameReplacements& links) const
{
    for (const auto index : class_names_)
    {
        const auto& entry = at(multinames_, index);
        const auto& name = at(strings_, entry.string);
        const auto& package = at(strings_, at(namespaces_, entry.ns));
        if (const auto found = symbols.find(name); found != symbols.end())
        {
            if (package.empty())
                links.emplace(name, found->second);
            else
                for (const auto separator : {".", "::"})
                    links.emplace(package + separator + name, package + separator + found->second);
        }
    }
}

void Abc::suggest_field_names(NameReplacements& suggestions) const
{
    for (const auto& [id, hint] : field_names_)
    {
        const auto& name = at(strings_, at(multinames_, id).string);
        const auto [known, inserted] = suggestions.try_emplace(name, hint);
        if (!inserted && known->second != hint)
            known->second.clear();
    }
}

Bytes Abc::rename_identifiers(const NameReplacements& symbols) const
{
    Bytes result;
    std::size_t copied = 0;
    for (unsigned i = 1; i < strings_.size(); ++i)
        if (const auto alias = symbols.find(strings_[i]); alias != symbols.end())
        {
            const auto [begin, end] = string_ranges_[i];
            append(result, bytes_.subspan(copied, begin - copied));
            put_u30(result, static_cast<unsigned>(alias->second.size()));
            append(result, {reinterpret_cast<const std::uint8_t*>(alias->second.data()), alias->second.size()});
            copied = end;
        }
    if (copied)
        append(result, bytes_.subspan(copied));
    return result;
}

bool Abc::public_name(unsigned index, std::string_view package, std::string_view local) const
{
    const auto& entry = at(multinames_, index);
    return entry.kind == 0x07 && entry.ns && at(namespace_kinds_, entry.ns) == 0x16 &&
        at(strings_, at(namespaces_, entry.ns)) == package && at(strings_, entry.string) == local;
}

std::optional<double> Abc::numeric_constant(const Instruction& item) const
{
    if (item.args.empty())
        return {};
    const auto operand = static_cast<unsigned>(item.args[0]);
    switch (item.op)
    {
    case 0x24: return double(item.args[0]);
    case 0x25: return double(std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(operand)));
    case 0x2D: return double(at(integers_, operand));
    case 0x2E: return double(at(unsigned_, operand));
    case 0x2F: return at(doubles_, operand);
    default: return {};
    }
}

namespace
{
std::vector<Instruction> decoder_code(const Body& body, std::size_t limit, unsigned first_local = 1)
{
    if (!body.exceptions.empty() || !body.slots.empty() || body.code.size() > limit)
        throw std::runtime_error("Corps non admissible pour l’évaluation des chaînes.");
    Reader reader(body.code);
    std::vector<Instruction> result;
    while (reader.remaining())
        result.push_back(decode(reader));
    if (has_boolean_prelude(body.code, first_local, body.locals))
        deobfuscate_flow(result, body.code.size(), {}, first_local, body.locals);
    return result;
}

bool debugger_instruction(unsigned op)
{
    return op == 0x02 || op == 0x09 || op == 0xEF || op == 0xF0 || op == 0xF1;
}
}

std::map<unsigned, StringTable> Abc::string_tables(unsigned cinit, const AbcClass& type) const
{
    const auto unsupported = [] { return std::runtime_error("Initialiseur de tables non reconnu."); };
    auto code = decoder_code(body(cinit), 256 * 1024);
    std::erase_if(code, [](const auto& item) { return debugger_instruction(item.op); });
    std::size_t cursor = 0;
    const auto take = [&](unsigned op) -> const Instruction&
    {
        const auto& item = at(code, cursor++);
        if (item.op != op)
            throw unsupported();
        return item;
    };
    if (!code.empty() && code.front().op == 0xD0)
    {
        take(0xD0);
        take(0x30);
    }
    std::map<unsigned, StringTable> tables;
    while (cursor < code.size() && code[cursor].op != 0x47)
    {
        const auto field = static_cast<unsigned>(take(0x5E).args[0]);
        const auto& multiname = at(multinames_, field);
        if (multiname.kind != 0x07 || !multiname.ns || at(namespace_kinds_, multiname.ns) != 0x17 ||
            at(strings_, at(namespaces_, multiname.ns)) != type.package)
            throw unsupported();
        const auto declaration = "internal static const " + at(strings_, multiname.string) + ":Array";
        if (std::ranges::none_of(type.members, [&](const auto& member) { return member.declaration == declaration; }))
            throw unsupported();
        const auto array = static_cast<unsigned>(take(0x5D).args[0]);
        if (!public_name(array, "", "Array"))
            throw unsupported();
        StringTable table;
        std::vector<double> values;
        for (; cursor < code.size(); ++cursor)
        {
            const auto& item = code[cursor];
            if (const auto value = numeric_constant(item); value && std::isfinite(*value) && values.size() < 4096)
                values.push_back(*value);
            else if (item.op == 0x56 && item.args[0] == static_cast<std::int64_t>(values.size()) && table.size() < 8192)
                table.emplace_back(std::exchange(values, {}));
            else
                break;
        }
        const auto& construct = take(0x4A);
        if (construct.args[0] != array || construct.args[1] != static_cast<std::int64_t>(table.size()) ||
            !values.empty() || table.size() < 2 || take(0x68).args[0] != field ||
            !tables.try_emplace(field, std::move(table)).second)
            throw unsupported();
    }
    take(0x47);
    if (cursor != code.size())
        throw unsupported();
    return tables;
}

StringDecoder Abc::string_decoder(unsigned id, std::map<unsigned, StringTable> tables) const
{
    const auto unsupported = [] { return std::runtime_error("Instruction non admise pour l’évaluation des chaînes."); };
    const auto& info = body(id);
    const auto parameters = static_cast<unsigned>(methods_[id].types.size());
    auto code = decoder_code(info, 8192, parameters + 1);
    std::vector<unsigned> table_names;
    StringDecoder result;
    result.locals = info.locals;
    result.parameters = parameters;
    for (auto& [field, values] : tables)
    {
        table_names.push_back(field);
        result.tables.push_back(std::move(values));
    }

    const auto builtin = [&](unsigned index, std::string_view local)
    {
        const auto& entry = at(multinames_, index);
        if (entry.kind == 0x09 && at(strings_, entry.string) == local)
            return std::ranges::any_of(at(namespace_sets_, entry.set), [&](unsigned ns)
            {
                return ns && at(namespace_kinds_, ns) == 0x16 && at(strings_, at(namespaces_, ns)).empty();
            });
        return public_name(index, "", local) || (entry.kind == 0x07 && entry.ns &&
            at(namespace_kinds_, entry.ns) == 0x08 && at(strings_, entry.string) == local &&
            at(strings_, at(namespaces_, entry.ns)) == "http://adobe.com/AS3/2006/builtin");
    };
    const auto lookup = [&](std::int64_t target)
    {
        const auto offset = static_cast<std::size_t>(target);
        const auto found = std::ranges::lower_bound(code, offset, {}, &Instruction::offset);
        if (found == code.end() || found->offset != offset)
            throw unsupported();
        const auto index = static_cast<std::size_t>(found - code.begin());
        if (index < 2 && code.size() >= 2 && code[0].op == 0xD0 && code[1].op == 0x30)
            throw unsupported();
        return index;
    };
    std::set<std::int64_t> targets;
    for (const auto& item : code)
        if (item.op >= 0x0C && item.op <= 0x1B)
            targets.insert(item.args.begin(), item.args.end());
    for (std::size_t i = 1; i < code.size(); ++i)
        if (code[i - 1].op == 0x5D && code[i].op == 0x66 && code[i - 1].args == code[i].args &&
            !targets.contains(static_cast<std::int64_t>(code[i].offset)))
        {
            code[i - 1].op = 0x60;
            code[i].op = 0x02;
            code[i].args.clear();
        }
    for (std::size_t i = 0; i < code.size(); ++i)
    {
        const auto& item = code[i];
        StringInstruction next{};
        next.op = item.op;
        const auto operand = item.args.empty() ? 0 : static_cast<unsigned>(item.args[0]);
        if (debugger_instruction(item.op) ||
            (i < 2 && code.size() >= 2 && code[0].op == 0xD0 && code[1].op == 0x30))
            next.op = 0x02;
        else if (const auto reg = load_register(item); reg && *reg && *reg < info.locals)
        {
            next.op = 0x62;
            next.index = *reg;
        }
        else if (const auto written = store_register(item); written && *written && *written < info.locals)
        {
            next.op = 0x63;
            next.index = *written;
        }
        else if (const auto value = numeric_constant(item))
            next.number = *value;
        else
        {
            switch (item.op)
            {
            case 0x10: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
                next.index = lookup(item.args[0]); break;
            case 0x20: case 0x21: case 0x48: case 0x73: case 0x82: case 0x83: case 0x85:
            case 0x93: case 0xA0: case 0xA1: case 0xA5: case 0xA7: case 0xA8: case 0xAA: case 0xC0:
                break;
            case 0x2C: next.text = string_units(at(strings_, operand)); break;
            case 0x5D:
                if (!public_name(operand, "", "int"))
                    throw unsupported();
                next.member = StringOperand::integer;
                break;
            case 0x60:
                if (builtin(operand, "String"))
                    next.member = StringOperand::string;
                else
                {
                    const auto table = std::ranges::find(table_names, operand);
                    if (table == table_names.end())
                        throw unsupported();
                    next.member = StringOperand::table;
                    next.index = static_cast<std::size_t>(table - table_names.begin());
                }
                break;
            case 0x66:
                if (at(multinames_, operand).kind == 0x1B)
                    next.member = StringOperand::index;
                else if (builtin(operand, "length"))
                    next.member = StringOperand::length;
                else
                    throw unsupported();
                break;
            case 0x46:
                next.count = static_cast<unsigned>(item.args[1]);
                if (next.count == 1 && public_name(operand, "", "int"))
                    next.member = StringOperand::integer;
                else if (next.count >= 1 && next.count <= 32 && builtin(operand, "fromCharCode"))
                    next.member = StringOperand::from_char_code;
                else if (next.count == 1 && builtin(operand, "charCodeAt"))
                    next.member = StringOperand::char_code_at;
                else if (next.count == 2 && builtin(operand, "substring"))
                    next.member = StringOperand::substring;
                else
                    throw unsupported();
                break;
            default: throw unsupported();
            }
        }
        result.code.push_back(std::move(next));
    }
    return result;
}

void Abc::register_string_decoders(StringDecoders& decoders, const std::map<std::string, std::size_t>& class_counts) const
{
    for (std::size_t index = 0; index < classes_.size(); ++index)
    {
        const auto& type = classes_[index];
        if (type.interface_type || type.members.size() < 3 || type.members.size() > 10 || class_counts.at(type.qualified) != 1 ||
            !public_name(class_names_[index], type.package, type.name) || !public_name(class_bases_[index], "", "Object"))
            continue;
        try
        {
            const auto script = std::ranges::find_if(class_scripts_, [&](const auto& item)
            {
                return item.second.size() == 1 && item.second.front() == index;
            });
            if (script == class_scripts_.end() || !class_script(script->first))
                continue;
            const auto ctor = std::ranges::find(type.members, type.name, &AbcMember::name);
            const auto cinit = std::ranges::find_if(type.members, [](const auto& member) { return member.declaration.empty(); });
            if (ctor == type.members.end() || !ctor->method || cinit == type.members.end() || !cinit->method)
                continue;
            const auto method = std::ranges::find_if(type.members, [](const auto& member)
            {
                return member.method && member.declaration.starts_with("public static function ");
            });
            if (method == type.members.end() ||
                std::ranges::count_if(type.members, [](const auto& member) { return member.method.has_value(); }) != 3)
                continue;
            const auto& signature = at(methods_, *method->method);
            if (signature.types.empty() || signature.types.size() > 4 || signature.arguments || signature.rest ||
                !public_name(signature.result, "", "String") ||
                std::ranges::any_of(signature.types, [&](auto parameter) { return !public_name(parameter, "", "int"); }))
                continue;
            auto code = decoder_code(body(*ctor->method), 128);
            std::erase_if(code, [](const auto& item) { return debugger_instruction(item.op); });
            constexpr std::array empty_constructor{0xD0u, 0x30u, 0xD0u, 0x49u, 0x47u};
            if (!std::ranges::equal(code, empty_constructor, {}, &Instruction::op) || code[3].args[0] != 0)
                continue;
            auto tables = string_tables(*cinit->method, type);
            if (tables.size() + 3 != type.members.size())
                continue;
            if (!tables.empty() && std::ranges::any_of(class_counts, [&](const auto& item)
                {
                    const auto separator = item.first.rfind('.');
                    const auto package = separator == std::string::npos ? std::string_view{} :
                        std::string_view(item.first).substr(0, separator);
                    return item.first != type.qualified && package == type.package;
                }))
                continue;
            decoders.methods.emplace(DecoderName{type.qualified, method->name}, string_decoder(*method->method, std::move(tables)));
        }
        catch (const std::runtime_error&) {}
    }
}

std::size_t Abc::rewrite_string_calls(std::vector<Instruction>& code, const Body& body, StringDecoders& decoders,
    std::map<std::string, unsigned, std::less<>>& pool, std::vector<std::string>& added) const
{
    std::set<std::int64_t> entries;
    for (const auto& item : code)
        if (item.op >= 0x0C && item.op <= 0x1B)
            entries.insert(item.args.begin(), item.args.end());
    for (const auto& entry : body.exceptions)
        entries.insert({entry.from, entry.to, entry.target});

    std::size_t resolved = 0;
    for (std::size_t first = 0; first + 2 < code.size(); ++first)
    {
        const auto& receiver = code[first];
        const auto loaded = receiver.op == 0x60;
        const auto retrieved = receiver.op == 0x5D && code[first + 1].op == 0x66 && receiver.args == code[first + 1].args;
        if (!loaded && !retrieved)
            continue;
        const auto& owner = at(multinames_, static_cast<unsigned>(receiver.args[0]));
        if (owner.kind != 0x07 || !owner.ns || at(namespace_kinds_, owner.ns) != 0x16)
            continue;
        auto last = first + (loaded ? 1 : 2);
        std::vector<std::int32_t> arguments;
        for (; last < code.size() && arguments.size() < 4; ++last)
        {
            const auto arg = numeric_constant(code[last]);
            if (!arg || !(*arg >= INT32_MIN && *arg <= INT32_MAX && std::trunc(*arg) == *arg))
                break;
            arguments.push_back(static_cast<std::int32_t>(*arg));
        }
        if (arguments.empty() || last == code.size() || code[last].op != 0x46 ||
            code[last].args[1] != static_cast<std::int64_t>(arguments.size()))
            continue;
        const auto method = static_cast<unsigned>(code[last].args[0]);
        const auto method_name = name(method, false);
        if (!public_name(method, "", method_name))
            continue;
        const auto protected_entry = entries.upper_bound(static_cast<std::int64_t>(receiver.offset));
        const auto after_call = last + 1 < code.size() ? code[last + 1].offset : body.code.size();
        if (protected_entry != entries.end() && *protected_entry < static_cast<std::int64_t>(after_call))
            continue;
        const auto& package = at(strings_, at(namespaces_, owner.ns));
        const auto& local = at(strings_, owner.string);
        const auto& string = decoders.evaluate({package.empty() ? local : package + '.' + local, method_name}, arguments);
        if (!string || string->size() > 4096)
            continue;
        auto constant = pool.find(*string);
        if (constant == pool.end())
        {
            if (strings_.size() + added.size() >= 100'000)
                continue;
            constant = pool.emplace(*string, static_cast<unsigned>(strings_.size() + added.size())).first;
            added.push_back(*string);
        }
        code[first] = {receiver.offset, 0x2C, {constant->second}};
        for (auto i = first + 1; i <= last; ++i)
            code[i] = {code[i].offset, 0x02, {}};
        if (last + 1 < code.size() && (code[last + 1].op == 0x70 || code[last + 1].op == 0x85) &&
            !entries.contains(static_cast<std::int64_t>(code[last + 1].offset)))
            code[last + 1] = {code[last + 1].offset, 0x02, {}};
        first = last;
        ++resolved;
    }
    if (resolved)
        std::erase_if(code, [&](const Instruction& item)
        {
            return item.op == 0x02 && !entries.contains(static_cast<std::int64_t>(item.offset));
        });
    return resolved;
}

AbcDeobfuscation Abc::deobfuscate(std::optional<std::size_t> type, StringDecoders* decoders) const
{
    const auto read_code = [](const Body& body)
    {
        std::vector<Instruction> instructions;
        Reader code(body.code);
        while (code.remaining())
        {
            if (instructions.size() >= 65'536)
                throw std::runtime_error("Trop d’instructions pour désobfusquer cette méthode.");
            instructions.push_back(decode(code));
        }
        return instructions;
    };
    const auto local_start = [this](unsigned id)
    {
        const auto& method = methods_[id];
        return static_cast<unsigned>(method.types.size() + 1 + (method.rest || method.arguments));
    };
    std::set<unsigned> selected;
    if (type)
    {
        for (const auto& member : classes_.at(*type).members)
            if (member.method)
                selected.insert(*member.method);
        for (const auto& [method, classes] : class_scripts_)
            if (std::ranges::find(classes, *type) != classes.end())
                selected.insert(method);
        std::vector<unsigned> pending(selected.begin(), selected.end());
        for (std::size_t i = 0; i < pending.size(); ++i)
        {
            const auto id = pending[i];
            if (!body_by_method_[id])
                continue;
            try
            {
                const auto& info = body(id);
                auto instructions = read_code(info);
                if (std::ranges::none_of(instructions, [](const auto& item) { return item.op == 0x40; }))
                    continue;
                deobfuscate_flow(instructions, info.code.size(), info.exceptions, local_start(id), info.locals);
                for (const auto& item : instructions)
                    if (item.op == 0x40)
                    {
                        const auto nested = static_cast<unsigned>(item.args[0]);
                        if (nested < methods_.size() && methods_[nested].label.empty() && selected.insert(nested).second)
                            pending.push_back(nested);
                    }
            }
            catch (const std::runtime_error&) {}
        }
    }
    AbcDeobfuscation result;
    std::map<std::string, unsigned, std::less<>> pool;
    std::vector<std::string> added;
    if (decoders)
        for (unsigned i = 1; i < strings_.size(); ++i)
            pool.try_emplace(strings_[i], i);
    std::size_t copied = 0;
    for (const auto& body : bodies_)
    {
        if (type && !selected.contains(body.method))
            continue;
        try
        {
            auto instructions = read_code(body);
            const auto changes = decoders ? FlowChanges{} :
                deobfuscate_flow(instructions, body.code.size(), body.exceptions, local_start(body.method), body.locals);
            const auto resolved = decoders ? rewrite_string_calls(instructions, body, *decoders, pool, added) : 0;
            if (!resolved && !changes.removed && !changes.redirected && !changes.constant_branches && !changes.constants)
                continue;
            const auto assembled = assemble_method(render_pcode(body, instructions, Output{}));
            validate(body.method, assembled, added.size());
            if (std::ranges::equal(body.code, assembled.code))
                continue;
            append(result.bytes, bytes_.subspan(copied, body.begin - copied));
            write_method_code(result.bytes, assembled);
            copied = body.traits;
            ++result.report.changed_methods;
            result.report.removed_instructions += changes.removed;
            result.report.constant_branches += changes.constant_branches;
            result.report.resolved_strings += resolved;
        }
        catch (const std::runtime_error&)
        {
            ++result.report.skipped_methods;
        }
    }
    if (copied)
        append(result.bytes, bytes_.subspan(copied));
    if (copied && !added.empty())
    {
        Bytes updated;
        append(updated, bytes_.first(strings_begin_));
        put_u30(updated, static_cast<unsigned>(strings_.size() + added.size()));
        const auto first = strings_.size() > 1 ? string_ranges_[1].first : strings_end_;
        append(updated, bytes_.subspan(first, strings_end_ - first));
        for (const auto& string : added)
        {
            put_u30(updated, static_cast<unsigned>(string.size()));
            append(updated, {reinterpret_cast<const std::uint8_t*>(string.data()), string.size()});
        }
        append(updated, ByteView(result.bytes).subspan(strings_end_));
        result.bytes = std::move(updated);
    }
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
    const auto initializer = std::ranges::find_if(type.members, [](const auto& member) { return member.declaration.empty(); });
    if (initializer != type.members.end() && initializer->method && at(body_by_method_, *initializer->method))
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
                const auto matches_field = [&](const auto& value)
                {
                    return !value.method && value.name == field && value.declaration.find("static ") != std::string::npos;
                };
                const auto member = std::ranges::find_if(type.members, matches_field);
                const auto position = static_cast<std::size_t>(member - type.members.begin());
                if (member == type.members.end() || static_values.contains(position) ||
                    std::ranges::find_if(std::next(member), type.members.end(), matches_field) != type.members.end())
                {
                    declarations_only = false;
                    break;
                }
                const auto value = statement.substr(equal + 3, statement.size() - equal - 4);
                if (const auto declared = member->declaration.find(" = "); declared != std::string::npos &&
                    std::string_view(member->declaration).substr(declared + 3) != value)
                {
                    declarations_only = false;
                    break;
                }
                // Compilers can emit the same default in the trait table and in the class initializer.
                static_values.emplace(position, value);
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
    std::vector<std::size_t> order;
    order.reserve(type.members.size());
    auto next_static = static_order.begin();
    for (std::size_t i = 0; i < type.members.size(); ++i)
        if (!type.members[i].method)
            // Preserve the execution order of the assignments folded into these declarations.
            order.push_back(static_values.contains(i) ? *next_static++ : i);
    if (initializer != type.members.end())
        order.push_back(static_cast<std::size_t>(initializer - type.members.begin()));
    for (std::size_t i = 0; i < type.members.size(); ++i)
        if (type.members[i].method && !type.members[i].declaration.empty())
            order.push_back(i);

    bool previous_field = false;
    bool previous_static_field = false;
    std::optional<std::size_t> first_static_line;
    result.members.resize(type.members.size());
    for (const auto position : order)
    {
        const auto& member = type.members[position];
        auto& range = result.members[position];
        if (member.declaration.empty())
        {
            range.first_line = first_static_line.value_or(line);
            if (!static_code.empty())
            {
                if (previous_field)
                    add("\n");
                range.first_line = line;
                append_body(static_code, "    ");
                range.past_last_line = line;
                add("\n");
                previous_field = false;
            }
            continue;
        }
        const bool static_field = !member.method && member.declaration.find("static ") != std::string::npos;
        if (previous_field && (member.method || static_field != previous_static_field))
            add("\n");
        range.first_line = line;
        previous_field = !member.method;
        previous_static_field = static_field;
        add("        " + names.spell(member.declaration));
        if (const auto value = static_values.find(position); value != static_values.end())
        {
            if (!first_static_line)
                first_static_line = line;
            if (member.declaration.find(" = ") == std::string::npos)
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
        }
        if (!member.method || !at(body_by_method_, *member.method) || type.interface_type)
        {
            add(";\n");
            range.past_last_line = line;
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
        add("        }\n");
        range.past_last_line = line;
        add("\n");
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
        for (auto& range : result.members)
        {
            range.first_line += names.imports.size() + 1;
            if (range.past_last_line)
                range.past_last_line += names.imports.size() + 1;
        }
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

As3Obfuscation detect_as3_obfuscation(const Movie& movie)
{
    As3Obfuscation result;
    script::SymbolNames symbols;
    for (std::size_t tag = 0; tag < movie.tags().size(); ++tag)
        if (movie.tags()[tag].code == 72 || movie.tags()[tag].code == 82)
        {
            try
            {
                const script::Abc module(abc_bytecode(movie, tag));
                result.methods += module.obfuscated_methods();
                module.collect_obfuscated_names(symbols);
            }
            catch (const std::runtime_error&) {}
        }
    result.identifiers = symbols.size();
    return result;
}

namespace
{
void append_linked_class(Bytes& result, const script::NameReplacements& names, Reader& input, bool& changed)
{
    const auto name = input.cstring();
    const auto found = names.find(name);
    const auto& value = found == names.end() ? name : found->second;
    changed |= found != names.end();
    append(result, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    result.push_back(0);
}

Bytes rename_linked_classes(unsigned code, ByteView payload, const script::NameReplacements& names)
{
    Reader input(payload);
    Bytes result;
    bool changed = false;
    if (code == 76)
    {
        const auto count = input.u16();
        append(result, payload.first(2));
        for (unsigned i = 0; i < count; ++i)
        {
            append(result, input.take(2));
            append_linked_class(result, names, input, changed);
        }
    }
    else
    {
        input.skip(1);
        const auto extra = input.u8();
        if (!(extra & 8))
            return {};
        input.skip(2);
        append(result, payload.first(input.position()));
        append_linked_class(result, names, input, changed);
    }
    if (!changed)
        return {};
    append(result, input.take(input.remaining()));
    return result;
}

SwfDeobfuscation rewrite_as3(const Movie& movie, std::optional<std::size_t> selected, std::optional<std::size_t> type,
    script::StringDecoders* decoders)
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
            const auto abc = script::Abc(abc_bytecode(movie, tag)).deobfuscate(type, decoders);
            result.report.skipped_methods += abc.report.skipped_methods;
            if (!abc.bytes.empty())
            {
                auto replacement = replace_abc(result.movie ? *result.movie : movie, tag, abc.bytes);
                result.movie = std::move(replacement);
                result.report.changed_methods += abc.report.changed_methods;
                result.report.removed_instructions += abc.report.removed_instructions;
                result.report.constant_branches += abc.report.constant_branches;
                result.report.resolved_strings += abc.report.resolved_strings;
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

SwfDeobfuscation deobfuscate_as3(const Movie& movie, std::optional<std::size_t> selected, std::optional<std::size_t> type)
{
    return rewrite_as3(movie, selected, type, nullptr);
}

SwfDeobfuscation simplify_as3_strings(const Movie& movie, std::optional<std::size_t> selected, std::optional<std::size_t> type)
{
    script::StringDecoders decoders;
    std::vector<script::Abc> modules;
    std::map<std::string, std::size_t> class_counts;
    for (std::size_t tag = 0; tag < movie.tags().size(); ++tag)
        if (movie.tags()[tag].code == 72 || movie.tags()[tag].code == 82)
        {
            try { modules.emplace_back(abc_bytecode(movie, tag)); }
            catch (const std::runtime_error&) { return {}; }
            for (const auto& declared : modules.back().classes())
                ++class_counts[declared.qualified];
        }
    for (const auto built_in : {"int", "String", "Array", "Object"})
        if (class_counts.contains(built_in))
            return {};
    for (const auto& module : modules)
        module.register_string_decoders(decoders, class_counts);
    modules.clear();
    if (decoders.methods.empty())
        return {};
    return rewrite_as3(movie, selected, type, &decoders);
}

SwfDeobfuscation rename_as3_identifiers(const Movie& movie)
{
    std::vector<std::pair<std::size_t, script::Abc>> modules;
    script::SymbolNames names, classes;
    for (std::size_t tag = 0; tag < movie.tags().size(); ++tag)
        if (movie.tags()[tag].code == 72 || movie.tags()[tag].code == 82)
        {
            try { modules.emplace_back(tag, script::Abc(abc_bytecode(movie, tag))); }
            catch (const std::runtime_error&) { return {}; }
            modules.back().second.collect_obfuscated_names(names, &classes);
        }
    if (names.empty())
        return {};

    // Pick a single spelling for each identifier across ABC blocks, including dynamic lookups by exact name.
    std::set<std::string_view> occupied;
    for (const auto& module : modules)
        occupied.insert(module.second.strings().begin(), module.second.strings().end());
    script::NameReplacements replacements, class_links, suggestions;
    for (const auto& module : modules)
        module.second.suggest_field_names(suggestions);
    script::SymbolNames aliases;
    unsigned class_number = 0, member_number = 0;
    for (const auto& name : names)
    {
        std::string alias;
        const bool is_class = classes.contains(name);
        if (const auto hint = suggestions.find(name); !is_class && hint != suggestions.end() && !hint->second.empty())
        {
            alias = hint->second;
            for (unsigned suffix = 2; occupied.contains(alias) || aliases.contains(alias); ++suffix)
                alias = hint->second + std::to_string(suffix);
        }
        else
            do
            {
                alias = is_class ? std::format("Class_{}", ++class_number) : std::format("symbol_{}", ++member_number);
            } while (occupied.contains(alias) || aliases.contains(alias));
        aliases.insert(alias);
        replacements.emplace(name, std::move(alias));
    }
    for (const auto& module : modules)
        module.second.collect_renamed_classes(replacements, class_links);
    replacements.insert(class_links.begin(), class_links.end());

    SwfDeobfuscation result;
    const auto current = [&]() -> const Movie& { return result.movie ? *result.movie : movie; };
    for (const auto& [tag, module] : modules)
        if (const auto abc = module.rename_identifiers(replacements); !abc.empty())
            result.movie = replace_abc(current(), tag, abc);
    if (!class_links.empty())
        for (std::size_t tag = 0; tag < movie.tags().size(); ++tag)
        {
            const auto code = movie.tags()[tag].code;
            if (code == 76 || code == 70 || code == 94)
                if (const auto linkage = rename_linked_classes(code, movie.data(tag), class_links); !linkage.empty())
                    result.movie = current().replace_tag(tag, linkage);
        }
    result.report.renamed_identifiers = names.size();
    return result;
}
}
