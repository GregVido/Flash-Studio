#include "Workspace.hpp"
#include "UiLanguage.hpp"

#include <Windows.h>
#include <shellapi.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <charconv>

namespace
{
namespace markup = winrt::Microsoft::UI::Xaml::Markup;
using ControlsMetadata = winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider;

struct StudioApplication :
    winrt::Microsoft::UI::Xaml::ApplicationT<StudioApplication, markup::IXamlMetadataProvider>
{
    StudioApplication(std::filesystem::path file, std::optional<std::size_t> tag) : file_(std::move(file)), tag_(tag) {}

    markup::IXamlType GetXamlType(const winrt::Windows::UI::Xaml::Interop::TypeName& type)
    {
        return metadata().GetXamlType(type);
    }

    markup::IXamlType GetXamlType(const winrt::hstring& name)
    {
        return metadata().GetXamlType(name);
    }

    winrt::com_array<markup::XmlnsDefinition> GetXmlnsDefinitions()
    {
        return metadata().GetXmlnsDefinitions();
    }

    void OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        try
        {
            Resources().MergedDictionaries().Append(winrt::Microsoft::UI::Xaml::Controls::XamlControlsResources());
            workspace_ = std::make_shared<studio::Workspace>();
            workspace_->start(file_, tag_);
        }
        catch (const winrt::hresult_error& error)
        {
            MessageBoxW(nullptr, error.message().c_str(), studio::ui::tr(L"Flash Studio — démarrage"), MB_OK | MB_ICONERROR);
            Exit();
        }
    }

private:
    const ControlsMetadata& metadata()
    {
        // Programmatically created WinUI controls still resolve types from XAML templates.
        if (!metadata_)
            metadata_ = ControlsMetadata();
        return metadata_;
    }

    ControlsMetadata metadata_{nullptr};
    std::filesystem::path file_;
    std::optional<std::size_t> tag_;
    std::shared_ptr<studio::Workspace> workspace_;
};
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        studio::ui::initialize_language();
        int count{};
        const auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
        std::filesystem::path file;
        std::optional<std::size_t> tag;
        if (arguments)
        {
            const std::unique_ptr<wchar_t*, decltype(&LocalFree)> argument_storage(arguments, LocalFree);
            for (int i = 1; i < count; ++i)
            {
                const std::wstring_view option(arguments[i]);
                if (option == L"--language" && i + 1 < count)
                {
                    const std::wstring_view value(arguments[++i]);
                    if (value == L"fr" || value == L"en")
                        studio::ui::use_language(value == L"fr" ? studio::ui::Language::french : studio::ui::Language::english);
                }
                else if (option == L"--tag" && i + 1 < count)
                {
                    const auto value = winrt::to_string(winrt::hstring(arguments[++i]));
                    std::size_t index{};
                    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), index);
                    if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
                        tag = index;
                }
                else if (file.empty())
                    file = option;
            }
        }
        winrt::Microsoft::UI::Xaml::Application::Start([&](auto&&)
        {
            winrt::make<StudioApplication>(std::move(file), tag);
        });
        return 0;
    }
    catch (const winrt::hresult_error& error)
    {
        MessageBoxW(nullptr, error.message().c_str(), L"Flash Studio", MB_OK | MB_ICONERROR);
        return 1;
    }
    catch (const std::exception&)
    {
        MessageBoxW(nullptr, studio::ui::tr(L"Impossible de démarrer Flash Studio."), L"Flash Studio", MB_OK | MB_ICONERROR);
        return 1;
    }
}
