#include "Abc.hpp"
#include "Binary.hpp"

#include <format>
#include <map>

namespace flash
{
namespace
{
std::string csv(std::string_view value)
{
    std::string text = "\"";
    for (const auto byte : value)
    {
        if (byte == '"')
            text += '"';
        text += byte;
    }
    return text + '"';
}

std::string component(std::string_view name)
{
    std::string safe;
    for (const auto c : name.substr(0, 85))
    {
        const auto ascii = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        safe += ascii ? c : '_';
    }
    while (safe.ends_with('.'))
        safe.pop_back();
    auto dos = safe.substr(0, safe.find('.'));
    for (auto& c : dos)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    if (dos == "CON" || dos == "PRN" || dos == "AUX" || dos == "NUL" ||
        (dos.size() == 4 && (dos.starts_with("COM") || dos.starts_with("LPT")) && dos[3] >= '0' && dos[3] <= '9'))
        safe.insert(0, "_");
    return safe.empty() ? "scripts" : safe;
}

std::filesystem::path class_path(const AbcClass& type)
{
    std::filesystem::path relative;
    auto package = std::string_view(type.package);
    while (!package.empty())
    {
        const auto end = package.find('.');
        relative /= component(package.substr(0, end));
        package.remove_prefix(end == std::string_view::npos ? package.size() : end + 1);
    }
    return relative / (component(type.name) + ".as");
}

std::size_t export_classes(const AbcModule& abc, const std::filesystem::path& destination)
{
    std::map<std::string, std::vector<unsigned>> classes;
    for (const auto& method : abc.methods())
        classes[method.owner].push_back(method.id);
    std::filesystem::create_directories(destination);
    std::size_t files = 0;
    for (const auto& [owner, methods] : classes)
    {
        const auto leaf = std::format("{:05}-{}", methods.front(), component(owner));
        std::string source = std::format("// {}\n\n", owner.empty() ? "Scripts et méthodes libres" : owner);
        std::string pcode;
        for (const auto id : methods)
        {
            const auto& method = abc.methods().at(id);
            try { source += abc.decompile(id); }
            catch (const std::runtime_error& error) { source += std::format("// Méthode {} : {}\n", id, error.what()); }
            if (method.code_size)
            {
                try { pcode += abc.pcode(id) + '\n'; }
                catch (const std::runtime_error&)
                {
                    pcode += "\n" + abc.disassemble(id) + '\n';
                }
            }
            if (source.size() > 64 * 1024 * 1024 || pcode.size() > 64 * 1024 * 1024)
                throw std::runtime_error("L’export d’une classe ABC dépasse 64 Mio.");
        }
        write_text(destination / (leaf + ".as"), source);
        write_text(destination / (leaf + ".pcode"), pcode);
        files += 2;
    }
    return files;
}
}

ExportReport export_project(const Movie& movie, const std::filesystem::path& destination)
{
    if (std::filesystem::exists(destination))
    {
        if (!std::filesystem::is_directory(destination) || !std::filesystem::is_empty(destination))
            throw std::runtime_error("Choisissez un dossier vide pour créer le projet exporté.");
    }
    else
        std::filesystem::create_directories(destination);

    write_file(destination / L"film.swf", movie.uncompressed());
    write_text(destination / L"film.xml", movie.export_xml());
    auto report = export_all(movie, destination / L"ressources");
    report.files += 2;
    const auto failed = [&](std::string_view error)
    {
        ++report.failed;
        if (report.errors.size() < 20)
            report.errors.emplace_back(error);
    };
    std::string catalog = "index,code,type,caractère,nom,parent,octets\n";
    std::string procedures = "tag,méthode,classe,nom,signature,octets,exceptions\n";
    std::string types = "tag,classe,membres,fichier\n";
    for (std::size_t i = 0; i < movie.tags().size(); ++i)
    {
        const auto& tag = movie.tags()[i];
        catalog += std::format("{},{},{},{},{},{},{}\n", i, tag.code, csv(tag_name(tag.code)),
            tag.character_id ? std::to_string(*tag.character_id) : "", csv(tag.name),
            tag.parent ? std::to_string(*tag.parent) : "", tag.size);
        if (tag.code == 72 || tag.code == 82)
        {
            try
            {
                const AbcModule abc(abc_bytecode(movie, i));
                for (const auto& method : abc.methods())
                    procedures += std::format("{},{},{},{},{},{},{}\n", i, method.id, csv(method.owner),
                        csv(method.name), csv(method.signature),
                        method.code_size ? std::to_string(*method.code_size) : "", method.handlers);
                report.files += export_classes(abc, destination / L"scripts" / std::format(L"abc-{:06}", i));
                for (std::size_t index = 0; index < abc.classes().size(); ++index)
                {
                    const auto& type = abc.classes()[index];
                    const auto relative = class_path(type);
                    auto file = std::filesystem::path("as3") / relative;
                    if (std::filesystem::exists(destination / file))
                        file = std::filesystem::path("as3-doublons") / std::format("abc-{}-classe-{}", i, index) / relative;
                    try
                    {
                        const auto source = abc.decompile_class(index);
                        std::filesystem::create_directories((destination / file).parent_path());
                        write_text(destination / file, source.text);
                        ++report.files;
                        types += std::format("{},{},{},{}\n", i, csv(type.qualified), type.members.size(), csv(file.generic_string()));
                    }
                    catch (const std::exception& error) { failed(std::format("Classe {} : {}", type.qualified, error.what())); }
                }
            }
            catch (const std::exception& error) { failed(std::format("ABC {} : {}", i, error.what())); }
        }
    }
    write_text(destination / L"tags.csv", catalog);
    write_text(destination / L"methodes-abc.csv", procedures);
    write_text(destination / L"classes-abc.csv", types);
    report.files += 3;

    std::optional<unsigned> omitted;
    try
    {
        const auto preview = render_frame(movie, 1);
        write_file(destination / L"scene.svg", preview.image.bytes);
        omitted = preview.omitted;
        ++report.files;
    }
    catch (const std::exception& error) { failed(std::format("Scène : {}", error.what())); }

    std::string notes = "Projet Flash Studio\n\n"
        "film.swf : copie FWS complète\n"
        "film.xml : structure éditable et réimportable dans Flash Studio\n"
        "tags.csv : inventaire des tags\n"
        "methodes-abc.csv : index des méthodes ActionScript 3\n"
        "classes-abc.csv : index des classes et des fichiers .as\n"
        "as3/ : classes ActionScript organisées en packages. Les corps non reconstruits lèvent une erreur explicite.\n"
        "as3-doublons/ : définitions du même nom conservées à part, si nécessaire\n"
        "scripts/ : méthodes ABC regroupées par classe avec leur source et leur P-code\n"
        "ressources/ : exports et sources reconstituées\n";
    if (omitted)
        notes += std::format("scene.svg : aperçu de la première image, {} éléments ou effets omis\n", *omitted);
    notes += std::format("\n{} fichiers écrits. {} opérations d’export en échec.\n", report.files + 1, report.failed);
    for (const auto& error : report.errors)
        notes += "- " + error + '\n';
    write_text(destination / L"LISEZ-MOI.txt", notes);
    ++report.files;
    return report;
}
}
