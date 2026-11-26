#include "WebExport.hpp"
#include "Abc.hpp"
#include "ActionScript.hpp"
#include "Binary.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <set>

namespace flash::web
{
namespace
{
std::string number(double value)
{
    if (std::isnan(value))
        return "NaN";
    if (!std::isfinite(value))
        return value < 0 ? "-Infinity" : "Infinity";
    return std::format("{:.17g}", value);
}

struct Value
{
    std::string expression;
    std::optional<std::string> literal;
};

void source_file(const std::filesystem::path& folder, std::size_t index, const AbcClassSource& source)
{
    std::filesystem::create_directories(folder);
    write_text(folder / std::format(L"class-{:04}.as", index), source.text);
}
}

std::string frame_action(ByteView input)
{
    Reader script(input);
    std::vector<std::string> constants;
    std::vector<Value> stack;
    std::array<std::optional<Value>, 256> registers;
    std::string function;
    unsigned temporary = 0;
    const auto pop = [&]
    {
        if (stack.empty())
            throw std::runtime_error("Expression ActionScript 1/2 incomplète.");
        auto value = std::move(stack.back());
        stack.pop_back();
        return value;
    };
    const auto push = [&](std::string expression, std::optional<std::string> literal = {})
    {
        if (expression.size() > 16384 || stack.size() >= 128)
            throw std::runtime_error("Expression ActionScript 1/2 trop grande.");
        stack.push_back({std::move(expression), std::move(literal)});
    };
    const auto string = [&](std::string value) { push(json_string(value), value); };
    const auto evaluate = [&](std::string expression)
    {
        if (expression.size() > 16384)
            throw std::runtime_error("Expression ActionScript 1/2 trop grande.");
        const auto reference = "$avm1_" + std::to_string(temporary++);
        function += "const " + reference + " = " + expression + ";\n";
        push(reference);
    };
    const auto binary = [&](std::string_view op, std::string_view coercion = {})
    {
        const auto right = pop().expression, left = pop().expression;
        const auto cast = [&](const std::string& value) { return coercion.empty() ? value : std::string(coercion) + '(' + value + ')'; };
        evaluate(cast(left) + " " + std::string(op) + " " + cast(right));
    };
    const auto variable = [&]
    {
        const auto value = pop();
        if (!value.literal || flash::script::identifier(*value.literal, "").empty())
            throw std::runtime_error("Nom de variable ActionScript 1/2 calculé ou qualifié.");
        return "clip.variables[" + value.expression + ']';
    };
    unsigned count = 0;
    while (script.remaining())
    {
        if (++count > 4096 || function.size() > 65536)
            throw std::runtime_error("Script ActionScript 1/2 trop long.");
        const auto op = script.u8();
        if (!op)
            break;
        Reader args(op >= 0x80 ? script.take(script.u16()) : ByteView{});
        switch (op)
        {
        case 0x04: function += "clip.nextFrame();\n"; break;
        case 0x05: function += "clip.prevFrame();\n"; break;
        case 0x06: function += "clip.play();\n"; break;
        case 0x07: function += "clip.stop();\n"; break;
        case 0x09: function += "clip.stopSounds();\n"; break;
        case 0x0A: binary("+", "Number"); break;
        case 0x0B: binary("-", "Number"); break;
        case 0x0C: binary("*", "Number"); break;
        case 0x0D: binary("/", "Number"); break;
        case 0x0E: binary("===", "Number"); break;
        case 0x0F: binary("<", "Number"); break;
        case 0x10: binary("&&", "Boolean"); break;
        case 0x11: binary("||", "Boolean"); break;
        case 0x12: evaluate("!" + pop().expression); break;
        case 0x13: binary("===", "String"); break;
        case 0x14: case 0x31: evaluate("String(" + pop().expression + ").length"); break;
        case 0x17: pop(); break;
        case 0x18: evaluate("(" + pop().expression + " | 0)"); break;
        case 0x1C: evaluate(variable()); break;
        case 0x1D: case 0x3C:
        {
            const auto value = pop();
            function += variable() + " = " + value.expression + ";\n";
            break;
        }
        case 0x41: function += variable() + " ??= undefined;\n"; break;
        case 0x21: binary("+", "String"); break;
        case 0x29: binary("<", "String"); break;
        case 0x26: function += "console.log(" + pop().expression + ");\n"; break;
        case 0x22: case 0x23:
        {
            auto value = op == 0x23 ? pop().expression : std::string{};
            const auto property = pop().expression;
            const auto target = pop();
            constexpr std::string_view properties[]{"x", "y", "scaleX", "scaleY", "", "", "alpha", "visible", "", "", "rotation"};
            unsigned index{};
            const auto end = property.data() + property.size();
            const auto field = std::from_chars(property.data(), end, index);
            if (!target.literal || flash::script::identifier(*target.literal, "").empty() ||
                field.ec != std::errc{} || field.ptr != end || index >= std::size(properties) || properties[index].empty())
                throw std::runtime_error("Cible ou propriété ActionScript 1/2 à porter.");
            const auto percentage = index == 2 || index == 3 || index == 6;
            const auto argument = target.expression + ", " + json_string(properties[index]);
            if (op == 0x22)
            {
                auto read = "clip.get(" + argument + ')';
                evaluate(percentage ? read + " * 100" : read);
                break;
            }
            if (percentage)
                value = "Number(" + value + ") / 100";
            function += "clip.set(" + argument + ", " + value + ");\n";
            break;
        }
        case 0x34: evaluate("clip.elapsed()"); break;
        case 0x3F: binary("%", "Number"); break;
        case 0x42:
        {
            const auto count_value = pop().expression;
            unsigned length{};
            const auto last = count_value.data() + count_value.size();
            const auto parsed = std::from_chars(count_value.data(), last, length);
            if (parsed.ec != std::errc{} || parsed.ptr != last || length > stack.size())
                throw std::runtime_error("Taille de tableau ActionScript 1/2 dynamique ou invalide.");
            std::string array = "[";
            for (unsigned index = 0; index < length; ++index)
            {
                if (index) array += ", ";
                array += pop().expression;
            }
            evaluate(array + ']');
            break;
        }
        case 0x44: evaluate("typeof " + pop().expression); break;
        case 0x47: binary("+"); break;
        case 0x48: binary("<"); break;
        case 0x49: binary("=="); break;
        case 0x4A: evaluate("Number(" + pop().expression + ')'); break;
        case 0x4B: evaluate("String(" + pop().expression + ')'); break;
        case 0x4C:
        {
            const auto top = pop();
            push(top.expression, top.literal);
            push(top.expression, top.literal);
            break;
        }
        case 0x4D:
        {
            const auto right = pop(), left = pop();
            push(right.expression, right.literal);
            push(left.expression, left.literal);
            break;
        }
        case 0x50: evaluate("Number(" + pop().expression + ") + 1"); break;
        case 0x51: evaluate("Number(" + pop().expression + ") - 1"); break;
        case 0x60: binary("&"); break;
        case 0x61: binary("|"); break;
        case 0x62: binary("^"); break;
        case 0x63: binary("<<"); break;
        case 0x64: binary(">>"); break;
        case 0x65: binary(">>>"); break;
        case 0x66: binary("==="); break;
        case 0x67: binary(">"); break;
        case 0x68: binary(">", "String"); break;
        case 0x81: function += std::format("clip.gotoAndStop({});\n", unsigned{args.u16()} + 1); break;
        case 0x87:
        {
            auto value = pop();
            registers[args.u8()] = value;
            push(std::move(value.expression), std::move(value.literal));
            break;
        }
        case 0x8C: function += "clip.gotoAndStop(" + json_string(args.cstring()) + ");\n"; break;
        case 0x88:
        {
            const auto size = args.u16();
            constants.clear();
            for (unsigned i = 0; i < size; ++i)
                constants.push_back(args.cstring());
            break;
        }
        case 0x96:
            while (args.remaining())
            {
                switch (args.u8())
                {
                case 0: string(args.cstring()); break;
                case 1: push(number(std::bit_cast<float>(args.u32()))); break;
                case 2: push("null"); break;
                case 3: push("undefined"); break;
                case 4:
                {
                    const auto& value = registers[args.u8()];
                    if (value) push(value->expression, value->literal);
                    else throw std::runtime_error("Registre ActionScript 1/2 défini hors du bloc.");
                    break;
                }
                case 5: push(args.u8() ? "true" : "false"); break;
                case 6:
                {
                    const auto high = args.u32();
                    push(number(std::bit_cast<double>((std::uint64_t{high} << 32) | args.u32())));
                    break;
                }
                case 7: push(std::to_string(std::bit_cast<std::int32_t>(args.u32()))); break;
                case 8: string(constants.at(args.u8())); break;
                case 9: string(constants.at(args.u16())); break;
                default: throw std::runtime_error("Opérande ActionScript 1/2 sans équivalent exporté.");
                }
            }
            break;
        case 0x9F:
        {
            const auto flags = args.u8();
            const auto frame = pop();
            auto position = frame.expression;
            if (flags & 2)
                position = "Number(" + position + ") + " + std::to_string(args.u16());
            function += std::string(flags & 1 ? "clip.gotoAndPlay(" : "clip.gotoAndStop(") + position + ");\n";
            break;
        }
        default:
            throw std::runtime_error(std::format("Instruction ActionScript 1/2 0x{:02X} à porter.", op));
        }
    }
    return function;
}

namespace
{
bool is_static(const AbcMember& member) { return member.declaration.find(" static ") != std::string::npos; }
bool is_private(const AbcMember& member) { return member.declaration.starts_with("private "); }

std::string method_key(const AbcMember& member)
{
    if (member.name.starts_with("get ") || member.name.starts_with("set "))
        return member.name.substr(0, 3) + ':' + member.name.substr(4);
    return member.name;
}

std::string function_expression(WebFunction function)
{
    return "(clip" + function.parameters + ") => {\n" + function.body + "}";
}

void find_references(const AbcModule& abc, std::set<std::string>& references)
{
    for (const auto& value : abc.strings())
    {
        const auto address = std::string_view(value).substr(0, value.find_first_of("?#"));
        if (address.size() < 5 || address.size() > 512 || address.find_first_of("\r\n\t<>\"|") != std::string_view::npos)
            continue;
        const auto extension = address.substr(address.size() - 4);
        if (extension[0] == '.' && (extension[1] == 's' || extension[1] == 'S') &&
            (extension[2] == 'w' || extension[2] == 'W') && (extension[3] == 'f' || extension[3] == 'F') &&
            references.size() < 1024)
            references.insert(value.substr(0, 1024));
    }
}

struct ClassInput
{
    std::size_t abc{};
    const AbcClass* type{};
};
using Classes = std::map<std::string, ClassInput, std::less<>>;

WebScope class_scope(const AbcClass& type, const Classes& classes, const std::set<std::string, std::less<>>& names)
{
    WebScope scope;
    scope.owner = type.qualified;
    scope.package = type.package;
    scope.types = names;
    std::set<const AbcClass*> examined;
    for (auto owner = &type; owner && examined.emplace(owner).second;)
    {
        for (const auto& member : owner->members)
        {
            if (member.declaration.empty() || member.name == owner->name || (owner != &type && is_private(member)))
                continue;
            const auto accessor = member.name.starts_with("get ") || member.name.starts_with("set ");
            const auto name = accessor ? member.name.substr(4) : member.name;
            if (is_static(member))
            {
                if (owner == &type) scope.statics.insert(name);
            }
            else if (member.method && !accessor)
                scope.methods.insert(name);
            else
                scope.fields.insert(name);
        }
        const auto base = classes.find(owner->base);
        if (owner->base == "flash.utils.ByteArray")
        {
            scope.fields.insert({"length", "position", "bytesAvailable", "endian"});
            scope.methods.insert({"clear", "readBoolean", "readByte", "readUnsignedByte", "readShort", "readUnsignedShort",
                "readInt", "readUnsignedInt", "readFloat", "readDouble", "readUTF", "readUTFBytes", "readMultiByte",
                "readBytes", "writeBoolean", "writeByte", "writeShort", "writeInt", "writeUnsignedInt", "writeFloat",
                "writeDouble", "writeUTF", "writeUTFBytes", "writeBytes"});
        }
        if (owner->base == "flash.events.Event")
            scope.fields.insert({"type", "bubbles", "cancelable", "target", "currentTarget"});
        if (owner->base == "flash.display.Bitmap")
            scope.fields.insert({"bitmapData", "pixelSnapping", "smoothing"});
        owner = base == classes.end() ? nullptr : base->second.type;
    }
    return scope;
}

WebClass compile_class(const AbcModule& abc, const AbcClass& type, WebScope scope,
    ExportReport& report, std::set<unsigned>& translated)
{
    WebClass output;
    output.base = type.base;
    output.sealed = type.sealed;
    output.interfaces = type.interfaces;
    const auto build_method = [&](const AbcMember& member, std::string_view display, bool constructor, bool static_method)
    {
        WebMethod method;
        method.private_access = is_private(member);
        scope.constructor = constructor;
        scope.static_method = static_method;
        try
        {
            auto source = abc.decompile(*member.method);
            // An interface declaration has no executable method body.
            if (!abc.methods().at(*member.method).code_size)
                return method;
            method.function = function_expression(compile_as3(source, scope));
            translated.insert(*member.method);
        }
        catch (const std::exception& error)
        {
            method.diagnostic = std::format("{}.{} : {}", type.qualified, display, error.what());
            omission(report, method.diagnostic);
        }
        return method;
    };
    for (const auto& member : type.members)
    {
        if (member.method)
        {
            if (member.declaration.empty())
                output.initialize = build_method(member, "initialiseur statique", false, true);
            else if (member.name == type.name)
                output.constructor = build_method(member, "constructeur", true, false);
            else
            {
                auto key = method_key(member);
                (is_static(member) ? output.statics : output.methods).emplace(key, build_method(member, key, false, is_static(member)));
            }
            continue;
        }
        const auto colon = member.declaration.find(':');
        if (colon == std::string::npos)
            continue;
        const auto suffix = std::string_view(member.declaration).substr(colon + 1);
        WebSlot slot{std::string(suffix.substr(0, suffix.find_first_of(" ;"))), {}, is_private(member)};
        if (const auto equal = suffix.find(" = "); equal != std::string_view::npos)
        {
            try
            {
                scope.static_method = is_static(member);
                scope.constructor = false;
                slot.value = function_expression(compile_as3("function value():* { return " + std::string(suffix.substr(equal + 3)) + "; }", scope));
            }
            catch (const std::exception& error)
            {
                omission(report, std::format("{}.{} : valeur initiale à porter. {}", type.qualified, member.name, error.what()));
            }
        }
        (is_static(member) ? output.static_slots : output.slots).emplace(member.name, std::move(slot));
    }
    return output;
}
}

WebProgram movie_scripts(const Movie& movie, const std::filesystem::path& sources,
    ExportReport& report, std::set<std::string>* references)
{
    WebProgram result;
    std::map<std::size_t, AbcModule> modules;
    Classes classes;
    for (std::size_t tag = 0; tag < movie.tags().size(); ++tag)
    {
        const auto code = movie.tags()[tag].code;
        try
        {
            if (code == 72 || code == 82)
            {
                auto& abc = modules.try_emplace(tag, abc_bytecode(movie, tag)).first->second;
                if (references)
                    find_references(abc, *references);
                const auto folder = sources / std::format(L"abc-{:06}", tag);
                for (std::size_t index = 0; index < abc.classes().size(); ++index)
                {
                    const auto& type = abc.classes()[index];
                    if (!classes.emplace(type.qualified, ClassInput{tag, &type}).second)
                        omission(report, std::format("ABC {} : classe {} définie plusieurs fois.", tag, type.qualified));
                    if (sources.empty())
                        continue;
                    try
                    {
                        source_file(folder, index, abc.decompile_class(index));
                        ++report.files;
                    }
                    catch (const std::exception& error)
                    {
                        omission(report, std::format("Classe {} : {}", type.qualified, error.what()));
                    }
                }

                if (sources.empty())
                    continue;
                std::string free_methods;
                for (const auto& method : abc.methods())
                    if (method.owner.empty() && method.code_size && !abc.declares_classes(method.id))
                    {
                        if (free_methods.size() > 32 * 1024 * 1024)
                            throw std::runtime_error("Le fichier des méthodes libres dépasse 32 Mio.");
                        try { free_methods += abc.decompile(method.id) + '\n'; }
                        catch (const std::exception& error) { omission(report, std::format("Méthode {} : {}", method.id, error.what())); }
                    }
                if (!free_methods.empty())
                {
                    std::filesystem::create_directories(folder);
                    write_text(folder / "standalone.as", free_methods);
                    ++report.files;
                }
            }
            else if (!sources.empty())
            {
                if (code == 59)
                {
                    Reader initialize(movie.data(tag));
                    const auto sprite = initialize.u16();
                    result.clips[sprite].legacy_initializer += frame_action(initialize.take(initialize.remaining()));
                }
                if (has_avm1_actions(movie, tag))
                {
                    const auto folder = sources / "avm1";
                    std::filesystem::create_directories(folder);
                    write_text(folder / std::format(L"tag-{:06}.as", tag), decompile(movie, tag));
                    ++report.files;
                }
            }
        }
        catch (const std::exception& error) { omission(report, std::format("Script du tag {} : {}", tag, error.what())); }
    }

    std::set<std::string, std::less<>> names;
    std::map<std::size_t, std::set<unsigned>> translated;
    for (const auto& [name, value] : classes)
        names.insert(name);
    for (const auto& [name, value] : classes)
    {
        const auto& type = *value.type;
        result.classes.emplace(name, compile_class(modules.at(value.abc), type, class_scope(type, classes, names),
            report, translated[value.abc]));
    }
    for (const auto& [tag, abc] : modules)
    {
        const auto& compiled = translated[tag];
        const auto bodies = std::ranges::count_if(abc.methods(), [&](const AbcMethod& method)
        {
            return method.code_size && !abc.declares_classes(method.id);
        });
        result.as3_bodies += static_cast<std::size_t>(bodies);
        result.compiled_bodies += compiled.size();
        if (compiled.size() < static_cast<std::size_t>(bodies))
            omission(report, std::format("ABC {} : {} corps ActionScript 3 convertis sur {}.", tag, compiled.size(), bodies));
    }

    if (movie.document_class())
        result.clips[0].type = *movie.document_class();
    for (const auto& tag : movie.tags())
        if ((tag.code == 39 || tag.code == 14 || tag.code == 7 || tag.code == 34 ||
            tag.category == Category::fonts || tag.category == Category::images) &&
            tag.character_id && result.classes.contains(tag.name))
            result.clips[*tag.character_id].type = tag.name;
    return result;
}
}
