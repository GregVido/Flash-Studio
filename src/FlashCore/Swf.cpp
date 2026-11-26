#include "Binary.hpp"
#include "Compression.hpp"

#include <Windows.h>
#include <array>
#include <atomic>
#include <format>
#include <fstream>
#include <unordered_map>

namespace flash
{
namespace
{
using enum Category;
struct TagType { unsigned code; std::string_view name; Category category; bool character = false; };
constexpr TagType types[]{
    {0,"End",timeline}, {1,"ShowFrame",timeline}, {2,"DefineShape",shapes,true},
    {4,"PlaceObject",timeline}, {5,"RemoveObject",timeline}, {6,"DefineBits",images,true},
    {7,"DefineButton",sprites,true}, {8,"JPEGTables",images}, {9,"SetBackgroundColor",timeline},
    {10,"DefineFont",fonts,true}, {11,"DefineText",text,true}, {12,"DoAction",scripts},
    {13,"DefineFontInfo",fonts}, {14,"DefineSound",sounds,true}, {15,"StartSound",sounds},
    {17,"DefineButtonSound",sounds}, {18,"SoundStreamHead",sounds}, {19,"SoundStreamBlock",sounds},
    {20,"DefineBitsLossless",images,true}, {21,"DefineBitsJPEG2",images,true},
    {22,"DefineShape2",shapes,true}, {23,"DefineButtonCxform",sprites}, {24,"Protect",other},
    {26,"PlaceObject2",timeline}, {28,"RemoveObject2",timeline}, {32,"DefineShape3",shapes,true},
    {33,"DefineText2",text,true}, {34,"DefineButton2",sprites,true}, {35,"DefineBitsJPEG3",images,true},
    {36,"DefineBitsLossless2",images,true}, {37,"DefineEditText",text,true},
    {39,"DefineSprite",sprites,true}, {40,"NameCharacter",other}, {41,"ProductInfo",other},
    {43,"FrameLabel",timeline}, {45,"SoundStreamHead2",sounds},
    {46,"DefineMorphShape",shapes,true}, {48,"DefineFont2",fonts,true}, {56,"ExportAssets",other},
    {57,"ImportAssets",other}, {58,"EnableDebugger",other}, {59,"DoInitAction",scripts},
    {60,"DefineVideoStream",other,true}, {61,"VideoFrame",other}, {62,"DefineFontInfo2",fonts},
    {63,"DebugID",other}, {64,"EnableDebugger2",other}, {65,"ScriptLimits",scripts},
    {66,"SetTabIndex",timeline}, {69,"FileAttributes",other}, {70,"PlaceObject3",timeline},
    {71,"ImportAssets2",other}, {72,"DoABC",scripts}, {73,"DefineFontAlignZones",fonts},
    {74,"CSMTextSettings",text}, {75,"DefineFont3",fonts,true}, {76,"SymbolClass",other},
    {77,"Metadata",other}, {78,"DefineScalingGrid",sprites}, {82,"DoABC2",scripts},
    {83,"DefineShape4",shapes,true}, {84,"DefineMorphShape2",shapes,true},
    {86,"DefineSceneAndFrameLabelData",timeline}, {87,"DefineBinaryData",binary,true},
    {88,"DefineFontName",fonts}, {89,"StartSound2",sounds}, {90,"DefineBitsJPEG4",images,true},
    {91,"DefineFont4",fonts,true}, {93,"EnableTelemetry",other}, {94,"PlaceObject4",timeline}
};
constexpr TagType unknown{0, "Tag inconnu", other};

const TagType& type_for(unsigned code)
{
    static constexpr auto by_code = []
    {
        std::array<const TagType*, 1024> map{};
        map.fill(&unknown);
        for (const auto& type : types)
            map[type.code] = &type;
        return map;
    }();
    return code < by_code.size() ? *by_code[code] : unknown;
}

void put_tag_header(Bytes& bytes, unsigned code, std::size_t size, bool long_header)
{
    const auto short_size = long_header ? std::size_t{63} : (std::min)(std::size_t{63}, size);
    put16(bytes, static_cast<std::uint16_t>((code << 6) | short_size));
    if (short_size == 63)
        put32(bytes, static_cast<std::uint32_t>(size));
}
}

std::wstring_view category_name(Category category)
{
    constexpr std::wstring_view names[]{L"Chronologie", L"Scripts", L"Images", L"Formes", L"Sons",
        L"Textes", L"Polices", L"Sprites", L"Données binaires", L"Autres"};
    return names[static_cast<std::size_t>(category)];
}

std::string_view tag_name(unsigned code) { return type_for(code).name; }

Bytes read_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Impossible d’ouvrir le fichier.");
    const auto size = static_cast<std::streamoff>(file.tellg());
    if (size < 0 || size > static_cast<std::streamoff>(max_file_size))
        throw std::runtime_error("La taille maximale prise en charge est de 256 Mio.");
    Bytes bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size))
        throw std::runtime_error("Lecture du fichier incomplète.");
    return bytes;
}

void write_file(const std::filesystem::path& path, ByteView bytes)
{
    static std::atomic<unsigned> sequence{0};
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    do
    {
        temporary = path.parent_path() / std::format(L".flashstudio-{}-{}.tmp", GetCurrentProcessId(), ++sequence);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
            throw std::runtime_error("Impossible de créer le fichier de destination.");
    } while (file == INVALID_HANDLE_VALUE);

    DWORD count{};
    const bool saved = bytes.empty() || (bytes.size() <= MAXDWORD &&
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size());
    CloseHandle(file);
    if (!saved || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Impossible d’enregistrer le fichier.");
    }
}

void write_text(const std::filesystem::path& path, std::string_view text)
{
    write_file(path, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

Movie Movie::open(const std::filesystem::path& path)
{
    auto bytes = read_file(path);
    return decode(bytes, std::move(bytes));
}

Movie Movie::parse(ByteView bytes) { return decode(bytes); }

Movie Movie::decode(ByteView bytes, Bytes&& owned)
{
    if (bytes.size() < 8 || bytes[1] != 'W' || bytes[2] != 'S' ||
        (bytes[0] != 'F' && bytes[0] != 'C' && bytes[0] != 'Z'))
        throw std::runtime_error("Signature SWF invalide. Formats reconnus : FWS, CWS et ZWS.");
    if (bytes.size() > max_file_size)
        throw std::runtime_error("La taille maximale prise en charge est de 256 Mio.");
    Reader input(bytes);
    input.skip(4);
    const auto size = input.u32();
    if (size < 15 || size > max_file_size)
        throw std::runtime_error("Taille SWF déclarée invalide ou supérieure à 256 Mio.");

    Movie movie;
    auto& header = movie.header_;
    header.compression = static_cast<char>(bytes.front());
    header.version = bytes[3];
    header.stored_size = bytes.size();
    header.expanded_size = size;
    if (header.compression == 'F')
    {
        input.take(size - 8);
        if (owned.empty())
            append(movie.bytes_, bytes.first(size));
        else
        {
            owned.resize(size);
            movie.bytes_ = std::move(owned);
        }
    }
    else
    {
        movie.bytes_.resize(size);
        std::ranges::copy(bytes.first(8), movie.bytes_.begin());
        auto body = std::span{movie.bytes_}.subspan(8);
        if (header.compression == 'C')
            inflate_zlib(input.take(input.remaining()), body);
        else
        {
            const auto packed_size = input.u32();
            const auto props = input.take(5);
            const auto packed = input.take(input.remaining());
            // Both conventions occur in published SWFs.
            if (packed_size != packed.size() && packed_size != packed.size() + props.size())
                throw std::runtime_error("Taille du flux LZMA incorrecte.");
            inflate_lzma(props, packed, body);
        }
        movie.bytes_.front() = 'F';
    }

    Reader fields(ByteView{movie.bytes_}.subspan(8));
    BitReader rect(fields);
    const auto bits = rect.unsigned_bits(5);
    header.left = rect.signed_bits(bits);
    header.right = rect.signed_bits(bits);
    header.top = rect.signed_bits(bits);
    header.bottom = rect.signed_bits(bits);
    header.frames_per_second = fields.u16() / 256.0;
    header.frame_count = fields.u16();
    movie.tag_offset_ = 8 + fields.position();
    movie.read_tags(movie.tag_offset_, movie.bytes_.size(), {}, 0);
    movie.resolve_names();
    return movie;
}

void Movie::read_tags(std::size_t start, std::size_t end, std::optional<std::size_t> parent, unsigned depth)
{
    if (depth > 64)
        throw std::runtime_error("Trop de niveaux de sprites imbriqués.");
    Reader input(ByteView{bytes_}.subspan(start, end - start));
    while (input.remaining())
    {
        if (tags_.size() == 250'000)
            throw std::runtime_error("Ce SWF dépasse la limite de 250 000 tags.");
        const auto header_offset = start + input.position();
        const auto record = input.u16();
        const auto code = static_cast<unsigned>(record >> 6);
        const auto short_size = static_cast<unsigned>(record & 63);
        const auto size = short_size == 63 ? input.u32() : short_size;
        const auto offset = start + input.position();
        const auto payload = input.take(size);
        const auto& type = type_for(code);
        const auto index = tags_.size();
        Tag tag{code, type.category, header_offset, offset, size, parent, {}, {}, index + 1};
        if (type.character && payload.size() >= 2)
            tag.character_id = Reader(payload).u16();
        tags_.push_back(std::move(tag));
        if (code == 0)
        {
            if (size != 0)
                throw std::runtime_error("Tag End invalide.");
            return;
        }
        if (code == 39)
        {
            if (size < 4)
                throw std::runtime_error("En-tête DefineSprite tronqué.");
            read_tags(offset + 4, offset + size, index, depth + 1);
            tags_[index].next_sibling = tags_.size();
        }
    }
    throw std::runtime_error("Le flux SWF ne contient pas de tag End.");
}

void Movie::resolve_names()
{
    std::unordered_map<unsigned, std::string> symbols;
    for (std::size_t i = 0; i < tags_.size(); ++i)
    {
        auto& tag = tags_[i];
        Reader input(data(i));
        try
        {
            if (tag.code == 56 || tag.code == 76)
            {
                const auto count = input.u16();
                for (unsigned j = 0; j < count; ++j)
                {
                    const auto id = input.u16();
                    auto name = input.cstring();
                    if (name.size() <= 4096)
                    {
                        if (!id && tag.code == 76 && !tag.parent && !name.empty())
                            document_class_ = name;
                        symbols[id] = std::move(name);
                    }
                }
            }
            if (tag.code == 40)
            {
                const auto id = input.u16();
                auto name = input.cstring();
                if (!name.empty() && name.size() <= 4096)
                    symbols.try_emplace(id, std::move(name));
            }
            if (tag.code == 82)
                input.skip(4);
            if (tag.code == 82 || tag.code == 43)
            {
                tag.name = input.cstring();
                if (tag.name.size() > 4096)
                    tag.name.clear();
            }
        }
        catch (const std::runtime_error&)
        {
            // Invalid metadata should not prevent the raw tag from being inspected.
        }
    }
    for (std::size_t i = 0; i < tags_.size(); ++i)
    {
        auto& tag = tags_[i];
        if (tag.character_id)
        {
            characters_.try_emplace(*tag.character_id, i);
            if (const auto found = symbols.find(*tag.character_id); found != symbols.end())
                tag.name = found->second;
        }
    }
}

std::optional<std::size_t> Movie::character(unsigned id) const
{
    const auto found = characters_.find(id);
    return found == characters_.end() ? std::nullopt : std::optional{found->second};
}

ByteView Movie::data(std::size_t index) const
{
    const auto& tag = tags_.at(index);
    return ByteView{bytes_}.subspan(tag.offset, tag.size);
}

Movie Movie::replace_binary(std::size_t index, ByteView replacement) const
{
    const auto& target = tags_.at(index);
    if (target.code != 87 || target.size < 6)
        throw std::runtime_error("Seul le contenu d’un tag DefineBinaryData peut être remplacé.");
    if (replacement.size() > max_file_size - 6)
        throw std::runtime_error("Le remplacement est trop volumineux.");

    Bytes payload;
    payload.reserve(replacement.size() + 6);
    append(payload, data(index).first(6));
    append(payload, replacement);
    return replace_tag(index, payload);
}

Movie Movie::replace_tag(std::size_t index, ByteView replacement, std::optional<unsigned> new_code) const
{
    const auto& target = tags_.at(index);
    if (replacement.size() > max_file_size || target.code == 0 ||
        (new_code && (!*new_code || *new_code >= 1024 || *new_code == 39 || target.code == 39)))
        throw std::runtime_error("Le contenu de ce tag ne peut pas être remplacé.");
    std::vector<std::pair<std::size_t, std::size_t>> edits;
    std::size_t new_size = replacement.size();
    for (std::optional current{index}; current; current = tags_[*current].parent)
    {
        const auto& tag = tags_[*current];
        edits.emplace_back(*current, new_size);
        const auto new_record_size = new_size + (new_size >= 63 || tag.offset - tag.header_offset == 6 ? 6 : 2);
        const auto old_record_size = tag.offset + tag.size - tag.header_offset;
        const auto parent_size = tag.parent ? tags_[*tag.parent].size : bytes_.size();
        new_size = parent_size - old_record_size + new_record_size;
    }
    if (new_size > max_file_size)
        throw std::runtime_error("Le SWF modifié dépasse 256 Mio.");

    Bytes updated;
    updated.reserve(new_size);
    const ByteView all{bytes_};
    std::size_t cursor = 0;
    for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit)
    {
        const auto& tag = tags_[edit->first];
        append(updated, all.subspan(cursor, tag.header_offset - cursor));
        const auto code = edit->first == index ? new_code.value_or(tag.code) : tag.code;
        put_tag_header(updated, code, edit->second, tag.offset - tag.header_offset == 6);
        cursor = tag.offset;
    }
    append(updated, replacement);
    append(updated, all.subspan(target.offset + target.size));
    for (unsigned i = 0; i < 4; ++i)
        updated[4 + i] = static_cast<std::uint8_t>(updated.size() >> (8 * i));
    return decode(updated, std::move(updated));
}

Movie Movie::insert_root_tag(std::size_t before, unsigned code, ByteView payload) const
{
    const auto& next = tags_.at(before);
    if (next.parent || !code || code > 1023 || max_file_size - bytes_.size() < 6 ||
        payload.size() > max_file_size - bytes_.size() - 6)
        throw std::runtime_error("Invalid root SWF tag insertion.");
    Bytes updated;
    const ByteView all{bytes_};
    updated.reserve(all.size() + payload.size() + 6);
    append(updated, all.first(next.header_offset));
    put_tag_header(updated, code, payload.size(), false);
    append(updated, payload);
    append(updated, all.subspan(next.header_offset));
    for (unsigned i = 0; i < 4; ++i)
        updated[4 + i] = static_cast<std::uint8_t>(updated.size() >> (8 * i));
    return decode(updated, std::move(updated));
}

Bytes Movie::compressed() const
{
    Bytes result;
    const auto header = uncompressed().first(8);
    const auto packed = deflate_zlib(uncompressed().subspan(8));
    result.reserve(packed.size() + header.size());
    append(result, header);
    append(result, packed);
    result[0] = 'C';
    result[3] = (std::max)(std::uint8_t{6}, result[3]);
    return result;
}

std::string hex_dump(ByteView bytes, std::size_t limit)
{
    std::string output;
    const auto count = (std::min)(bytes.size(), limit);
    for (std::size_t offset = 0; offset < count; offset += 16)
    {
        output += std::format("{:08X}  ", offset);
        std::string ascii;
        for (std::size_t i = offset; i < offset + 16; ++i)
        {
            if (i == offset + 8)
                output += ' ';
            if (i >= count)
            {
                output += "   ";
                continue;
            }
            const auto byte = bytes[i];
            output += std::format("{:02X} ", byte);
            ascii += byte >= 32 && byte < 127 ? static_cast<char>(byte) : '.';
        }
        output += " │ " + ascii + '\n';
    }
    if (bytes.size() > limit)
        output += std::format("\n… {} octets supplémentaires.\n", bytes.size() - limit);
    return output;
}

std::string inspect(const Movie& movie, std::size_t index)
{
    const auto& tag = movie.tags().at(index);
    auto detail = std::format("{} ({})\nPosition dans le SWF décompressé : 0x{:X}\nTaille du contenu : {} octets\n",
        tag_name(tag.code), tag.code, tag.offset, tag.size);
    if (tag.character_id)
        detail += std::format("ID du caractère : {}\n", *tag.character_id);
    if (!tag.name.empty())
        detail += std::format("Nom : {}\n", tag.name);
    if (tag.parent)
        detail += std::format("Sprite parent : tag {}\n", *tag.parent);
    try
    {
        Reader input(movie.data(index));
        const auto entries = [&](std::string_view title)
        {
            const auto count = input.v32();
            detail += std::format("{} : {}\n", title, count);
            if (count > input.remaining() / 2)
                throw std::runtime_error("Nombre d’entrées du tag invalide.");
            for (unsigned i = 0; i < count; ++i)
            {
                const auto frame = input.v32();
                const auto label = input.cstring();
                if (i < 128)
                    detail += std::format("  Image {} · {:.200s}\n", static_cast<std::uint64_t>(frame) + 1, label);
            }
            if (count > 128)
                detail += std::format("  … {} autres\n", count - 128);
        };
        switch (tag.code)
        {
        case 4: case 5:
        {
            const auto id = input.u16();
            detail += std::format("Caractère : {} · profondeur : {}\n", id, input.u16());
            break;
        }
        case 9:
        {
            const auto r = input.u8(), g = input.u8(), b = input.u8();
            detail += std::format("Couleur de fond : #{:02X}{:02X}{:02X}\n", r, g, b);
            break;
        }
        case 18: case 45:
        {
            input.skip(1);
            const auto flags = input.u8();
            constexpr unsigned rates[]{5512, 11025, 22050, 44100};
            const auto samples = input.u16();
            detail += std::format("Compression audio : {} · {} Hz · {} canaux\nÉchantillons par bloc : {}\n",
                flags >> 4, rates[(flags >> 2) & 3], 1 + (flags & 1), samples);
            if ((flags >> 4) == 2)
                detail += std::format("Décalage MP3 : {} échantillons\n", std::bit_cast<std::int16_t>(input.u16()));
            break;
        }
        case 26: case 70: case 94:
        {
            const auto flags = input.u8(), extra = tag.code == 26 ? std::uint8_t{0} : input.u8();
            detail += std::format("Profondeur : {} · déplacement : {}\n", input.u16(), flags & 1 ? "oui" : "non");
            if (extra & 8)
                detail += std::format("Classe : {:.200s}\n", input.cstring());
            if (flags & 2)
                detail += std::format("Caractère : {}\n", input.u16());
            break;
        }
        case 28: detail += std::format("Profondeur : {}\n", input.u16()); break;
        case 39:
            input.skip(2);
            detail += std::format("Images du sprite : {}\n", input.u16());
            break;
        case 43:
            input.cstring();
            if (input.remaining())
                detail += std::format("Ancre nommée : {}\n", input.u8() ? "oui" : "non");
            break;
        case 56: case 57: case 71: case 76:
        {
            if (tag.code == 57 || tag.code == 71)
                detail += std::format("Fichier source : {:.200s}\n", input.cstring());
            if (tag.code == 71)
                input.skip(2);
            const auto count = input.u16();
            detail += std::format("Symboles : {}\n", count);
            for (unsigned i = 0; i < count && i < 128; ++i)
            {
                const auto id = input.u16();
                detail += std::format("  #{} · {:.200s}\n", id, input.cstring());
            }
            if (count > 128)
                detail += std::format("  … {} autres\n", count - 128);
            break;
        }
        case 60:
        {
            input.skip(2);
            const auto frames = input.u16(), width = input.u16(), height = input.u16();
            const auto flags = input.u8(), codec = input.u8();
            detail += std::format("Vidéo : {} × {} px · {} images · codec {}\nLissage : {} · filtrage : {}\n",
                width, height, frames, codec, flags & 1 ? "oui" : "non", (flags >> 1) & 7);
            break;
        }
        case 61:
        {
            const auto id = input.u16();
            const auto frame = input.u16();
            detail += std::format("Flux vidéo #{} · image {} · {} octets encodés\n", id, frame, input.remaining());
            break;
        }
        case 65:
        {
            const auto depth = input.u16();
            detail += std::format("Profondeur de récursion : {} · délai maximal : {} s\n", depth, input.u16());
            break;
        }
        case 69:
        {
            const auto flags = input.u32();
            detail += std::format("ActionScript 3 : {} · réseau : {} · métadonnées : {}\n",
                flags & 8 ? "oui" : "non", flags & 1 ? "oui" : "non", flags & 16 ? "oui" : "non");
            break;
        }
        case 86:
            entries("Scènes");
            entries("Étiquettes d’image");
            break;
        default: break;
        }
    }
    catch (const std::runtime_error& error) { detail += std::format("Lecture des champs interrompue : {}\n", error.what()); }
    return detail;
}

std::string summarize(const Movie& movie)
{
    const auto& h = movie.header();
    auto summary = std::format("{}WS • SWF {}\nScène : {:g} × {:g} px, origine ({:g}, {:g})\n"
        "Cadence : {:g} i/s · {} images\nTaille stockée : {} octets\n"
        "Taille décompressée : {} octets\nTags : {}\n", h.compression, h.version,
        (static_cast<double>(h.right) - h.left) / 20.0, (static_cast<double>(h.bottom) - h.top) / 20.0,
        h.left / 20.0, h.top / 20.0,
        h.frames_per_second, h.frame_count, h.stored_size, h.expanded_size, movie.tags().size());
    if (movie.document_class())
        summary += std::format("Classe du document : {}\n", *movie.document_class());
    return summary;
}
}
