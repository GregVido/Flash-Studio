#include "UiStyle.hpp"

#include <winrt/Microsoft.Web.WebView2.Core.h>
#include <winrt/Windows.Data.Json.h>
#include <map>

namespace studio::ui
{
namespace
{
bool light = false;
thread_local std::map<unsigned, SolidColorBrush> brushes;
}

bool light_theme() { return light; }
ElementTheme element_theme() { return light ? ElementTheme::Light : ElementTheme::Dark; }

Windows::UI::Color color(unsigned rgb)
{
    if (light)
    {
        switch (rgb)
        {
        case base: rgb = 0xF1F5F8; break;
        case panel: rgb = 0xFAFCFD; break;
        case surface: rgb = 0xEDF3F7; break;
        case stroke: rgb = 0xD1DCE5; break;
        case primary: rgb = 0x17232D; break;
        case secondary: rgb = 0x4E6072; break;
        case accent: rgb = 0x006784; break;
        case danger: rgb = 0xAB273D; break;
        case selected_surface: rgb = 0xDCEEF6; break;
        case on_accent: rgb = 0xFFFFFF; break;
        case 0x83DCCC: rgb = 0x196F65; break;
        case 0x9FB5FF: rgb = 0x345AAE; break;
        case 0x232C3B: rgb = 0xE1EAF1; break;
        case 0x3b65a2u: rgb = 0xBAD7FA; break;
        }
    }
    return {255, static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8),
        static_cast<std::uint8_t>(rgb)};
}

SolidColorBrush brush(unsigned rgb)
{
    if (const auto found = brushes.find(rgb); found != brushes.end())
        return found->second;
    return brushes.emplace(rgb, SolidColorBrush(color(rgb))).first->second;
}

void set_light_theme(bool value)
{
    light = value;
    for (const auto& [rgb, paint] : brushes)
        paint.Color(color(rgb));
}

void initialize_theme()
{
    try
    {
        const auto file = settings_path(L"theme.txt");
        std::error_code error;
        if (std::filesystem::file_size(file, error) == 5)
        {
            const auto bytes = flash::read_file(file);
            set_light_theme(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == "light");
        }
    }
    catch (const std::exception&) {}
}
}

namespace studio
{
using namespace ui;

void Workspace::apply_theme()
{
    root_.RequestedTheme(element_theme());
    const auto caption = window_.AppWindow().TitleBar();
    caption.ButtonForegroundColor(color(primary));
    caption.ButtonInactiveForegroundColor(color(secondary));
    caption.ButtonHoverForegroundColor(color(primary));
    caption.ButtonHoverBackgroundColor(color(surface));
    if (source_web_)
        source_web_.DefaultBackgroundColor(color(base));
    if (source_ready_)
    {
        Windows::Data::Json::JsonObject message;
        message.Insert(L"action", Windows::Data::Json::JsonValue::CreateStringValue(L"theme"));
        message.Insert(L"value", Windows::Data::Json::JsonValue::CreateStringValue(light_theme() ? L"light" : L"dark"));
        try { source_web_.CoreWebView2().PostWebMessageAsJson(message.Stringify()); }
        catch (const hresult_error&) {}
    }
    sync_shape_theme();
}

fire_and_forget Workspace::show_settings()
{
    const auto lifetime = shared_from_this();
    ContentDialog dialog;
    dialog.XamlRoot(root_.XamlRoot());
    dialog.RequestedTheme(element_theme());
    dialog.Title(box_value(tr(L"Paramètres")));
    dialog.CloseButtonText(tr(L"Fermer"));
    dialog.SecondaryButtonText(tr(L"À propos"));

    StackPanel preferences;
    preferences.Width(315);
    preferences.Spacing(18);
    ComboBox theme;
    theme.Header(box_value(tr(L"Apparence")));
    theme.HorizontalAlignment(HorizontalAlignment::Stretch);
    theme.Items().Append(box_value(tr(L"Sombre")));
    theme.Items().Append(box_value(tr(L"Clair")));
    theme.SelectedIndex(light_theme() ? 1 : 0);
    preferences.Children().Append(theme);

    StackPanel language_panel;
    language_panel.Spacing(8);
    ComboBox locale;
    locale.Header(box_value(tr(L"Langue de l’application")));
    locale.HorizontalAlignment(HorizontalAlignment::Stretch);
    locale.Items().Append(box_value(L"Français"));
    locale.Items().Append(box_value(L"English"));
    locale.SelectedIndex(language() == Language::french ? 0 : 1);
    language_panel.Children().Append(locale);
    auto note = label(L"", 12, secondary);
    note.TextWrapping(TextWrapping::Wrap);
    note.Visibility(Visibility::Collapsed);
    language_panel.Children().Append(note);
    preferences.Children().Append(language_panel);
    dialog.Content(preferences);

    const auto theme_changed = theme.SelectionChanged(auto_revoke, [this, dialog, theme, note](auto&&, auto&&)
    {
        try
        {
            const auto file = settings_path(L"theme.txt");
            std::filesystem::create_directories(file.parent_path());
            const auto next = theme.SelectedIndex() == 1;
            flash::write_text(file, next ? "light" : "dark");
            set_light_theme(next);
            apply_theme();
            dialog.RequestedTheme(element_theme());
        }
        catch (const std::exception&)
        {
            note.Text(tr(L"Impossible d’enregistrer l’apparence."));
            note.Visibility(Visibility::Visible);
        }
    });
    const auto language_changed = locale.SelectionChanged(auto_revoke, [locale, note](auto&&, auto&&)
    {
        try
        {
            const auto next = locale.SelectedIndex() == 0 ? Language::french : Language::english;
            save_language(next);
            note.Text(tr(L"Langue enregistrée. Redémarrez Flash Studio pour l’appliquer."));
            note.Visibility(next == language() ? Visibility::Collapsed : Visibility::Visible);
        }
        catch (const std::exception&)
        {
            note.Text(tr(L"Impossible d’enregistrer la langue de l’application."));
            note.Visibility(Visibility::Visible);
        }
    });
    try
    {
        if (co_await dialog.ShowAsync() == ContentDialogResult::Secondary && !closed_)
            show_about();
    }
    catch (const hresult_error& error) { if (!closed_) status(error.message(), true); }
}
}
