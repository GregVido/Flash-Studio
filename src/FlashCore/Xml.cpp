#include "Binary.hpp"

#include <Windows.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <wrl/client.h>
#include <xmllite.h>
#include <format>

#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "XmlLite.lib")

namespace flash
{
namespace
{
using Microsoft::WRL::ComPtr;

void xml_check(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("Document XML SWF invalide.");
}

}

std::string encode_base64(ByteView input)
{
    if (input.empty())
        return {};
    const auto length = static_cast<DWORD>(input.size());
    constexpr auto options = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD size{};
    if (!CryptBinaryToStringA(input.data(), length, options, nullptr, &size))
        throw std::runtime_error("Impossible d’encoder les données du tag.");
    std::string text(size, '\0');
    if (!CryptBinaryToStringA(input.data(), length, options, text.data(), &size))
        throw std::runtime_error("Impossible d’encoder les données du tag.");
    text.resize(size);
    return text;
}

namespace
{
Bytes unbase64(std::wstring_view text)
{
    if (text.empty())
        return {};
    constexpr auto options = CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT;
    const auto length = static_cast<DWORD>(text.size());
    DWORD size{};
    if (!CryptStringToBinaryW(text.data(), length, options, nullptr, &size, nullptr, nullptr) || size > max_file_size)
        throw std::runtime_error("Données base64 invalides dans le document XML SWF.");
    Bytes output(size);
    if (!CryptStringToBinaryW(text.data(), length, options, output.data(), &size, nullptr, nullptr))
        throw std::runtime_error("Données base64 invalides dans le document XML SWF.");
    output.resize(size);
    return output;
}

std::wstring node_text(IXmlReader& xml)
{
    LPCWSTR text{};
    UINT length{};
    xml_check(xml.GetValue(&text, &length));
    return {text, length};
}

std::wstring_view node_name(IXmlReader& xml)
{
    LPCWSTR text{};
    UINT length{};
    xml_check(xml.GetLocalName(&text, &length));
    return {text, length};
}

std::wstring attribute(IXmlReader& xml, const wchar_t* name)
{
    if (xml.MoveToAttributeByName(name, L"") != S_OK)
        throw std::runtime_error("Attribut manquant dans le document XML SWF.");
    auto value = node_text(xml);
    xml_check(xml.MoveToElement());
    return value;
}

unsigned number(std::wstring_view text, unsigned max)
{
    if (text.empty() || text.size() > 4)
        throw std::runtime_error("Entier invalide dans le document XML SWF.");
    unsigned value = 0;
    for (const auto c : text)
    {
        if (c < L'0' || c > L'9')
            throw std::runtime_error("Entier invalide dans le document XML SWF.");
        value = 10 * value + static_cast<unsigned>(c - L'0');
    }
    if (value > max)
        throw std::runtime_error("Entier hors limites dans le document XML SWF.");
    return value;
}

bool whitespace(XmlNodeType type)
{
    return type == XmlNodeType_Whitespace || type == XmlNodeType_Comment || type == XmlNodeType_XmlDeclaration;
}

void add_tag(Bytes& bytes, unsigned code, ByteView payload, bool long_header)
{
    const auto short_size = long_header ? std::size_t{63} : std::min(std::size_t{63}, payload.size());
    if (bytes.size() > max_file_size - 6)
        throw std::runtime_error("Le SWF issu du XML dépasse 256 Mio.");
    put16(bytes, static_cast<std::uint16_t>((code << 6) | short_size));
    if (short_size == 63)
        put32(bytes, static_cast<std::uint32_t>(payload.size()));
    append(bytes, payload);
}
}

std::string Movie::export_xml() const
{
    const auto frame = uncompressed().subspan(8, tag_offset_ - 8);
    auto xml = std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<swf version=\"{}\" frame-header=\"{}\">\n", header_.version, encode_base64(frame));
    for (std::size_t i = 0; i < tags_.size(); ++i)
    {
        const auto& tag = tags_[i];
        if (tag.parent)
            continue;
        if ((tag.size + 2) / 3 * 4 + 64 > max_file_size - xml.size())
            throw std::runtime_error("Le document XML dépasse 256 Mio.");
        xml += std::format("  <tag code=\"{}\" long=\"{}\">{}</tag>\n", tag.code,
            tag.offset - tag.header_offset == 6 ? 1 : 0, encode_base64(data(i)));
    }
    xml += "</swf>\n";
    return xml;
}

Movie Movie::import_xml(ByteView bytes)
{
    if (bytes.empty() || bytes.size() > max_file_size)
        throw std::runtime_error("Taille du document XML SWF invalide.");
    ComPtr<IStream> input;
    input.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    if (!input)
        throw std::runtime_error("Impossible de lire le document XML SWF.");
    ComPtr<IXmlReader> xml;
    xml_check(CreateXmlReader(IID_PPV_ARGS(&xml), nullptr));
    xml_check(xml->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit));
    xml_check(xml->SetProperty(XmlReaderProperty_MaxElementDepth, 3));
    xml_check(xml->SetInput(input.Get()));

    Bytes movie;
    bool closed = false;
    XmlNodeType type{};
    HRESULT next{};
    while ((next = xml->Read(&type)) == S_OK)
    {
        if (whitespace(type))
            continue;
        if (movie.empty())
        {
            if (type != XmlNodeType_Element || xml->IsEmptyElement() || node_name(*xml.Get()) != L"swf")
                throw std::runtime_error("Élément swf attendu dans le document XML.");
            const auto version = number(attribute(*xml.Get(), L"version"), 255);
            const auto frame = unbase64(attribute(*xml.Get(), L"frame-header"));
            Reader frame_input(frame);
            skip_rect(frame_input);
            if (frame_input.remaining() != 4)
                throw std::runtime_error("En-tête d’image XML SWF invalide.");
            movie = {'F', 'W', 'S', static_cast<std::uint8_t>(version), 0, 0, 0, 0};
            append(movie, frame);
            continue;
        }
        if (!closed && type == XmlNodeType_EndElement && node_name(*xml.Get()) == L"swf")
        {
            closed = true;
            continue;
        }
        if (closed || type != XmlNodeType_Element || node_name(*xml.Get()) != L"tag")
            throw std::runtime_error("Élément tag attendu dans le document XML SWF.");
        const auto code = number(attribute(*xml.Get(), L"code"), 1023);
        const auto long_header = number(attribute(*xml.Get(), L"long"), 1) != 0;
        Bytes data;
        if (!xml->IsEmptyElement())
        {
            std::wstring encoded;
            while ((next = xml->Read(&type)) == S_OK && type != XmlNodeType_EndElement)
            {
                if (type == XmlNodeType_Text || type == XmlNodeType_CDATA || type == XmlNodeType_Whitespace)
                    encoded += node_text(*xml.Get());
                else if (type != XmlNodeType_Comment)
                    throw std::runtime_error("Contenu inattendu dans un tag XML SWF.");
            }
            if (next != S_OK || node_name(*xml.Get()) != L"tag")
                throw std::runtime_error("Tag XML SWF non fermé.");
            data = unbase64(encoded);
        }
        add_tag(movie, code, data, long_header);
    }
    xml_check(next);
    if (!closed)
        throw std::runtime_error("Document XML SWF incomplet.");
    for (unsigned i = 0; i < 4; ++i)
        movie[4 + i] = static_cast<std::uint8_t>(movie.size() >> (8 * i));
    return decode(movie, std::move(movie));
}
}
