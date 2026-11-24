#include "Workspace.hpp"
#include "UiStyle.hpp"

namespace studio
{
using namespace ui;
using namespace Microsoft::UI::Xaml::Input;
using namespace Windows::System;

namespace
{
constexpr auto ctrl = VirtualKeyModifiers::Control;
constexpr auto shift = VirtualKeyModifiers::Shift;
struct Shortcut
{
    VirtualKey key;
    VirtualKeyModifiers modifiers;
    const wchar_t* action;
    const wchar_t* keys;
    const wchar_t* label;
};

constexpr Shortcut shortcuts[]{
    {VirtualKey::O, ctrl, L"open", L"Ctrl+O", L"Ouvrir un SWF"},
    {VirtualKey::O, ctrl | shift, L"folder", L"Ctrl+Maj+O", L"Ouvrir un dossier"},
    {VirtualKey::E, ctrl | shift, L"workspace", L"Ctrl+Maj+E", L"Afficher ou masquer l’espace de travail"},
    {VirtualKey::W, ctrl, L"close", L"Ctrl+W", L"Fermer le SWF actif"},
    {VirtualKey::Tab, ctrl, L"next", L"Ctrl+Tab", L"SWF suivant"},
    {VirtualKey::Tab, ctrl | shift, L"previous", L"Ctrl+Maj+Tab", L"SWF précédent"},
    {VirtualKey::Number1, ctrl, L"resources", L"Ctrl+1", L"Ressources"},
    {VirtualKey::Number2, ctrl, L"scripts", L"Ctrl+2", L"Scripts"},
    {VirtualKey::Number3, ctrl, L"tags", L"Ctrl+3", L"Tags"},
    {VirtualKey::F, ctrl, L"", L"Ctrl+F", L"Rechercher dans la vue active"},
    {VirtualKey::F, ctrl | shift, L"search", L"Ctrl+Maj+F", L"Rechercher dans le SWF"},
    {VirtualKey::S, ctrl | shift, L"save", L"Ctrl+Maj+S", L"Enregistrer sous"},
    {VirtualKey::F1, VirtualKeyModifiers::None, L"about", L"F1", L"À propos"}
};
}

void Workspace::register_shortcuts()
{
    for (const auto& shortcut : shortcuts)
    {
        const std::wstring_view action(shortcut.action);
        if (action.empty() || action == L"open" || action == L"search" || action == L"save")
            continue;
        KeyboardAccelerator binding;
        binding.Key(shortcut.key);
        binding.Modifiers(shortcut.modifiers);
        binding.Invoked([this, action](auto&&, const KeyboardAcceleratorInvokedEventArgs& event)
        {
            event.Handled(run_shortcut(action));
        });
        root_.KeyboardAccelerators().Append(binding);
    }
}

bool Workspace::run_shortcut(std::wstring_view action)
{
    if (closed_ || busy_)
        return false;
    if (action == L"open")
        open_dialog();
    else if (action == L"folder")
        open_folder_dialog();
    else if (action == L"workspace")
        toggle_folder_panel();
    else if (action == L"about")
        show_about();
    else if (!movie_ || !active_document_)
        return false;
    else if (action == L"close")
        close_document(*active_document_);
    else if (action == L"next" || action == L"previous")
    {
        const auto count = documents_.size();
        activate_document((*active_document_ + (action == L"next" ? 1 : count - 1)) % count);
    }
    else if (action == L"resources")
        switch_mode(Mode::resources);
    else if (action == L"scripts")
        switch_mode(Mode::scripts);
    else if (action == L"tags")
        switch_mode(Mode::tags);
    else if (action == L"search")
        show_global_search();
    else if (action == L"save")
        save_movie();
    else
        return false;
    return true;
}

fire_and_forget Workspace::show_about()
{
    if (busy_ || closed_)
        co_return;
    const auto lifetime = shared_from_this();
    busy_ = true;
    try
    {
        ContentDialog popup;
        popup.XamlRoot(root_.XamlRoot());
        popup.Title(box_value(tr(L"À propos de Flash Studio")));
        popup.CloseButtonText(tr(L"Fermer"));
        popup.DefaultButton(ContentDialogButton::Close);

        StackPanel content;
        content.Spacing(12);
        auto caption = label(tr(L"Inspection, édition et export de SWF"), 15, primary, true);
        caption.TextWrapping(TextWrapping::Wrap);
        content.Children().Append(caption);
        content.Children().Append(label(L"C++ · WinUI 3", 13, secondary));
        content.Children().Append(divider());
        content.Children().Append(label(tr(L"Raccourcis clavier"), 14, primary, true));

        Grid keyboard;
        keyboard.ColumnSpacing(16);
        keyboard.RowSpacing(8);
        column(keyboard, 1, GridUnitType::Star);
        column(keyboard, 1, GridUnitType::Auto);
        unsigned line = 0;
        for (const auto& shortcut : shortcuts)
        {
            row(keyboard);
            place(keyboard, label(tr(shortcut.label), 13), line);
            place(keyboard, label(tr(shortcut.keys), 12, secondary), line++, 1);
        }
        ScrollViewer scroll;
        scroll.MaxHeight(335);
        scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.Content(keyboard);
        content.Children().Append(scroll);
        popup.Content(content);
        co_await popup.ShowAsync();
    }
    catch (const hresult_error& error) { if (!closed_) status(error.message(), true); }
    busy_ = false;
}
}
