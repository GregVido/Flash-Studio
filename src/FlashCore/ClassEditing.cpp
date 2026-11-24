#include "ClassEditing.hpp"
#include "Abc.hpp"
#include "AbcLink.hpp"
#include "ActionScript.hpp"
#include "Compression.hpp"

#include <Windows.h>
#include <objbase.h>
#include <array>
#include <format>
#include <memory>

#pragma comment(lib, "Ole32.lib")

namespace flash
{
namespace
{
std::filesystem::path environment_path(const wchar_t* name)
{
    const auto size = GetEnvironmentVariableW(name, nullptr, 0);
    if (!size)
        return {};
    std::wstring value(size, L'\0');
    const auto used = GetEnvironmentVariableW(name, value.data(), size);
    if (!used || used >= size)
        return {};
    value.resize(used);
    return value;
}

std::filesystem::path find_java()
{
    const auto java_home = environment_path(L"JAVA_HOME");
    std::error_code error;
    if (java_home.is_absolute() && std::filesystem::is_regular_file(java_home / L"bin" / L"java.exe", error))
        return java_home / L"bin" / L"java.exe";
    for (const auto variable : {L"ProgramW6432", L"ProgramFiles"})
    {
        const auto directory = environment_path(variable);
        if (!directory.is_absolute())
            continue;
        for (std::filesystem::directory_iterator it(directory / L"Java", error), end; !error && it != end; it.increment(error))
            if (auto candidate = it->path() / L"bin" / L"java.exe"; std::filesystem::is_regular_file(candidate, error))
                return candidate;
    }
    const auto path = environment_path(L"Path").wstring();
    auto remainder = std::wstring_view(path);
    while (!remainder.empty())
    {
        const auto end = remainder.find(L';');
        const std::filesystem::path directory(remainder.substr(0, end));
        if (directory.is_absolute() && std::filesystem::is_regular_file(directory / L"java.exe", error))
            return directory / L"java.exe";
        if (end == std::wstring_view::npos)
            break;
        remainder.remove_prefix(end + 1);
    }
    throw std::runtime_error("Java was not found. Install Java or set JAVA_HOME.");
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        GUID guid{};
        std::array<wchar_t, 40> name{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, name.data(), static_cast<int>(name.size())))
            throw std::runtime_error("Impossible de préparer le dossier de compilation.");
        path = std::filesystem::temp_directory_path() / (std::wstring(L"FlashStudio-") + name.data());
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("Impossible de créer le dossier de compilation.");
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    std::filesystem::path path;
};

std::wstring wide(std::string_view text)
{
    if (text.empty())
        return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size)
        throw std::runtime_error("Nom de classe UTF-8 invalide.");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring quote_argument(std::wstring_view argument)
{
    std::wstring text = L"\"";
    unsigned slashes = 0;
    for (const auto c : argument)
    {
        if (c == L'\\')
        {
            ++slashes;
            continue;
        }
        text.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        text += c;
    }
    text.append(slashes * 2, L'\\');
    return text + L'"';
}

void run_compiler(const std::filesystem::path& compiler, const std::filesystem::path& directory,
    const std::vector<std::wstring>& arguments, const std::filesystem::path& expected_output)
{
    if (!compiler.is_absolute() || !std::filesystem::is_regular_file(compiler))
        throw std::runtime_error("The ActionScript compiler archive was not found.");
    const auto java = find_java();
    const auto log = directory / L"compilation.txt";
    auto command = quote_argument(java.wstring()) + L" -Djava.awt.headless=true -Dfile.encoding=UTF-8 -jar " + quote_argument(compiler.wstring());
    for (const auto& argument : arguments)
        command += L' ' + quote_argument(argument);
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto close = [](void* handle) { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); };
    using Handle = std::unique_ptr<void, decltype(close)>;
    Handle output(CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr), close);
    const Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr), close);
    if (output.get() == INVALID_HANDLE_VALUE || input.get() == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Impossible d’ouvrir les flux du compilateur AS3.");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.get();
    startup.hStdOutput = startup.hStdError = output.get();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(java.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
        throw std::runtime_error("Impossible de lancer Java pour compiler la classe.");
    const Handle child(process.hProcess, close);
    const Handle thread(process.hThread, close);
    if (WaitForSingleObject(child.get(), 45'000) != WAIT_OBJECT_0)
    {
        TerminateProcess(child.get(), 1);
        WaitForSingleObject(child.get(), 3000);
        throw std::runtime_error("Le compilateur AS3 n’a pas répondu dans le délai imparti.");
    }
    output.reset();
    DWORD exit{};
    if (!GetExitCodeProcess(child.get(), &exit) || exit || !std::filesystem::is_regular_file(expected_output))
    {
        const auto bytes = read_file(log);
        const auto limit = (std::min)(bytes.size(), std::size_t{8192});
        const auto diagnostic = std::string(bytes.end() - limit, bytes.end());
        throw std::runtime_error(diagnostic.empty() ? "ActionScript compilation failed." : diagnostic);
    }
}

std::filesystem::path class_file(std::string_view qualified)
{
    std::filesystem::path file;
    while (!qualified.empty())
    {
        const auto separator = qualified.find('.');
        const auto part = qualified.substr(0, separator);
        if (script::identifier(part, "") != part || part.empty())
            throw std::runtime_error("Le nom de la classe ne peut pas être compilé directement.");
        file /= wide(part);
        if (separator == std::string_view::npos)
        {
            file += L".as";
            return file;
        }
        qualified.remove_prefix(separator + 1);
    }
    throw std::runtime_error("Nom de classe vide.");
}

bool abc_tag(unsigned code) { return code == 72 || code == 82; }

Bytes library_swf(ByteView archive)
{
    // The SWC catalog can be ignored here. ASC imports each ABC from library.swf.
    if (archive.size() < 22)
        throw std::runtime_error("Invalid Flash API SWC archive.");
    const auto first = archive.size() > 65'557 ? archive.size() - 65'557 : 0;
    for (auto offset = archive.size() - 22;; --offset)
    {
        Reader end(archive.subspan(offset));
        if (end.u32() == 0x06054B50)
        {
            const auto disk = end.u32();
            const auto disk_entries = end.u16();
            const auto entries = end.u16();
            const auto size = end.u32();
            const auto directory = end.u32();
            const auto comment = end.u16();
            if (!disk && entries == disk_entries && comment == end.remaining() && directory <= offset &&
                size <= offset - directory)
            {
                Reader central(archive.subspan(directory, size));
                for (unsigned entry = 0; entry < entries; ++entry)
                {
                    if (central.u32() != 0x02014B50)
                        break;
                    central.skip(6);
                    const auto method = central.u16();
                    central.skip(8);
                    const auto stored = central.u32();
                    const auto expanded = central.u32();
                    const auto name = central.u16(), extra = central.u16(), note = central.u16();
                    central.skip(8);
                    const auto start = central.u32();
                    const auto filename = central.string(name);
                    central.skip(extra + note);
                    if (filename != "library.swf")
                        continue;
                    if (start >= directory)
                        break;
                    Reader local(archive.subspan(start, directory - start));
                    if (local.u32() != 0x04034B50)
                        break;
                    local.skip(22);
                    const auto local_name = local.u16(), local_extra = local.u16();
                    if (local.string(local_name) != filename)
                        break;
                    local.skip(local_extra);
                    const auto data = local.take(stored);
                    if (!method && stored == expanded)
                        return Bytes(data.begin(), data.end());
                    if (method == 8)
                        return inflate_zip_entry(data, expanded);
                    break;
                }
            }
        }
        if (offset == first)
            break;
    }
    throw std::runtime_error("The Flash API SWC archive has no readable library.swf.");
}

struct ClassLocation { std::size_t tag, index; };

std::optional<ClassLocation> class_location(const Movie& movie, std::string_view name)
{
    std::optional<ClassLocation> match;
    for (std::size_t tag = 0; tag < movie.tags().size(); ++tag)
        if (abc_tag(movie.tags()[tag].code))
        {
            const AbcModule module(abc_bytecode(movie, tag));
            for (std::size_t index = 0; index < module.classes().size(); ++index)
                if (module.classes()[index].qualified == name)
                {
                    if (match)
                        throw std::runtime_error("Multiple classes have the same qualified name.");
                    match = ClassLocation{tag, index};
                }
        }
    return match;
}

Bytes compile(const Movie& movie, const std::optional<ClassLocation>& editing, std::string_view name,
    std::string_view source, const std::filesystem::path& compiler, bool french)
{
    if (source.empty() || source.size() > 1024 * 1024)
        throw std::runtime_error("The class source must contain between 1 byte and 1 MiB.");
    const auto file = class_file(name).filename();
    const TemporaryDirectory work;
    write_text(work.path / file, source);
    std::vector<std::wstring> arguments{L"-AS3", L"-strict", L"-md", L"-language", french ? L"FR" : L"EN"};
    unsigned library_index = 0;
    const auto add_library = [&](ByteView abc)
    {
        if (library_index >= 1'000)
            throw std::runtime_error("Too many ABC libraries for one compilation.");
        const auto filename = std::format(L"lib{}.abc", ++library_index);
        write_file(work.path / filename, abc);
        arguments.insert(arguments.end(), {L"-import", filename});
    };
    const auto platform = compiler.parent_path() / L"playerglobal.swc";
    const auto builtins = compiler.parent_path() / L"builtin.abc";
    if (std::filesystem::is_regular_file(platform))
    {
        const auto swc = read_file(platform);
        const auto api = Movie::parse(library_swf(swc));
        for (std::size_t tag = 0; tag < api.tags().size(); ++tag)
            if (abc_tag(api.tags()[tag].code))
                add_library(abc_bytecode(api, tag));
    }
    else if (std::filesystem::is_regular_file(builtins))
        add_library(read_file(builtins));
    if (!library_index)
        throw std::runtime_error("Place playerglobal.swc or builtin.abc next to asc.jar.");
    for (std::size_t tag = 0; tag < movie.tags().size(); ++tag)
        if (abc_tag(movie.tags()[tag].code))
        {
            const auto abc = abc_bytecode(movie, tag);
            if (editing && tag == editing->tag)
                add_library(script::hide_class_for_import(abc, editing->index));
            else
                add_library(abc);
        }

    arguments.push_back(file.wstring());
    auto output = work.path / file;
    output.replace_extension(L".abc");
    run_compiler(compiler, work.path, arguments, output);
    auto result = read_file(output);
    const AbcModule module(result);
    if (module.classes().size() != 1 || module.classes()[0].qualified != name ||
        !module.classes()[0].declaration.starts_with("public "))
        throw std::runtime_error("The source must declare exactly one public class with the requested name.");
    return result;
}
}

bool valid_as3_class_name(std::string_view qualified_name)
{
    if (qualified_name.empty() || qualified_name.size() > 240 || qualified_name.ends_with('.'))
        return false;
    try { (void)class_file(qualified_name); return true; }
    catch (const std::runtime_error&) { return false; }
}

bool supports_as3_class_creation(const Movie& movie)
{
    if (movie.header().version < 9)
        return false;
    const auto flags = std::ranges::find_if(movie.tags(), [](const Tag& tag)
    {
        return !tag.parent && tag.code == 69 && tag.size >= 4;
    });
    return flags != movie.tags().end() &&
        (movie.data(static_cast<std::size_t>(flags - movie.tags().begin()))[0] & 8);
}

Movie compile_as3_class(const Movie& movie, std::size_t tag, std::string_view name, std::string_view source,
    const std::filesystem::path& compiler, bool french)
{
    const auto location = class_location(movie, name);
    if (!location || location->tag != tag)
        throw std::runtime_error("The selected class was not found in the requested ABC.");
    const auto abc = abc_bytecode(movie, tag);
    const auto compiled = compile(movie, location, name, source, compiler, french);
    const AbcModule previous(abc), replacement(compiled);
    if (previous.classes()[location->index].declaration != replacement.classes()[0].declaration)
        throw std::runtime_error("Class editing requires the original superclass, interfaces and class modifiers.");
    const auto& old_class = previous.classes()[location->index];
    std::vector<std::pair<unsigned, unsigned>> retained;
    for (const auto& member : replacement.classes()[0].members)
    {
        if (!member.method)
            continue;
        const auto is_static = [](const AbcMember& item) { return item.declaration.find("static ") != std::string::npos; };
        const auto equivalent = std::ranges::find_if(old_class.members, [&](const AbcMember& item)
        {
            return item.method && item.name == member.name && is_static(item) == is_static(member);
        });
        if (equivalent == old_class.members.end())
            continue;
        const auto method = *equivalent->method;
        const auto shared = std::ranges::any_of(previous.classes(), [&](const AbcClass& owner)
        {
            return &owner != &old_class && std::ranges::any_of(owner.members,
                [&](const AbcMember& other) { return other.method == method; });
        });
        if (!shared)
            retained.emplace_back(*member.method, method);
    }
    auto linked = script::link_class(abc, location->index, compiled, retained);
    (void)AbcModule(linked);
    return replace_abc(movie, tag, linked);
}

Movie add_as3_class(const Movie& movie, std::string_view name, std::string_view source,
    const std::filesystem::path& compiler, bool french)
{
    if (!valid_as3_class_name(name))
        throw std::runtime_error("Invalid ActionScript class or package name.");
    if (!supports_as3_class_creation(movie))
        throw std::runtime_error("A new class requires an ActionScript 3 SWF.");
    if (class_location(movie, name))
        throw std::runtime_error("This class already exists in the SWF.");
    const auto abc = compile(movie, {}, name, source, compiler, french);
    const auto& tags = movie.tags();
    const auto before = std::ranges::find_if(tags, [](const Tag& tag)
    {
        return !tag.parent && (abc_tag(tag.code) || tag.code == 76 || tag.code == 1 || tag.code == 0);
    });
    if (before == tags.end())
        throw std::runtime_error("No root frame was found for the new class.");
    Bytes payload{1, 0, 0, 0}; // Register the class lazily with the current application domain.
    payload.insert(payload.end(), name.begin(), name.end());
    payload.push_back(0);
    append(payload, abc);
    return movie.insert_root_tag(static_cast<std::size_t>(before - tags.begin()), 82, payload);
}
}

