#include "UiLanguage.hpp"
#include "../FlashCore/Swf.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>

namespace studio::ui
{
namespace
{
Language selected = Language::english;

struct Translation { std::wstring_view french; const wchar_t* english; };
constexpr Translation catalog[]{
#include "UiTranslations.inc"
};
static_assert(std::ranges::is_sorted(catalog, {}, &Translation::french));
}

std::filesystem::path settings_path(std::wstring_view filename)
{
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        throw std::runtime_error("Could not locate the Flash Studio settings folder.");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(folder, CoTaskMemFree);
    return std::filesystem::path(folder) / L"Flash Studio" / filename;
}

void initialize_language()
{
    selected = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH ? Language::french : Language::english;
    try
    {
        const auto path = settings_path(L"language.txt");
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) != 2)
            return;
        const auto bytes = flash::read_file(path);
        const std::string_view value(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (value == "en" || value == "fr")
            selected = value == "fr" ? Language::french : Language::english;
    }
    catch (const std::exception&) {}
}

Language language() { return selected; }

void use_language(Language value) { selected = value; }

void save_language(Language value)
{
    const auto path = settings_path(L"language.txt");
    std::filesystem::create_directories(path.parent_path());
    flash::write_text(path, value == Language::french ? "fr" : "en");
}

const wchar_t* tr(const wchar_t* french)
{
    if (selected == Language::french)
        return french;
    const auto found = std::ranges::lower_bound(catalog, std::wstring_view(french), {}, &Translation::french);
    return found != std::end(catalog) && found->french == french ? found->english : french;
}

std::wstring localized_error(std::wstring_view message)
{
    if (selected == Language::english)
    {
        const auto found = std::ranges::lower_bound(catalog, message, {}, &Translation::french);
        if (found != std::end(catalog) && found->french == message)
            return found->english;
    }
    else
    {
        const auto found = std::ranges::find_if(catalog, [&](const Translation& entry) { return message == entry.english; });
        if (found != std::end(catalog))
            return std::wstring(found->french);
    }
    return std::wstring(message);
}
}
