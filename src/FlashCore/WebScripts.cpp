#include "WebExport.hpp"
#include "Abc.hpp"
#include "ActionScript.hpp"
#include "Binary.hpp"

#include <format>
#include <set>

namespace flash::web
{
namespace
{
void source_file(const std::filesystem::path& folder, std::size_t index, const AbcClassSource& source)
{
    std::filesystem::create_directories(folder);
    write_text(folder / std::format(L"class-{:04}.as", index), source.text);
}
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
            auto javascript = compile_as3(source, scope);
            if (type.qualified == "Scene" && display == "enterF" &&
                javascript.body.find("clip.view[\"curZic\"]") != std::string::npos)
            {
                // The stage music persists across rooms, including the interval without collision geometry.
                constexpr std::string_view collision = R"(clip.view["camera"]["currentMap"]["physic"]["surfaceMap"]["sceneColid"])";
                if (const auto offset = javascript.body.find(collision); offset != std::string::npos)
                    javascript.body.replace(offset, collision.size(),
                        R"(clip.view["camera"]?.["currentMap"]?.["physic"]?.["surfaceMap"]?.["sceneColid"])");
            }
            if (type.qualified == "map.RainEffectItem" && display == "addPass" &&
                javascript.body.find(R"(clip.view["contentBmd"]["copyPixels"]($arg1, $arg1["rect"],)") != std::string::npos)
            {
                // The SWF fills the surface with complete repeated tiles at integer offsets.
                javascript.body.insert(0, R"JS(const target = clip.view["contentBmd"];
if (target?.width && target.height && $arg1?.width && $arg1.height) {
    const x = -(($arg1.width - (($arg2.x % $arg1.width) >>> 0)) % target.width) | 0;
    const y = -(($arg1.height - (($arg2.y % $arg1.height) >>> 0)) % target.height) | 0;
    if (x <= 0 && y <= 0 && target.tilePixels($arg1, x, y)) return;
}
)JS");
            }
            method.function = function_expression(std::move(javascript));
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
