#include "../FlashCore/Swf.hpp"
#include "../FlashCore/Abc.hpp"
#include "../FlashCore/Search.hpp"
#include "../FlashCore/ClassEditing.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <iostream>

namespace
{
std::string utf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const auto length = static_cast<int>(value.size());
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), length, result.data(), bytes, nullptr, nullptr);
    return result;
}

std::size_t index_of(std::wstring_view text)
{
    std::string narrow;
    for (const auto digit : text)
    {
        if (digit < L'0' || digit > L'9')
            throw std::runtime_error("Indice de tag invalide.");
        narrow += static_cast<char>(digit);
    }
    std::size_t value{};
    const auto end = narrow.data() + narrow.size();
    const auto result = std::from_chars(narrow.data(), end, value);
    if (result.ec != std::errc{} || result.ptr != end)
        throw std::runtime_error("Indice de tag invalide.");
    return value;
}

double scale_of(std::wstring_view text)
{
    std::string narrow;
    for (const auto character : text)
    {
        if (character > 127)
            throw std::runtime_error("Échelle d’image invalide.");
        narrow += static_cast<char>(character);
    }
    double scale{};
    const auto end = narrow.data() + narrow.size();
    const auto result = std::from_chars(narrow.data(), end, scale);
    if (result.ec != std::errc{} || result.ptr != end)
        throw std::runtime_error("Échelle d’image invalide.");
    return scale;
}

void usage()
{
    std::cout << R"(FlashTool — inspection de fichiers SWF

  FlashTool info <film.swf>
  FlashTool list <film.swf>
  FlashTool search <film.swf> <motif> [--source|--pcode|--text|--constants|--tags] [--regex] [--case]
  FlashTool inspect <film.swf> <index>
  FlashTool disasm <film.swf> <index>
  FlashTool decompile <film.swf> <index>
  FlashTool export <film.swf> <index> <destination>
  FlashTool raw <film.swf> <index> <destination>
  FlashTool unpack <film.swf> <destination.swf>
  FlashTool compress <film.swf> <destination.swf>
  FlashTool export-xml <film.swf> <destination.xml>
  FlashTool import-xml <film.xml> <destination.swf>
  FlashTool frame <film.swf> <image> <destination.svg|.png|.bmp|.jpg|.tiff>
  FlashTool sprite-frame <film.swf> <index> <image> <destination.svg|.png|.bmp|.jpg|.tiff>
  FlashTool sprite-animation <film.swf> <index> <destination.gif|.apng|.svg> [échelle]
  FlashTool class-edit <film.swf> <tag-abc> <classe> <source.as> <copie.swf> <asc.jar>
  FlashTool class-new <film.swf> <classe> <source.as> <copie.swf> <asc.jar>
  FlashTool export-all <film.swf> <dossier>
  FlashTool export-project <film.swf> <dossier vide>
  FlashTool export-html5 <film.swf> <dossier vide> [espace de travail]
  FlashTool export-html5-classes <film.swf> <classes.js>
  FlashTool export-html5-workspace <dossier source> <dossier vide> [film principal]
  FlashTool abc-list <film.swf> <tag>
  FlashTool abc-classes <film.swf> <tag>
  FlashTool abc-class <film.swf> <tag> <classe>
  FlashTool abc-audit <film.swf> <tag>
  FlashTool abc-coverage <film.swf> <tag>
  FlashTool obfuscation-info <film.swf>
  FlashTool deobfuscate <film.swf> <copie.swf> [tag [classe]] [--names] [--strings]
  FlashTool abc-source <film.swf> <tag> <méthode>
  FlashTool abc-pcode <film.swf> <tag> <méthode>
  FlashTool abc-simplify <film.swf> <tag> <méthode>
  FlashTool abc-graph <film.swf> <tag> <méthode>
  FlashTool abc-edit <film.swf> <tag> <méthode> <instructions.txt> <destination.swf>
  FlashTool abc-replace <film.swf> <tag> <bloc.abc> <destination.swf>
  FlashTool abc-string <film.swf> <tag> <constante> <texte.txt> <destination.swf>
  FlashTool image <film.swf> <tag> <destination.svg|.png|.bmp|.jpg|.tiff> [échelle matricielle]
  FlashTool export-visuals <film.swf> <dossier vide> <.svg|.png|.bmp|.jpg|.tiff>
  FlashTool text-svg <film.swf> <tag> <destination.svg>
  FlashTool replace-tag <film.swf> <index> <contenu.bin> <destination.swf>
  FlashTool replace-binary <film.swf> <index> <contenu.bin> <destination.swf>
  FlashTool replace-text <film.swf> <index> <texte.txt> <destination.swf>
  FlashTool replace-image <film.swf> <index> <image> <destination.swf>
  FlashTool replace-sound <film.swf> <index> <audio> <destination.swf>
  FlashTool font-preview <film.swf> <index> <destination.svg>
  FlashTool binary-preview <film.swf> <index>

Les indices commencent à 0. La commande export écrit le format natif ou converti du tag.
)";
}
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 1 || std::wstring_view(argv[1]) == L"--help")
    {
        usage();
        return 0;
    }
    try
    {
        const std::wstring_view command = argv[1];
        if (argc >= 4 && command == L"search")
        {
            flash::SearchOptions options;
            for (int i = 4; i < argc; ++i)
            {
                const std::wstring_view option = argv[i];
                if (option == L"--regex") options.regex = true;
                else if (option == L"--case") options.match_case = true;
                else if (option == L"--source") options.area = flash::SearchArea::source;
                else if (option == L"--pcode") options.area = flash::SearchArea::instructions;
                else if (option == L"--text") options.area = flash::SearchArea::text;
                else if (option == L"--constants") options.area = flash::SearchArea::constants;
                else if (option == L"--tags") options.area = flash::SearchArea::tags;
                else throw std::runtime_error("Option de recherche inconnue.");
            }
            const auto movie = flash::Movie::open(argv[2]);
            const auto report = flash::search_movie(movie, argv[3], options);
            for (const auto& hit : report.hits)
                std::cout << std::format("{}\t{}\t{}\t{}\t{}\t{}\n", hit.tag, utf8(flash::search_area_name(hit.area)),
                    utf8(hit.location), hit.line, hit.column, utf8(hit.excerpt));
            if (report.truncated || report.skipped)
                std::cerr << std::format("{} résultats affichés; {} entrées illisibles{}\n",
                    report.hits.size(), report.skipped, report.truncated ? "; limite atteinte" : "");
        }
        else if (argc == 3 && (command == L"info" || command == L"list"))
        {
            const auto movie = flash::Movie::open(argv[2]);
            if (command == L"info")
                std::cout << flash::summarize(movie);
            else
            {
                for (std::size_t i = 0; i < movie.tags().size(); ++i)
                {
                    const auto& tag = movie.tags()[i];
                    std::cout << std::format("{}\t{}\t{}\t{}\t{}\t{}\n", i, tag.code, flash::tag_name(tag.code), tag.size,
                        tag.parent ? std::to_string(*tag.parent) : "-", tag.name);
                }
            }
        }
        else if (argc == 4 && (command == L"inspect" || command == L"disasm" || command == L"decompile"))
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto index = index_of(argv[3]);
            if (command == L"inspect")
                std::cout << flash::inspect(movie, index) << '\n' << flash::hex_dump(movie.data(index));
            else if (command == L"disasm")
                std::cout << flash::disassemble(movie, index);
            else
                std::cout << flash::decompile(movie, index);
        }
        else if (argc == 4 && command == L"unpack")
        {
            const auto movie = flash::Movie::open(argv[2]);
            flash::write_file(argv[3], movie.uncompressed());
        }
        else if (argc == 4 && command == L"compress")
            flash::write_file(argv[3], flash::Movie::open(argv[2]).compressed());
        else if (argc == 4 && command == L"export-xml")
            flash::write_text(argv[3], flash::Movie::open(argv[2]).export_xml());
        else if ((argc == 4 && (command == L"export-all" || command == L"export-project")) ||
            (argc == 5 && command == L"export-visuals"))
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto report = command == L"export-visuals" ? flash::export_visuals(movie, argv[3], argv[4]) :
                command == L"export-all" ? flash::export_all(movie, argv[3]) : flash::export_project(movie, argv[3]);
            std::cout << std::format("{} fichiers exportés, {} tags en échec.\n", report.files, report.failed);
            for (const auto& error : report.errors)
                std::cerr << error << '\n';
            return report.failed ? 3 : 0;
        }
        else if (argc == 4 && command == L"export-html5-classes")
        {
            flash::write_text(argv[3], flash::html5_classes(flash::Movie::open(argv[2])));
        }
        else if ((argc == 4 || argc == 5) && command == L"export-html5")
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto source = std::filesystem::path(argv[2]);
            const auto report = flash::export_html5(movie, argv[3], utf8(source.stem().wstring()), source,
                argc == 5 ? std::filesystem::path(argv[4]) : std::filesystem::path{});
            std::cout << std::format("Projet HTML5 : {} fichiers et {} points de portage (LISEZ-MOI.txt).\n", report.files, report.failed);
            return report.failed ? 3 : 0;
        }
        else if ((argc == 4 || argc == 5) && command == L"export-html5-workspace")
        {
            const auto report = flash::export_html5_workspace(argv[2], argv[3],
                argc == 5 ? std::filesystem::path(argv[4]) : std::filesystem::path{}, {}, [](auto done, auto total)
                {
                    if (!done || done == total || done % 100 == 0)
                        std::cerr << std::format("HTML5 : {}/{}\n", done, total);
                });
            std::cout << std::format("{} SWF exportés, {} avec des points de portage, {} échecs.\n",
                report.converted, report.partial, report.failed);
            return report.failed ? 3 : 0;
        }
        else if (argc == 4 && command == L"abc-list")
        {
            const auto movie = flash::Movie::open(argv[2]);
            const flash::AbcModule abc(flash::abc_bytecode(movie, index_of(argv[3])));
            for (const auto& method : abc.methods())
                std::cout << std::format("{}\t{}\t{}\t{}\n", method.id, method.owner, method.signature,
                    method.code_size ? std::to_string(*method.code_size) : "-");
        }
        else if (argc == 3 && command == L"obfuscation-info")
        {
            const auto detection = flash::detect_as3_obfuscation(flash::Movie::open(argv[2]));
            std::cout << detection.methods + detection.identifiers << '\n';
        }
        else if (argc >= 4 && argc <= 8 && command == L"deobfuscate")
        {
            const auto destination = std::filesystem::absolute(argv[3]);
            const auto original = std::filesystem::absolute(argv[2]);
            if (CompareStringOrdinal(destination.c_str(), -1, original.c_str(), -1, TRUE) == CSTR_EQUAL ||
                (std::filesystem::exists(destination) && std::filesystem::equivalent(destination, original)))
                throw std::runtime_error("Choisissez une copie du SWF comme destination.");
            const auto movie = flash::Movie::open(original);
            bool strings = false, names = false;
            auto count = argc;
            while (count > 4)
            {
                const std::wstring_view flag = argv[count - 1];
                if (flag == L"--strings" && !strings)
                    strings = true;
                else if (flag == L"--names" && !names)
                    names = true;
                else
                    break;
                --count;
            }
            if (count > 6)
                throw std::runtime_error("Arguments de désobfuscation invalides.");
            const auto tag = count >= 5 ? std::optional{index_of(argv[4])} : std::nullopt;
            const auto type = count == 6 ? std::optional{index_of(argv[5])} : std::nullopt;
            auto result = flash::deobfuscate_as3(movie, tag, type);
            if (names)
            {
                auto renamed = flash::rename_as3_identifiers(result.movie ? *result.movie : movie);
                if (renamed.movie)
                    result.movie = std::move(renamed.movie);
                result.report.renamed_identifiers = renamed.report.renamed_identifiers;
            }
            if (strings)
            {
                auto simplified = flash::simplify_as3_strings(result.movie ? *result.movie : movie, tag, type);
                if (simplified.movie)
                    result.movie = std::move(simplified.movie);
                result.report.resolved_strings = simplified.report.resolved_strings;
            }
            if (result.movie)
            {
                if (movie.header().compression == 'C')
                    flash::write_file(destination, result.movie->compressed());
                else
                    flash::write_file(destination, result.movie->uncompressed());
            }
            const auto& report = result.report;
            std::cout << std::format("{} méthodes nettoyées, {} instructions inaccessibles retirées, {} branchements constants simplifiés, {} méthodes ignorées.\n",
                report.changed_methods, report.removed_instructions, report.constant_branches, report.skipped_methods);
            if (strings)
                std::cout << std::format("{} appels remplacés par des chaînes littérales.\n", report.resolved_strings);
            if (names)
                std::cout << std::format("{} identifiants renommés.\n", report.renamed_identifiers);
        }
        else if (argc == 4 && command == L"abc-audit")
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto original = flash::abc_bytecode(movie, index_of(argv[3]));
            const flash::AbcModule abc(original);
            std::size_t exact = 0, rewritten = 0, failed = 0;
            for (const auto& method : abc.methods())
            {
                if (!method.code_size)
                    continue;
                try
                {
                    const auto rebuilt = abc.replace_method(method.id, abc.pcode(method.id));
                    ++rewritten;
                    if (std::ranges::equal(original, rebuilt))
                        ++exact;
                }
                catch (const std::exception& error)
                {
                    if (++failed <= 12)
                        std::cerr << std::format("Méthode {} : {}\n", method.id, error.what());
                }
            }
            std::cout << std::format("{} corps réassemblés, {} strictement identiques, {} en échec.\n",
                rewritten, exact, failed);
            return failed ? 3 : 0;
        }
        else if (argc == 4 && command == L"abc-coverage")
        {
            const auto movie = flash::Movie::open(argv[2]);
            const flash::AbcModule abc(flash::abc_bytecode(movie, index_of(argv[3])));
            std::size_t source = 0, fallback = 0, declarations = 0;
            for (const auto& method : abc.methods())
                if (method.code_size)
                {
                    if (abc.declares_classes(method.id))
                    {
                        ++declarations;
                        continue;
                    }
                    if (const auto error = abc.source_problem(method.id))
                    {
                        ++fallback;
                        std::cout << std::format("{}\t{}\t{}\n", method.id, method.owner, *error);
                    }
                    else
                        ++source;
                }
            std::cout << std::format("{} corps reconstruits, {} scripts d’initialisation restitués en déclarations, {} replis vers le P-code.\n",
                source, declarations, fallback);
        }
        else if ((argc == 4 && command == L"abc-classes") || (argc == 5 && command == L"abc-class"))
        {
            const auto movie = flash::Movie::open(argv[2]);
            const flash::AbcModule abc(flash::abc_bytecode(movie, index_of(argv[3])));
            if (command == L"abc-class")
                std::cout << abc.decompile_class(index_of(argv[4])).text;
            else
                for (std::size_t i = 0; i < abc.classes().size(); ++i)
                    std::cout << std::format("{}\t{}\t{}\n", i, abc.classes()[i].qualified, abc.classes()[i].members.size());
        }
        else if ((argc == 5 && (command == L"abc-pcode" || command == L"abc-simplify" || command == L"abc-graph" ||
            command == L"abc-source" || command == L"image" || command == L"text-svg")) || (argc == 6 && command == L"image"))
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto tag = index_of(argv[3]);
            if (command == L"text-svg")
                flash::write_file(argv[4], flash::text_svg(movie, tag).bytes);
            else if (command == L"image")
            {
                const auto extension = std::filesystem::path(argv[4]).extension().wstring();
                const auto scale = argc == 6 ? scale_of(argv[5]) : 1.0;
                flash::write_file(argv[4], flash::convert_image(flash::visual_asset(movie, tag), extension, scale).bytes);
            }
            else
            {
                const auto method = index_of(argv[4]);
                if (method >= (1u << 30))
                    throw std::runtime_error("Numéro de méthode hors limites.");
                const flash::AbcModule abc(flash::abc_bytecode(movie, tag));
                const auto id = static_cast<unsigned>(method);
                if (command == L"abc-graph")
                    std::cout << abc.flow_graph(id);
                else if (command == L"abc-source")
                    std::cout << abc.decompile(id);
                else
                    std::cout << abc.pcode(id, command == L"abc-simplify");
            }
        }
        else if (argc == 6 && command == L"abc-replace")
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto updated = flash::replace_abc(movie, index_of(argv[3]), flash::read_file(argv[4]));
            flash::write_file(argv[5], updated.uncompressed());
        }
        else if (argc == 7 && (command == L"abc-edit" || command == L"abc-string"))
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto tag = index_of(argv[3]), index = index_of(argv[4]);
            if (index >= (1u << 30))
                throw std::runtime_error("Indice ABC hors limites.");
            const flash::AbcModule abc(flash::abc_bytecode(movie, tag));
            const auto input = flash::read_file(argv[5]);
            auto text = input.empty() ? std::string_view{} :
                std::string_view(reinterpret_cast<const char*>(input.data()), input.size());
            if (text.starts_with("\xEF\xBB\xBF"))
                text.remove_prefix(3);
            const auto compiled = command == L"abc-edit" ? abc.replace_method(static_cast<unsigned>(index), text) :
                abc.replace_string(static_cast<unsigned>(index), text);
            const auto updated = flash::replace_abc(movie, tag, compiled);
            flash::write_file(argv[6], updated.uncompressed());
        }
        else if (argc == 4 && command == L"import-xml")
        {
            const auto movie = flash::Movie::import_xml(flash::read_file(argv[2]));
            flash::write_file(argv[3], movie.uncompressed());
        }
        else if ((argc == 8 && command == L"class-edit") || (argc == 7 && command == L"class-new"))
        {
            const bool editing = command == L"class-edit";
            const int name = editing ? 4 : 3;
            const auto movie = flash::Movie::open(argv[2]);
            const auto destination = std::filesystem::absolute(argv[name + 2]);
            if (CompareStringOrdinal(destination.c_str(), -1, std::filesystem::absolute(argv[2]).c_str(), -1, TRUE) == CSTR_EQUAL ||
                (std::filesystem::exists(destination) && std::filesystem::equivalent(destination, argv[2])))
                throw std::runtime_error("Choisissez une copie du SWF comme destination.");
            const auto raw = flash::read_file(argv[name + 1]);
            const std::string source(raw.begin(), raw.end());
            const auto compiler = std::filesystem::absolute(argv[name + 3]);
            const auto compiled = editing ? flash::compile_as3_class(movie, index_of(argv[3]), utf8(argv[name]), source, compiler) :
                flash::add_as3_class(movie, utf8(argv[name]), source, compiler);
            if (movie.header().compression == 'C')
                flash::write_file(destination, compiled.compressed());
            else
                flash::write_file(destination, compiled.uncompressed());
        }
        else if ((argc == 5 || argc == 6) && command == L"sprite-animation")
        {
            const auto path = std::filesystem::path(argv[4]);
            auto extension = path.extension().wstring();
            if (!extension.empty())
                CharLowerBuffW(extension.data(), static_cast<DWORD>(extension.size()));
            const auto formats = std::array{flash::AnimationFormat::gif, flash::AnimationFormat::apng, flash::AnimationFormat::svg};
            const auto format = std::ranges::find_if(formats, [&](auto item) { return flash::animation_extension(item) == extension; });
            if (format == formats.end())
                throw std::runtime_error("Formats d’animation disponibles : GIF, APNG et SVG.");
            const auto movie = flash::Movie::open(argv[2]);
            const auto animation = flash::export_sprite_animation(movie, index_of(argv[3]), *format, argc == 6 ? scale_of(argv[5]) : 1);
            flash::write_file(path, animation.image.bytes);
            if (animation.frames_with_omissions)
                std::cerr << std::format("{} images avec des éléments ou effets omis.\n", animation.frames_with_omissions);
        }
        else if ((argc == 5 && command == L"frame") || (argc == 6 && command == L"sprite-frame"))
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto sprite = argc == 6 ? std::optional{index_of(argv[3])} : std::nullopt;
            const auto number = index_of(argv[argc - 2]);
            if (number > 65535)
                throw std::runtime_error("Numéro d’image invalide.");
            const auto frame = flash::render_frame(movie, static_cast<unsigned>(number), sprite);
            const auto extension = std::filesystem::path(argv[argc - 1]).extension().wstring();
            flash::write_file(argv[argc - 1], flash::convert_image(frame.image, extension.empty() ? L".svg" : extension).bytes);
            if (frame.omitted)
                std::cerr << std::format("Rendu partiel : {} éléments ou effets omis.\n", frame.omitted);
        }
        else if (argc == 5 && (command == L"export" || command == L"raw"))
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto index = index_of(argv[3]);
            if (command == L"raw")
                flash::write_file(argv[4], movie.data(index));
            else if (const auto asset = flash::extract(movie, index))
                flash::write_file(argv[4], asset->bytes);
            else
                throw std::runtime_error("Aucun export spécialisé pour ce type de tag.");
        }
        else if (argc == 5 && command == L"font-preview")
        {
            const auto preview = flash::font_svg(flash::Movie::open(argv[2]), index_of(argv[3]));
            flash::write_file(argv[4], preview.bytes);
        }
        else if (argc == 4 && command == L"binary-preview")
            std::cout << flash::binary_preview(flash::Movie::open(argv[2]), index_of(argv[3]));
        else if (argc == 6 && command == L"replace-sound")
        {
            const auto movie = flash::Movie::open(argv[2]).replace_sound(index_of(argv[3]), argv[4]);
            flash::write_file(argv[5], movie.uncompressed());
        }
        else if (argc == 6 && (command == L"replace-binary" || command == L"replace-tag" || command == L"replace-text" ||
            command == L"replace-image"))
        {
            const auto movie = flash::Movie::open(argv[2]);
            const auto index = index_of(argv[3]);
            const auto content = flash::read_file(argv[4]);
            const auto updated = [&]
            {
                if (command == L"replace-binary")
                    return movie.replace_binary(index, content);
                if (command == L"replace-tag")
                    return movie.replace_tag(index, content);
                if (command == L"replace-image")
                    return movie.replace_image(index, content);
                const std::string_view text = content.empty() ? std::string_view{} :
                    std::string_view(reinterpret_cast<const char*>(content.data()), content.size());
                return movie.replace_text(index, text);
            }();
            flash::write_file(argv[5], updated.uncompressed());
        }
        else
        {
            usage();
            return 2;
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "Erreur : " << error.what() << '\n';
        return 1;
    }
}
