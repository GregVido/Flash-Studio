#include "Workspace.hpp"
#include "../FlashCore/ShapeEditing.hpp"
#include "UiStyle.hpp"
#include "ResourceIds.h"

#include <Windows.h>
#include <shobjidl.h>
#include <microsoft.ui.xaml.window.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <format>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace studio
{
using namespace winrt;
using namespace ui;
using namespace Microsoft::UI;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Controls::Primitives;
using namespace Microsoft::UI::Xaml::Input;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Media::Imaging;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using Windows::Foundation::IInspectable;

namespace
{
constexpr std::size_t tags_per_page = 64;

const std::array<std::pair<flash::Category, wchar_t>, 10> filters{{
    {flash::Category::timeline, L'\uE81E'}, {flash::Category::scripts, L'\uE943'},
    {flash::Category::images, L'\uEB9F'}, {flash::Category::shapes, L'\uE81E'},
    {flash::Category::sounds, L'\uE8D6'}, {flash::Category::text, L'\uE8D2'},
    {flash::Category::fonts, L'\uE8D2'}, {flash::Category::sprites, L'\uE819'},
    {flash::Category::binary, L'\uE7C3'}, {flash::Category::other, L'\uE712'}
}};

HWND hwnd(const Window& window)
{
    HWND handle{};
    check_hresult(window.as<IWindowNative>()->get_WindowHandle(&handle));
    return handle;
}

fire_and_forget load_logo(BitmapImage bitmap)
{
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(FLASH_STUDIO_LOGO), RT_RCDATA);
    if (!resource)
        co_return;
    const auto handle = LoadResource(module, resource);
    const auto* data = static_cast<const std::uint8_t*>(handle ? LockResource(handle) : nullptr);
    if (!data)
        co_return;
    try
    {
        InMemoryRandomAccessStream stream;
        DataWriter writer(stream);
        writer.WriteBytes(array_view(data, data + SizeofResource(module, resource)));
        co_await writer.StoreAsync();
        writer.DetachStream();
        stream.Seek(0);
        co_await bitmap.SetSourceAsync(stream);
    }
    catch (const hresult_error&) {}
}

enum class DocumentFormat { uncompressed = 1, compressed, xml };
enum class ResourceImport { none, bitmap, audio };

std::optional<std::filesystem::path> dialog(const Window& window, bool save, std::wstring_view extension,
    std::wstring_view suggested = {}, DocumentFormat* format = nullptr, bool folder = false, bool raster = false, bool vector = false,
    ResourceImport resource = ResourceImport::none)
{
    auto picker = create_instance<IFileDialog>(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog);
    const std::wstring wildcard = L"*" + std::wstring(extension);
    COMDLG_FILTERSPEC types[]{
        {extension == L".swf" ? tr(L"Animation Flash") : tr(L"Fichiers correspondants"), wildcard.c_str()},
        {tr(L"Tous les fichiers"), L"*.*"}
    };
    const COMDLG_FILTERSPEC movie_types[]{
        {tr(L"SWF décompressé (FWS)"), L"*.swf"},
        {tr(L"SWF compressé (CWS)"), L"*.swf"},
        {tr(L"Document XML Flash Studio"), L"*.xml"}
    };
    const COMDLG_FILTERSPEC open_types[]{
        {tr(L"Fichiers SWF et XML"), L"*.swf;*.xml"},
        {tr(L"Tous les fichiers"), L"*.*"}
    };
    const COMDLG_FILTERSPEC image_types[]{
        {tr(L"Image SVG"), L"*.svg"}, {tr(L"Image PNG"), L"*.png"}, {tr(L"Image BMP"), L"*.bmp"},
        {tr(L"Image JPEG"), L"*.jpg"}, {tr(L"Image TIFF"), L"*.tiff"}
    };
    const COMDLG_FILTERSPEC imports[]{
        {tr(L"Images bitmap"), L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff"},
        {tr(L"Fichiers audio"), L"*.wav;*.mp3;*.m4a;*.aac;*.wma;*.flac"}
    };
    if (folder)
        check_hresult(picker->SetTitle(tr(save ? L"Choisir le dossier d’export" : L"Ouvrir un dossier de SWF")));
    else if (raster)
        check_hresult(picker->SetFileTypes(vector ? 5 : 4, image_types + (vector ? 0 : 1)));
    else if (format)
        check_hresult(picker->SetFileTypes(3, movie_types));
    else if (!save && extension == L".swf")
        check_hresult(picker->SetFileTypes(2, open_types));
    else if (resource != ResourceImport::none)
    {
        const COMDLG_FILTERSPEC allowed[]{imports[resource == ResourceImport::bitmap ? 0 : 1], types[1]};
        check_hresult(picker->SetFileTypes(2, allowed));
    }
    else
        check_hresult(picker->SetFileTypes(extension.empty() ? 1 : 2, extension.empty() ? types + 1 : types));
    DWORD options{};
    check_hresult(picker->GetOptions(&options));
    check_hresult(picker->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
        (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST) | (format || raster ? FOS_STRICTFILETYPES : 0) |
        (folder ? FOS_PICKFOLDERS : 0)));
    if (save && !extension.empty())
    {
        if (!format)
            check_hresult(picker->SetDefaultExtension(wildcard.c_str() + 2));
        const std::wstring name(suggested);
        check_hresult(picker->SetFileName(name.c_str()));
        if (format)
            check_hresult(picker->SetFileTypeIndex(static_cast<unsigned>(*format)));
    }
    const auto result = picker->Show(hwnd(window));
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    check_hresult(result);
    com_ptr<IShellItem> item;
    check_hresult(picker->GetResult(item.put()));
    PWSTR path{};
    check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(path, CoTaskMemFree);
    std::filesystem::path destination(path);
    if (format)
    {
        UINT selected{};
        check_hresult(picker->GetFileTypeIndex(&selected));
        *format = static_cast<DocumentFormat>(selected);
        if (lower(destination.extension().wstring()) == L".xml")
            *format = DocumentFormat::xml;
    }
    return destination;
}

bool is_abc(unsigned code) { return code == 72 || code == 82; }
bool is_text(unsigned code) { return code == 11 || code == 33 || code == 37 || code == 77; }

void write_document_file(const std::filesystem::path& path, const flash::Movie& movie, DocumentFormat format)
{
    GUID id;
    check_hresult(CoCreateGuid(&id));
    wchar_t name[40];
    if (!StringFromGUID2(id, name, static_cast<int>(std::size(name))))
        throw std::runtime_error("Impossible de créer un fichier temporaire.");
    const auto temporary = path.parent_path() / (std::wstring(name) + L".tmp");
    try
    {
        switch (format)
        {
        case DocumentFormat::uncompressed: flash::write_file(temporary, movie.uncompressed()); break;
        case DocumentFormat::compressed: flash::write_file(temporary, movie.compressed()); break;
        case DocumentFormat::xml: flash::write_text(temporary, movie.export_xml()); break;
        }
        check_bool(MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    }
    catch (...)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}
}

void Workspace::start(const std::filesystem::path& initial_file, std::optional<std::size_t> initial_tag)
{
    ui_thread_ = GetCurrentThreadId();
    window_ = Window();
    window_.Title(L"Flash Studio");
    window_.AppWindow().Closing([weak = weak_from_this()](auto&&, const Microsoft::UI::Windowing::AppWindowClosingEventArgs& event)
    {
        if (const auto owner = weak.lock(); owner && !owner->close_confirmed_)
        {
            event.Cancel(true);
            owner->root_.DispatcherQueue().TryEnqueue([weak]
            {
                if (const auto workspace = weak.lock())
                    workspace->request_quit();
            });
        }
    });
    window_.Closed([this](auto&&, auto&&)
    {
        closed_ = true;
        if (source_web_)
            source_web_.Close();
        close_shape_editor();
        close_frame_player();
        if (thumbnails_cancel_)
            thumbnails_cancel_->store(true, std::memory_order_relaxed);
    });
    build();
    for (const auto& [kind, width, height] : {std::array{ICON_BIG, SM_CXICON, SM_CYICON},
        std::array{ICON_SMALL, SM_CXSMICON, SM_CYSMICON}})
    {
        if (const auto icon = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(FLASH_STUDIO_ICON), IMAGE_ICON,
            GetSystemMetrics(width), GetSystemMetrics(height), LR_SHARED))
            SendMessageW(hwnd(window_), WM_SETICON, kind, reinterpret_cast<LPARAM>(icon));
    }
    const auto app_window = window_.AppWindow();
    app_window.Resize({1320, 830});
    window_.Activate();
    if (!initial_file.empty())
    {
        std::error_code error;
        if (std::filesystem::is_directory(initial_file, error))
            open_folder(initial_file);
        else
            open_file(initial_file, initial_tag);
    }
    else
        restore_session();
}

void Workspace::build()
{
    initialize_theme();
    root_ = Grid();
    root_.RequestedTheme(element_theme());
    window_.SystemBackdrop(MicaBackdrop());
    root_.AllowDrop(true);
    root_.DragOver([](auto&&, const DragEventArgs& args)
    {
        if (args.DataView().Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
            args.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
    });
    root_.Drop([this](auto&&, const DragEventArgs& args) { drop(args); });
    row(root_, 40, GridUnitType::Pixel);
    row(root_);
    row(root_, 1, GridUnitType::Star);
    row(root_);

    Grid title_bar;
    column(title_bar, 42);
    column(title_bar, 1, GridUnitType::Star);
    const auto caption_padding = column(title_bar, 138);
    BitmapImage logo;
    load_logo(logo);
    Image header_logo;
    header_logo.Width(22);
    header_logo.Height(22);
    header_logo.Source(logo);
    ToolTipService::SetToolTip(header_logo, box_value(L"Flash Studio"));
    place(title_bar, header_logo, 0);
    build_documents();
    documents_bar_.VerticalAlignment(VerticalAlignment::Bottom);
    place(title_bar, documents_bar_, 0, 1);
    Grid drag_area;
    drag_area.Background(SolidColorBrush(Windows::UI::Color{0, 0, 0, 0}));
    drag_area.MinWidth(64);
    documents_bar_.TabStripFooter(drag_area);
    drag_area.Loaded([this](const IInspectable& sender, auto&&) { window_.SetTitleBar(sender.as<UIElement>()); });
    const auto size_caption = [this, caption_padding](const IInspectable& element, auto&&)
    {
        const auto root = element.as<FrameworkElement>().XamlRoot();
        if (root)
            caption_padding.Width({window_.AppWindow().TitleBar().RightInset() / root.RasterizationScale(), GridUnitType::Pixel});
    };
    title_bar.Loaded(size_caption);
    title_bar.SizeChanged(size_caption);
    place(root_, title_bar, 0);

    Grid header;
    header.Background(brush(panel));
    header.Padding(pad(12, 8, 12, 8));
    header.ColumnSpacing(16);
    column(header, 1, GridUnitType::Auto);
    column(header, 1, GridUnitType::Star);
    column(header, 1, GridUnitType::Auto);
    auto document = StackPanel();
    document.Spacing(2);
    document.VerticalAlignment(VerticalAlignment::Center);
    file_name_ = label(tr(L"Explorateur SWF"), 14, primary, true);
    file_info_ = label(tr(L"Ouvrez une animation Flash"), 12, secondary);
    document.Children().Append(file_name_);
    document.Children().Append(file_info_);
    place(header, document, 0, 1);

    auto actions = horizontal(8);
    actions.VerticalAlignment(VerticalAlignment::Center);
    open_ = Button();
    open_.Content(icon_label(L'\uE838', tr(L"Ouvrir"), on_accent));
    open_.Background(brush(accent));
    open_.Padding(pad(14, 8, 14, 8));
    open_.Click([this](auto&&, auto&&) { open_dialog(); });
    save_ = Button();
    save_.Content(glyph(L'\uE74E', 16));
    save_.Padding(pad(11, 8, 11, 8));
    Automation::AutomationProperties::SetName(save_, tr(L"Enregistrer"));
    ToolTipService::SetToolTip(save_, box_value(tr(L"Enregistrer")));
    save_.Click([this](auto&&, auto&&) { save_movie(); });
    save_.IsEnabled(false);
    actions.Children().Append(open_);
    save_as_ = MenuFlyoutItem();
    save_as_.Text(tr(L"Enregistrer sous"));
    save_as_.Click([this](auto&&, auto&&) { save_movie(true); });
    save_as_.IsEnabled(false);
    folder_button_ = Button();
    folder_button_.Content(glyph(L'\uE8B7', 16));
    folder_button_.Padding(pad(10, 8, 10, 8));
    Automation::AutomationProperties::SetName(folder_button_, tr(L"Espace de travail"));
    ToolTipService::SetToolTip(folder_button_, box_value(tr(L"Espace de travail (Ctrl+Maj+E)")));
    folder_button_.Click([this](auto&&, auto&&) { toggle_folder_panel(); });
    DropDownButton file_commands;
    file_commands.Content(box_value(tr(L"Fichier")));
    MenuFlyout file_menu;
    MenuFlyoutItem open_folder;
    open_folder.Text(tr(L"Ouvrir un dossier"));
    open_folder.Click([this](auto&&, auto&&) { open_folder_dialog(); });
    file_menu.Items().Append(open_folder);
    file_menu.Items().Append(recent_);
    file_menu.Items().Append(MenuFlyoutSeparator());
    file_menu.Items().Append(save_as_);
    file_menu.Opening([this](auto&&, auto&&) { show_recent(); });
    file_commands.Flyout(file_menu);
    actions.Children().Append(file_commands);
    actions.Children().Append(save_);
    place(header, actions, 0);
    auto utility_actions = horizontal(6);
    utility_actions.VerticalAlignment(VerticalAlignment::Center);
    utility_actions.Children().Append(folder_button_);
    global_search_ = Button();
    global_search_.Content(glyph(L'\uE721', 16));
    global_search_.Padding(pad(11, 8, 11, 8));
    global_search_.IsEnabled(false);
    Automation::AutomationProperties::SetName(global_search_, tr(L"Rechercher dans le SWF"));
    ToolTipService::SetToolTip(global_search_, box_value(tr(L"Rechercher dans le SWF (Ctrl+Maj+F)")));
    global_search_.Click([this](auto&&, auto&&) { show_global_search(); });
    utility_actions.Children().Append(global_search_);
    Button preferences;
    preferences.Content(glyph(L'\uE713', 16));
    preferences.Padding(pad(10, 8, 10, 8));
    Automation::AutomationProperties::SetName(preferences, tr(L"Paramètres"));
    ToolTipService::SetToolTip(preferences, box_value(tr(L"Paramètres")));
    preferences.Click([this](auto&&, auto&&) { show_settings(); });
    utility_actions.Children().Append(preferences);
    place(header, utility_actions, 0, 2);
    place(root_, header, 1);

    KeyboardAccelerator shortcut;
    shortcut.Key(Windows::System::VirtualKey::O);
    shortcut.Modifiers(Windows::System::VirtualKeyModifiers::Control);
    open_.KeyboardAccelerators().Append(shortcut);
    KeyboardAccelerator save_shortcut;
    save_shortcut.Key(Windows::System::VirtualKey::S);
    save_shortcut.Modifiers(Windows::System::VirtualKeyModifiers::Control);
    save_.KeyboardAccelerators().Append(save_shortcut);
    KeyboardAccelerator save_as_shortcut;
    save_as_shortcut.Key(Windows::System::VirtualKey::S);
    save_as_shortcut.Modifiers(Windows::System::VirtualKeyModifiers::Control | Windows::System::VirtualKeyModifiers::Shift);
    save_as_shortcut.Invoked([this](auto&&, const KeyboardAcceleratorInvokedEventArgs& args)
    {
        if (save_as_.IsEnabled())
        {
            save_movie(true);
            args.Handled(true);
        }
    });
    root_.KeyboardAccelerators().Append(save_as_shortcut);
    KeyboardAccelerator find_shortcut;
    find_shortcut.Key(Windows::System::VirtualKey::F);
    find_shortcut.Modifiers(Windows::System::VirtualKeyModifiers::Control);
    find_shortcut.Invoked([this](auto&&, const KeyboardAcceleratorInvokedEventArgs& args)
    {
        if (!movie_ || source_session_)
            return;
        if (mode_ == Mode::resources && selected_)
        {
            const UpdatingSelection updating(refreshing_);
            tiles_.SelectedIndex(-1);
            select_tag({});
        }
        const auto input = mode_ == Mode::scripts ? script_search_ : mode_ == Mode::resources ? gallery_search_ : search_;
        input.Focus(FocusState::Keyboard);
        input.SelectAll();
        args.Handled(true);
    });
    root_.KeyboardAccelerators().Append(find_shortcut);
    KeyboardAccelerator global_shortcut;
    global_shortcut.Key(Windows::System::VirtualKey::F);
    global_shortcut.Modifiers(Windows::System::VirtualKeyModifiers::Control | Windows::System::VirtualKeyModifiers::Shift);
    global_search_.KeyboardAccelerators().Append(global_shortcut);
    register_shortcuts();

    Grid main;
    workarea_ = Border();
    workarea_.BorderThickness(pad(0, 1, 0, 1));
    workarea_.BorderBrush(brush(stroke));
    Grid workspace;
    column(workspace, 58);
    column(workspace, 1, GridUnitType::Star);
    build_modes(workspace);
    Grid content;
    const auto side_column = column(content, 232);
    column(content, 0.65, GridUnitType::Star);
    column(content, 1, GridUnitType::Star);
    root_.SizeChanged([this, side_column](auto&&, const SizeChangedEventArgs& args)
    {
        side_column.Width({args.NewSize().Width < 975 ? 184.0 : 232.0, GridUnitType::Pixel});
        if (folder_pane_)
            folder_pane_.DisplayMode(args.NewSize().Width >= 1450 ? SplitViewDisplayMode::Inline : SplitViewDisplayMode::Overlay);
        update_layout();
    });
    place(workspace, content, 0, 1);
    workarea_.Child(workspace);
    main.Children().Append(workarea_);

    Grid sidebar;
    sidebar.Background(brush(panel));
    sidebar.Padding(pad(9, 16, 9, 10));
    row(sidebar);
    row(sidebar, 1, GridUnitType::Star);
    sidebar_title_ = label(tr(L"Bibliothèque"), 15, primary, true);
    sidebar_title_.Margin(pad(9, 0, 0, 13));
    place(sidebar, sidebar_title_, 0);
    categories_ = ListView();
    categories_.SelectionMode(ListViewSelectionMode::Single);
    Automation::AutomationProperties::SetName(categories_, tr(L"Catégories de tags"));
    categories_.SelectionChanged([this](auto&&, auto&&)
    {
        if (refreshing_)
            return;
        if (mode_ == Mode::resources)
            refresh_gallery();
        else if (mode_ == Mode::tags)
            refresh_tags();
    });
    place(sidebar, categories_, 1);
    build_script_browser(sidebar);
    place(content, sidebar, 0);

    listing_ = Border();
    listing_.BorderBrush(brush(stroke));
    listing_.BorderThickness(pad(1, 0, 1, 0));
    listing_.Background(brush(base));
    Grid results;
    row(results);
    row(results);
    row(results, 1, GridUnitType::Star);
    row(results);
    auto controls = StackPanel();
    controls.Spacing(10);
    controls.Margin(pad(17, 18, 17, 12));
    list_count_ = label(L"Tags", 15, primary, true);
    controls.Children().Append(list_count_);
    search_ = TextBox();
    search_.PlaceholderText(tr(L"Rechercher un tag…"));
    search_.MaxLength(256);
    Automation::AutomationProperties::SetName(search_, tr(L"Rechercher un tag"));
    search_.TextChanged([this](auto&&, auto&&) { refresh_tags(); });
    controls.Children().Append(search_);
    place(results, controls, 0);
    place(results, divider(), 1);
    tags_ = ListView();
    tags_.SelectionMode(ListViewSelectionMode::Single);
    tags_.Padding(pad(7, 5, 7, 5));
    Automation::AutomationProperties::SetName(tags_, tr(L"Tags du fichier"));
    tags_.SelectionChanged([this](auto&&, auto&&)
    {
        if (refreshing_)
            return;
        const auto index = tags_.SelectedIndex();
        const auto slot = page_start_ + static_cast<std::size_t>(std::max(index, 0));
        select_tag(index >= 0 && slot < page_end_ ? std::optional{matches_[slot]} : std::nullopt);
    });
    place(results, tags_, 2);
    empty_list_ = label(tr(L"Aucun tag correspondant"), 13, secondary);
    empty_list_.Margin(pad(18, 20, 12, 0));
    empty_list_.VerticalAlignment(VerticalAlignment::Top);
    empty_list_.Visibility(Visibility::Collapsed);
    place(results, empty_list_, 2);
    auto pagination = horizontal(8);
    pagination.Margin(pad(13, 6, 13, 10));
    previous_page_ = Button();
    previous_page_.Content(box_value(tr(L"Précédent")));
    previous_page_.Click([this](auto&&, auto&&) { if (page_) show_page(page_ - 1); });
    next_page_ = Button();
    next_page_.Content(box_value(tr(L"Suivant")));
    next_page_.Click([this](auto&&, auto&&) { if (page_end_ < matches_.size()) show_page(page_ + 1); });
    page_label_ = label(L"", 12, secondary);
    page_label_.VerticalAlignment(VerticalAlignment::Center);
    pagination.Children().Append(previous_page_);
    pagination.Children().Append(next_page_);
    pagination.Children().Append(page_label_);
    place(results, pagination, 3);
    listing_.Child(results);
    place(content, listing_, 0, 1);
    build_gallery();
    place(content, gallery_, 0, 1);

    inspector_ = Grid();
    row(inspector_);
    inspector_.Background(brush(panel));
    row(inspector_);
    row(inspector_);
    row(inspector_);
    row(inspector_, 1, GridUnitType::Star);
    Grid detail;
    detail.Padding(pad(14, 12, 14, 10));
    detail.ColumnSpacing(10);
    column(detail, 1, GridUnitType::Auto);
    column(detail, 1, GridUnitType::Star);
    place(detail, gallery_back_, 0);
    auto info = StackPanel();
    info.Spacing(5);
    detail_title_ = label(tr(L"Aperçu du fichier"), 20, primary, true);
    detail_meta_ = label(tr(L"Sélectionnez un tag pour l’inspecter"), 12, secondary);
    info.Children().Append(detail_title_);
    info.Children().Append(detail_meta_);
    place(detail, info, 0, 1);
    export_ = DropDownButton();
    export_.Content(box_value(tr(L"Exporter")));
    export_.VerticalAlignment(VerticalAlignment::Center);
    MenuFlyout options;
    const auto add_export = [&](std::wstring_view title, Export format)
    {
        MenuFlyoutItem item;
        item.Text(title);
        item.Click([this, format](auto&&, auto&&) { export_tag(format); });
        options.Items().Append(item);
        return item;
    };
    export_asset_ = add_export(tr(L"Ressource sélectionnée"), Export::asset);
    export_raw_ = add_export(tr(L"Contenu brut du tag"), Export::raw);
    options.Items().Append(MenuFlyoutSeparator());
    export_source_ = add_export(tr(L"Source ActionScript"), Export::source);
    export_assembly_ = add_export(tr(L"Instructions désassemblées"), Export::instructions);
    export_pcode_ = add_export(tr(L"P-code de la méthode"), Export::pcode);
    export_graph_ = add_export(tr(L"Graphe de contrôle de la méthode (.dot)"), Export::graph);
    convert_image_ = MenuFlyoutItem();
    convert_image_.Text(tr(L"Exporter le visuel…"));
    convert_image_.Click([this](auto&&, auto&&) { export_image(); });
    options.Items().Append(convert_image_);
    animation_export_ = MenuFlyoutSubItem();
    animation_export_.Text(tr(L"Exporter le sprite animé"));
    for (const auto& [title, format] : {std::pair{L"GIF…", flash::AnimationFormat::gif},
        std::pair{tr(L"PNG animé (APNG)…"), flash::AnimationFormat::apng}, std::pair{tr(L"SVG animé…"), flash::AnimationFormat::svg}})
    {
        MenuFlyoutItem item;
        item.Text(title);
        item.Click([this, format](auto&&, auto&&) { export_animation(format); });
        animation_export_.Items().Append(item);
    }
    options.Items().Append(animation_export_);
    options.Items().Append(MenuFlyoutSeparator());
    MenuFlyoutItem all_resources;
    all_resources.Text(tr(L"Toutes les ressources…"));
    all_resources.Click([this](auto&&, auto&&) { export_all(); });
    options.Items().Append(all_resources);
    MenuFlyoutItem batch;
    batch.Text(tr(L"Exporter les visuels par lot…"));
    batch.Click([this](auto&&, auto&&) { choose_visual_batch(); });
    options.Items().Append(batch);
    MenuFlyoutItem project;
    project.Text(tr(L"Exporter le projet…"));
    project.Click([this](auto&&, auto&&) { export_all(true); });
    options.Items().Append(project);
    MenuFlyoutItem html5;
    html5.Text(tr(L"Exporter en HTML5…"));
    html5.Click([this](auto&&, auto&&) { export_all(false, {}, true); });
    options.Items().Append(html5);
    export_.Flyout(options);
    export_.IsEnabled(false);
    actions.Children().Append(export_);
    build_script_tabs();
    place(inspector_, detail, 1);

    Grid bar;
    column(bar, 1, GridUnitType::Star);
    column(bar, 1, GridUnitType::Auto);
    bar.Background(brush(base));
    bar.Padding(pad(13, 8, 13, 8));
    auto views = horizontal(3);
    const std::wstring_view captions[]{tr(L"Détails"), L"ActionScript", L"Instructions", L"Hex", tr(L"Scène")};
    for (std::size_t i = 0; i < tabs_.size(); ++i)
    {
        auto& tab = tabs_[i];
        tab.Content(box_value(hstring(captions[i])));
        tab.FontSize(12);
        tab.Padding(pad(9, 6, 9, 6));
        tab.IsChecked(i == 0);
        tab.Click([this, i](auto&&, auto&&) { set_view(static_cast<View>(i)); });
        views.Children().Append(tab);
    }
    place(bar, views, 0);
    auto view_actions = horizontal(5);
    replace_ = Button();
    replace_.Content(box_value(tr(L"Remplacer…")));
    replace_.FontSize(12);
    replace_.Visibility(Visibility::Collapsed);
    Automation::AutomationProperties::SetName(replace_, tr(L"Remplacer les données binaires"));
    replace_.Click([this](auto&&, auto&&) { edit_tag(); });
    view_actions.Children().Append(replace_);
    shape_edit_ = Button();
    shape_edit_.Content(box_value(tr(L"Éditer la forme")));
    shape_edit_.FontSize(12);
    shape_edit_.Visibility(Visibility::Collapsed);
    shape_edit_.Click([this](auto&&, auto&&) { edit_shape(); });
    view_actions.Children().Append(shape_edit_);
    source_edit_ = Button();
    source_edit_.Content(box_value(tr(L"Éditer le code")));
    source_edit_.FontSize(12);
    source_edit_.Visibility(Visibility::Collapsed);
    source_edit_.Click([this](auto&&, auto&&)
    {
        if (view_ == View::instructions)
            begin_method_source();
        else
            edit_class();
    });
    view_actions.Children().Append(source_edit_);
    script_pin_ = Button();
    script_pin_.FontSize(12);
    script_pin_.Visibility(Visibility::Collapsed);
    script_pin_.Click([this](auto&&, auto&&)
    {
        if (const auto* tab = current_script_tab())
            toggle_script_pin(tab->script.tag, tab->script.qualified);
    });
    view_actions.Children().Append(script_pin_);
    toggle_outline_ = ToggleButton();
    toggle_outline_.Content(box_value(tr(L"Membres")));
    toggle_outline_.FontSize(12);
    toggle_outline_.IsChecked(true);
    toggle_outline_.Click([this](auto&&, auto&&) { update_layout(); });
    view_actions.Children().Append(toggle_outline_);
    place(bar, view_actions, 0, 1);
    place(inspector_, bar, 2);

    abc_tools_ = Grid();
    abc_tools_.Visibility(Visibility::Collapsed);
    abc_tools_.Padding(pad(12, 8, 12, 2));
    abc_tools_.ColumnSpacing(8);
    column(abc_tools_, 1, GridUnitType::Star);
    column(abc_tools_, 1.4, GridUnitType::Star);
    column(abc_tools_, 1, GridUnitType::Auto);
    column(abc_tools_, 1, GridUnitType::Auto);
    abc_owner_ = ComboBox();
    abc_owner_.PlaceholderText(tr(L"Classe ou script"));
    abc_owner_.HorizontalAlignment(HorizontalAlignment::Stretch);
    abc_owner_.IsTextSearchEnabled(true);
    Automation::AutomationProperties::SetName(abc_owner_, tr(L"Classe ou script ABC"));
    abc_owner_.SelectionChanged([this](auto&&, auto&&)
    {
        const auto group = abc_owner_.SelectedIndex();
        if (refreshing_ || !abc_ || group < 0)
            return;
        show_abc_methods(static_cast<std::size_t>(group), {});
        show_selection();
    });
    place(abc_tools_, abc_owner_, 0, 0);
    abc_method_list_ = ComboBox();
    abc_method_list_.PlaceholderText(tr(L"Méthode"));
    abc_method_list_.HorizontalAlignment(HorizontalAlignment::Stretch);
    abc_method_list_.IsTextSearchEnabled(true);
    Automation::AutomationProperties::SetName(abc_method_list_, tr(L"Méthode ABC"));
    abc_method_list_.SelectionChanged([this](auto&&, auto&&)
    {
        if (refreshing_ || !abc_)
            return;
        const auto group = abc_owner_.SelectedIndex(), item = abc_method_list_.SelectedIndex();
        if (group < 0 || item < 0)
            return;
        abc_method_ = abc_groups_.at(static_cast<std::size_t>(group)).methods.at(static_cast<std::size_t>(item));
        update_abc_visibility();
        show_selection();
    });
    place(abc_tools_, abc_method_list_, 0, 1);
    deobfuscate_ = Button();
    deobfuscate_.Content(box_value(tr(L"Désobfusquer…")));
    deobfuscate_.Click([this](auto&&, auto&&) { deobfuscate_swf(false); });
    place(abc_tools_, deobfuscate_, 0, 2);
    DropDownButton abc_actions;
    abc_actions.Content(box_value(tr(L"Modifier ABC")));
    MenuFlyout abc_menu;
    edit_class_ = MenuFlyoutItem();
    edit_class_.Text(tr(L"Éditer la classe ActionScript…"));
    edit_class_.Click([this](auto&&, auto&&) { edit_class(); });
    abc_menu.Items().Append(edit_class_);
    edit_pcode_ = MenuFlyoutItem();
    edit_pcode_.Text(tr(L"Éditer le P-code…"));
    edit_pcode_.Click([this](auto&&, auto&&) { begin_method_source(); });
    abc_menu.Items().Append(edit_pcode_);
    edit_simplified_ = MenuFlyoutItem();
    edit_simplified_.Text(tr(L"Nettoyer le P-code de la méthode…"));
    edit_simplified_.Click([this](auto&&, auto&&) { begin_method_source(true); });
    abc_menu.Items().Append(edit_simplified_);
    MenuFlyoutItem clean_movie;
    clean_movie.Text(tr(L"Désobfusquer le SWF…"));
    clean_movie.Click([this](auto&&, auto&&) { deobfuscate_swf(true); });
    abc_menu.Items().Append(clean_movie);
    MenuFlyoutItem constant;
    constant.Text(tr(L"Modifier une constante chaîne…"));
    constant.Click([this](auto&&, auto&&) { edit_abc(AbcEdit::pool_string); });
    abc_menu.Items().Append(constant);
    MenuFlyoutItem find_constant;
    find_constant.Text(tr(L"Rechercher dans les constantes ABC…"));
    find_constant.Click([this](auto&&, auto&&) { show_global_search(flash::SearchArea::constants); });
    abc_menu.Items().Append(find_constant);
    MenuFlyoutItem replace_module;
    replace_module.Text(tr(L"Remplacer le bloc ABC…"));
    replace_module.Click([this](auto&&, auto&&) { edit_abc(AbcEdit::import); });
    abc_menu.Items().Append(replace_module);
    abc_actions.Flyout(abc_menu);
    place(abc_tools_, abc_actions, 0, 3);
    place(inspector_, abc_tools_, 3);

    Grid preview;
    output_ = TextBox();
    output_.Margin(pad(12));
    output_.FontFamily(FontFamily(L"Consolas"));
    output_.FontSize(13);
    output_.IsReadOnly(true);
    output_.AcceptsReturn(true);
    output_.TextWrapping(TextWrapping::NoWrap);
    output_.Background(brush(base));
    output_.SelectionHighlightColor(brush(0x3b65a2u));
    output_.BorderBrush(brush(stroke));
    output_.Padding(pad(14));
    ScrollViewer::SetHorizontalScrollBarVisibility(output_, ScrollBarVisibility::Auto);
    ScrollViewer::SetVerticalScrollBarVisibility(output_, ScrollBarVisibility::Auto);
    Automation::AutomationProperties::SetName(output_, tr(L"Contenu du tag sélectionné"));
    progress_ = ProgressRing();
    progress_.Width(34);
    progress_.Height(34);
    progress_.HorizontalAlignment(HorizontalAlignment::Center);
    progress_.VerticalAlignment(VerticalAlignment::Center);
    progress_.Visibility(Visibility::Collapsed);
    image_ = Image();
    image_.Stretch(Stretch::Uniform);
    image_.Margin(pad(18));
    picture_view_ = ScrollViewer();
    picture_view_.Margin(pad(12));
    picture_view_.Background(brush(base));
    picture_view_.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);
    picture_view_.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    picture_view_.HorizontalScrollMode(ScrollMode::Auto);
    picture_view_.VerticalScrollMode(ScrollMode::Auto);
    picture_view_.ZoomMode(ZoomMode::Enabled);
    picture_view_.MinZoomFactor(0.5f);
    picture_view_.MaxZoomFactor(6.0f);
    picture_view_.Content(image_);
    picture_view_.Visibility(Visibility::Collapsed);
    picture_view_.SizeChanged([this](auto&&, const SizeChangedEventArgs& size)
    {
        image_.MaxWidth(std::max(1.0, static_cast<double>(size.NewSize().Width) - 36));
        image_.MaxHeight(std::max(1.0, static_cast<double>(size.NewSize().Height) - 36));
    });
    Automation::AutomationProperties::SetName(picture_view_, tr(L"Aperçu de la ressource, zoomable"));
    audio_ = MediaPlayerElement();
    audio_.AreTransportControlsEnabled(true);
    audio_.AutoPlay(false);
    audio_.Height(170);
    audio_.VerticalAlignment(VerticalAlignment::Center);
    audio_.Margin(pad(20));
    audio_.Visibility(Visibility::Collapsed);
    Automation::AutomationProperties::SetName(audio_, tr(L"Écouter le son sélectionné"));
    preview_error_ = label(L"", 13, danger);
    preview_error_.Margin(pad(16));
    preview_error_.TextWrapping(TextWrapping::Wrap);
    preview_error_.VerticalAlignment(VerticalAlignment::Center);
    preview_error_.Visibility(Visibility::Collapsed);
    preview.Children().Append(output_);
    preview.Children().Append(picture_view_);
    preview.Children().Append(audio_);
    preview.Children().Append(preview_error_);
    resource_preview_ = Grid();
    resource_preview_.Visibility(Visibility::Collapsed);
    preview.Children().Append(resource_preview_);
    preview.Children().Append(progress_);
    build_source_editor(preview);
    build_outline(preview);
    place(inspector_, preview, 4);
    build_frame_player();
    place(content, inspector_, 0, 2);

    empty_ = Border();
    empty_.Background(brush(base));
    empty_.BorderBrush(brush(stroke));
    empty_.BorderThickness(pad(0, 1, 0, 1));
    StackPanel welcome;
    welcome.Spacing(14);
    welcome.HorizontalAlignment(HorizontalAlignment::Center);
    welcome.VerticalAlignment(VerticalAlignment::Center);
    Image emblem;
    emblem.Width(84);
    emblem.Height(84);
    emblem.HorizontalAlignment(HorizontalAlignment::Center);
    emblem.Source(logo);
    welcome.Children().Append(emblem);
    auto question = label(tr(L"Ouvrir une animation Flash"), 26, primary, true);
    question.HorizontalAlignment(HorizontalAlignment::Center);
    welcome.Children().Append(question);
    auto instructions = label(tr(L"Glissez un fichier SWF ou un dossier dans cette fenêtre"), 14, secondary);
    instructions.HorizontalAlignment(HorizontalAlignment::Center);
    welcome.Children().Append(instructions);
    Button choose;
    choose.Content(icon_label(L'\uE838', tr(L"Parcourir les fichiers")));
    choose.Click([this](auto&&, auto&&) { open_dialog(); });
    Button choose_folder;
    choose_folder.Content(icon_label(L'\uE8B7', tr(L"Ouvrir un dossier")));
    choose_folder.Click([this](auto&&, auto&&) { open_folder_dialog(); });
    auto welcome_actions = horizontal(8);
    welcome_actions.HorizontalAlignment(HorizontalAlignment::Center);
    welcome_actions.Margin(pad(0, 7, 0, 0));
    welcome_actions.Children().Append(choose);
    welcome_actions.Children().Append(choose_folder);
    welcome.Children().Append(welcome_actions);
    empty_.Child(welcome);
    main.Children().Append(empty_);
    build_folder_panel(main);
    workarea_.Visibility(Visibility::Collapsed);

    auto footer = horizontal(8);
    footer.Background(brush(panel));
    footer.Padding(pad(14, 5, 14, 5));
    status_icon_ = label(L"●", 12, accent);
    status_ = label(tr(L"Prêt"), 12, secondary);
    footer.Children().Append(status_icon_);
    footer.Children().Append(status_);
    place(root_, footer, 3);
    window_.ExtendsContentIntoTitleBar(true);
    const auto caption = window_.AppWindow().TitleBar();
    caption.PreferredHeightOption(Microsoft::UI::Windowing::TitleBarHeightOption::Standard);
    caption.ButtonForegroundColor(color(primary));
    caption.ButtonInactiveForegroundColor(color(secondary));
    caption.ButtonBackgroundColor(Windows::UI::Color{0, 0, 0, 0});
    caption.ButtonInactiveBackgroundColor(Windows::UI::Color{0, 0, 0, 0});
    caption.ButtonHoverBackgroundColor(color(surface));
    window_.Content(root_);
    apply_theme();
}

void Workspace::status(std::wstring_view message, bool error)
{
    status_.Text(error ? localized_error(message) : std::wstring(message));
    status_icon_.Foreground(brush(error ? danger : accent));
    status_.Foreground(brush(error ? danger : secondary));
}

void Workspace::open_dialog()
{
    if (busy_)
        return;
    try
    {
        if (const auto path = dialog(window_, false, L".swf"))
            open_file(*path);
    }
    catch (const hresult_error& error)
    {
        status(error.message(), true);
    }
}

void Workspace::open_folder_dialog()
{
    if (busy_)
        return;
    try
    {
        if (const auto folder = dialog(window_, false, L"", {}, nullptr, true))
            open_folder(*folder);
    }
    catch (const hresult_error& error) { status(error.message(), true); }
}

fire_and_forget Workspace::drop(DragEventArgs args)
{
    auto lifetime = shared_from_this();
    const auto deferral = args.GetDeferral();
    try
    {
        const auto items = co_await args.DataView().GetStorageItemsAsync();
        for (const auto& item : items)
        {
            if (closed_)
                break;
            const auto file = item.try_as<StorageFile>();
            if (file)
                co_await open_file(std::filesystem::path(file.Path().c_str()));
            else if (const auto folder = item.try_as<StorageFolder>())
                co_await open_folder(std::filesystem::path(folder.Path().c_str()));
            else
                status(tr(L"Déposez un fichier SWF ou un dossier."), true);
        }
    }
    catch (const hresult_error& error)
    {
        if (!closed_)
            status(error.message(), true);
    }
    deferral.Complete();
}

Windows::Foundation::IAsyncAction Workspace::open_file(std::filesystem::path path, std::optional<std::size_t> initial_tag,
    bool offer_cleanup)
{
    if (busy_ || closed_)
        co_return;
    auto lifetime = shared_from_this();
    std::error_code path_error;
    if (auto absolute = std::filesystem::absolute(path, path_error); !path_error)
        path = absolute.lexically_normal();
    for (std::size_t index = 0; index < documents_.size(); ++index)
        if (CompareStringOrdinal(documents_[index].path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
        {
            activate_document(index);
            remember_recent(path);
            co_return;
        }
    busy_ = true;
    open_.IsEnabled(false);
    save_.IsEnabled(false);
    documents_bar_.IsEnabled(false);
    replace_.IsEnabled(false);
    status(tr(L"Lecture de ") + path.filename().wstring() + L"…");
    const apartment_context ui;
    co_await resume_background();
    std::shared_ptr<flash::Movie> movie;
    std::vector<Row> rows;
    std::wstring problem;
    flash::As3Obfuscation detection;
    try
    {
        const auto extension = lower(path.extension().wstring());
        movie = std::make_shared<flash::Movie>(extension == L".xml" ?
            flash::Movie::import_xml(flash::read_file(path)) : flash::Movie::open(path));
        rows = index_rows(*movie);
        if (offer_cleanup)
            detection = flash::detect_as3_obfuscation(*movie);
    }
    catch (const std::exception& error) { problem = widen(error.what()); }
    co_await ui;
    if (closed_)
        co_return;
    busy_ = false;
    open_.IsEnabled(true);
    documents_bar_.IsEnabled(true);
    replace_.IsEnabled(true);
    if (problem.empty())
    {
        remember_recent(path);
        if (initial_tag && *initial_tag >= movie->tags().size())
            problem = tr(L"Index du tag hors limites.");
        add_document(std::move(path), std::move(movie), std::move(rows), problem.empty() ? initial_tag : std::nullopt);
        status(tr(L"Fichier ouvert"));
    }
    if (!problem.empty())
    {
        sync_folder_selection(false);
        status(problem, true);
    }
    save_.IsEnabled(static_cast<bool>(movie_));
    if ((detection.methods || detection.identifiers) && problem.empty())
        co_await prompt_deobfuscation(detection);
}

Windows::Foundation::IAsyncAction Workspace::prompt_deobfuscation(flash::As3Obfuscation detection)
{
    const auto lifetime = shared_from_this();
    const auto input = movie_;
    if (!input || busy_ || closed_)
        co_return;
    bool accepted = false;
    try
    {
        busy_ = true;
        ContentDialog prompt;
        prompt.XamlRoot(root_.XamlRoot());
        prompt.Title(box_value(tr(L"Obfuscation ActionScript possible")));
        auto message = label(format_message(
            L"Ce SWF semble obfusqué ({} méthodes et {} identifiants suspects). Voulez-vous le désobfusquer ? Le résultat sera enregistré dans une copie.",
            detection.methods, detection.identifiers));
        message.TextWrapping(TextWrapping::WrapWholeWords);
        message.TextTrimming(TextTrimming::None);
        prompt.Content(message);
        prompt.PrimaryButtonText(tr(L"Désobfusquer"));
        prompt.CloseButtonText(tr(L"Plus tard"));
        prompt.DefaultButton(ContentDialogButton::Primary);
        accepted = co_await prompt.ShowAsync() == ContentDialogResult::Primary;
    }
    catch (const hresult_error& error) { if (!closed_) status(error.message(), true); }
    busy_ = false;
    if (accepted && !closed_ && movie_ == input)
        co_await deobfuscate_swf(true);
}

Windows::Foundation::IAsyncAction Workspace::deobfuscate_swf(bool whole_document)
{
    if (!movie_ || busy_ || closed_ || (!whole_document && (!selected_ || !is_abc(movie_->tags()[*selected_].code))))
        co_return;
    const auto lifetime = shared_from_this();
    const auto input = movie_;
    const auto current = mode_ == Mode::scripts && selected_script_ ? &script_entries_.at(*selected_script_) : nullptr;
    auto target = current && current->type ?
        std::optional(ScriptTarget{current->tag, current->type, current->qualified, {}, {}, View::source}) : std::nullopt;
    const auto tag = whole_document ? std::nullopt : selected_;
    const auto type = tag && current && current->tag == *tag ? current->type : std::nullopt;
    const apartment_context ui;
    const auto set_busy = [&](bool working)
    {
        busy_ = working;
        open_.IsEnabled(!working);
        save_.IsEnabled(!working);
        documents_bar_.IsEnabled(!working);
        script_tabs_bar_.IsEnabled(!working);
        deobfuscate_.IsEnabled(!working);
    };
    set_busy(true);
    status(tr(L"Désobfuscation du bytecode ActionScript…"));
    co_await resume_background();
    std::shared_ptr<const flash::Movie> cleaned;
    flash::DeobfuscationReport report;
    std::vector<Row> rows;
    std::wstring problem;
    try
    {
        auto result = flash::deobfuscate_as3(*input, tag, type);
        report = result.report;
        if (result.movie)
            cleaned = std::make_shared<flash::Movie>(std::move(*result.movie));
    }
    catch (const std::exception& error) { problem = widen(error.what()); }
    co_await ui;
    if (closed_)
        co_return;
    bool simplify_expressions = false;
    if (problem.empty())
    {
        try
        {
            ContentDialog prompt;
            prompt.XamlRoot(root_.XamlRoot());
            prompt.Title(box_value(tr(L"Simplifier les expressions et les noms ?")));
            auto message = label(tr(L"Remplacer les appels aux décodeurs reconnus par leurs chaînes et renommer les identifiants obfusqués dans tout le SWF ? Flash Studio analyse le bytecode sans lancer le SWF."));
            message.TextWrapping(TextWrapping::WrapWholeWords);
            message.TextTrimming(TextTrimming::None);
            prompt.Content(message);
            prompt.PrimaryButtonText(tr(L"Simplifier"));
            prompt.CloseButtonText(tr(L"Passer la simplification"));
            prompt.DefaultButton(ContentDialogButton::Close);
            simplify_expressions = co_await prompt.ShowAsync() == ContentDialogResult::Primary;
        }
        catch (const hresult_error& error) { problem = error.message(); }
    }
    if (closed_)
        co_return;
    if (problem.empty() && (cleaned || simplify_expressions))
    {
        co_await resume_background();
        try
        {
            if (simplify_expressions)
            {
                auto renamed = flash::rename_as3_identifiers(cleaned ? *cleaned : *input);
                if (renamed.movie)
                    cleaned = std::make_shared<flash::Movie>(std::move(*renamed.movie));
                report.renamed_identifiers = renamed.report.renamed_identifiers;
                auto simplified = flash::simplify_as3_strings(cleaned ? *cleaned : *input, tag, type);
                if (simplified.movie)
                    cleaned = std::make_shared<flash::Movie>(std::move(*simplified.movie));
                report.resolved_strings = simplified.report.resolved_strings;
            }
            if (cleaned)
                rows = index_rows(*cleaned);
        }
        catch (const std::exception& error) { problem = widen(error.what()); }
        co_await ui;
    }
    if (closed_)
        co_return;
    if (!cleaned || !problem.empty())
    {
        set_busy(false);
        if (problem.empty())
            status(tr(L"Aucune modification à enregistrer."));
        else
            status(problem, true);
        co_return;
    }

    set_busy(false);
    if (!apply_document_edit(input, std::move(cleaned), std::move(rows), tag, report.renamed_identifiers != 0))
        co_return;
    if (target)
    {
        if (report.renamed_identifiers)
            target->qualified.clear();
        open_script_target(*target);
    }
    else if (mode_ != Mode::scripts)
        switch_mode(Mode::scripts);
    auto summary = format_message(L"{} méthodes nettoyées · {} branchements constants simplifiés · {} méthodes ignorées",
        report.changed_methods, report.constant_branches, report.skipped_methods);
    if (simplify_expressions)
        summary += format_message(L" · {} appels remplacés par des chaînes · {} identifiants renommés",
            report.resolved_strings, report.renamed_identifiers);
    status(summary);
}

void Workspace::update_document()
{
    const auto& header = movie_->header();
    file_info_.Text(format_message(L"SWF {}  ·  {}WS  ·  {:g} × {:g} px  ·  {:g} i/s",
        header.version, static_cast<wchar_t>(active_document_ ? documents_[*active_document_].disk_compression : header.compression),
        (static_cast<double>(header.right) - header.left) / 20.0,
        (static_cast<double>(header.bottom) - header.top) / 20.0, header.frames_per_second));
    sync_folder_selection();
    if (active_document_)
        update_tab_label(*active_document_);
    empty_.Visibility(Visibility::Collapsed);
    workarea_.Visibility(Visibility::Visible);
    global_search_.IsEnabled(true);
    document_class_.Visibility(movie_->document_class() ? Visibility::Visible : Visibility::Collapsed);
    reset_explorer();
    if (active_document_)
    {
        auto& catalog = documents_[*active_document_].catalog;
        if (catalog)
        {
            script_entries_ = std::move(catalog->entries);
            script_errors_ = catalog->errors;
            scripts_indexed_ = true;
            catalog.reset();
        }
    }
    {
        const UpdatingSelection updating(refreshing_);
        selected_.reset();
        abc_.reset();
        abc_method_.reset();
        search_.Text(L"");
    }
    switch_mode(mode_);
}

std::optional<flash::Category> Workspace::category_filter() const
{
    const auto index = categories_.SelectedIndex();
    return index > 0 ? std::optional(category_filters_[index - 1]) : std::nullopt;
}

void Workspace::populate_categories()
{
    const UpdatingSelection updating(refreshing_);
    categories_.Items().Clear();
    category_filters_.clear();
    const bool gallery = mode_ == Mode::resources;
    categories_.Items().Append(icon_label(L'\uE8A9', gallery ? tr(L"Tout afficher") : tr(L"Tous les tags")));
    std::array<std::size_t, filters.size()> counts{};
    for (const auto& tag : movie_->tags())
        if (!gallery || resource(tag))
            ++counts[static_cast<std::size_t>(tag.category)];
    for (const auto& [category, icon] : filters)
    {
        const auto count = counts[static_cast<std::size_t>(category)];
        if (gallery && count == 0)
            continue;
        category_filters_.push_back(category);
        categories_.Items().Append(icon_label(icon, std::format(L"{}  {}", tr(flash::category_name(category).data()), count)));
    }
    const auto restore = gallery && previous_category_ ? std::ranges::find(category_filters_, *previous_category_) : category_filters_.end();
    categories_.SelectedIndex(restore == category_filters_.end() ? 0 : static_cast<int>(restore - category_filters_.begin()) + 1);
}

std::vector<Workspace::Row> Workspace::index_rows(const flash::Movie& movie)
{
    std::vector<Row> rows;
    rows.reserve(movie.tags().size());
    for (std::size_t index = 0; index < movie.tags().size(); ++index)
    {
        const auto& tag = movie.tags()[index];
        auto title = widen(flash::tag_name(tag.code));
        if (tag.character_id)
            title += std::format(L"  #{}", *tag.character_id);
        if (!tag.name.empty())
        {
            auto name = widen(tag.name);
            for (auto& character : name)
                if (character < 32 || character == 127)
                    character = L' ';
            title += L"  ·  " + name;
        }
        auto search_key = lower(title);
        // WinUI #11226: equal boxed values can trap the UI Automation sibling walker.
        title = std::format(L"{:05}  {}{}", index, tag.parent ? L"↳ " : L"", title);
        rows.push_back({std::move(title), std::move(search_key)});
    }
    return rows;
}

void Workspace::refresh_tags()
{
    if (!movie_ || refreshing_ || mode_ != Mode::tags)
        return;
    matches_.clear();
    matches_.reserve(movie_->tags().size());
    const auto category = category_filter();
    const auto query = lower(search_.Text().c_str());
    const auto numeric = number(query);
    std::size_t page = 0;
    for (std::size_t i = 0; i < movie_->tags().size(); ++i)
    {
        const auto& tag = movie_->tags()[i];
        if (category && tag.category != category)
            continue;
        if (!query.empty() && numeric != i && numeric != tag.code &&
            indexed_rows_[i].search_key.find(query) == std::wstring::npos)
            continue;
        if (selected_ == i)
            page = matches_.size() / tags_per_page;
        matches_.push_back(i);
    }
    empty_list_.Visibility(matches_.empty() ? Visibility::Visible : Visibility::Collapsed);
    list_count_.Text(std::format(L"Tags  ·  {}", matches_.size()));
    show_page(page);
}

void Workspace::show_page(std::size_t page)
{
    page_ = page;
    page_start_ = page * tags_per_page;
    page_end_ = std::min(page_start_ + tags_per_page, matches_.size());
    std::vector<IInspectable> rows;
    rows.reserve(page_end_ - page_start_);
    int restore = -1;
    for (std::size_t i = page_start_; i < page_end_; ++i)
    {
        const auto index = matches_[i];
        if (selected_ == index)
            restore = static_cast<int>(i - page_start_);
        rows.push_back(box_value(hstring(indexed_rows_[index].title)));
    }
    {
        const UpdatingSelection updating(refreshing_);
        tags_.ItemsSource(single_threaded_observable_vector<IInspectable>(std::move(rows)));
        tags_.SelectedIndex(restore);
        if (restore >= 0)
            tags_.ScrollIntoView(tags_.SelectedItem());
    }
    previous_page_.IsEnabled(page_ != 0);
    next_page_.IsEnabled(page_end_ != matches_.size());
    page_label_.Text(matches_.empty() ? L"" : std::format(L"{}–{} / {}", page_start_ + 1, page_end_, matches_.size()));
    if (restore < 0)
        select_tag(std::nullopt);
}

void Workspace::select_tag(std::optional<std::size_t> index, std::optional<View> desired_view)
{
    if (!movie_ || source_session_)
        return;
    const bool changed = selected_ != index;
    selected_ = index;
    const bool editable_shape = index && flash::is_editable_shape(movie_->tags()[*index].code);
    shape_edit_.Visibility(editable_shape ? Visibility::Visible : Visibility::Collapsed);
    if (changed)
    {
        abc_.reset();
        abc_method_.reset();
        abc_groups_.clear();
    }
    auto next = view_;
    bool script = false, can_replace = false, scene = !index;
    if (index)
    {
        const auto& tag = movie_->tags()[*index];
        scene = tag.code == 39;
        script = is_abc(tag.code) || flash::has_avm1_actions(*movie_, *index);
        can_replace = (tag.code == 87 && tag.size >= 6) || is_text(tag.code) || tag.code == 14 || flash::is_bitmap_tag(tag.code);
        replace_.Content(box_value(is_text(tag.code) ? tr(L"Modifier…") : tr(L"Remplacer…")));
        Automation::AutomationProperties::SetName(replace_, is_text(tag.code) ? tr(L"Modifier le texte") :
            tag.code == 14 ? tr(L"Remplacer le son") : flash::is_bitmap_tag(tag.code) ? tr(L"Remplacer l’image") :
            tr(L"Remplacer les données binaires"));
        detail_title_.Text(widen(flash::tag_name(tag.code)));
        detail_meta_.Text(format_message(L"Tag {}  ·  0x{:X}  ·  {} octets", *index, tag.offset, tag.size));
    }
    else
    {
        detail_title_.Text(tr(L"Aperçu du fichier"));
        detail_meta_.Text(tr(L"Sélectionnez un tag pour l’inspecter"));
    }
    export_.IsEnabled(!busy_);
    export_asset_.IsEnabled(index.has_value());
    export_raw_.IsEnabled(index.has_value());
    animation_export_.IsEnabled(index && movie_->tags()[*index].code == 39);
    export_source_.IsEnabled(script);
    export_assembly_.IsEnabled(script);
    tabs_[1].IsEnabled(script);
    tabs_[2].IsEnabled(script);
    tabs_[3].IsEnabled(index.has_value());
    tabs_[4].IsEnabled(scene);
    for (std::size_t i = 0; i < tabs_.size(); ++i)
        tabs_[i].Visibility((mode_ != Mode::tags || editable_shape) && !tabs_[i].IsEnabled() ?
            Visibility::Collapsed : Visibility::Visible);
    if (view_ == View::frame && !scene)
        next = View::details;
    replace_.Visibility(can_replace ? Visibility::Visible : Visibility::Collapsed);
    if (!index || (!script && (view_ == View::source || view_ == View::instructions)))
        next = View::details;
    if (changed && script && next == View::details)
        next = View::source;
    if (changed && index && scene)
        next = View::frame;
    update_layout();
    set_view(desired_view.value_or(next));
}

void Workspace::set_view(View view)
{
    if (source_session_)
        return;
    view_ = view;
    convert_image_.IsEnabled(movie_ && (view == View::frame || (selected_ && flash::is_visual_tag(movie_->tags()[*selected_].code))));
    for (std::size_t i = 0; i < tabs_.size(); ++i)
        tabs_[i].IsChecked(i == static_cast<std::size_t>(view));
    frame_bar_.Visibility(view == View::frame ? Visibility::Visible : Visibility::Collapsed);
    update_abc_visibility();
    const auto type = selected_script_ ? script_entries_[*selected_script_].type : std::nullopt;
    if (type && abc_)
    {
        const auto& info = abc_->classes()[*type];
        detail_title_.Text(widen(info.qualified));
        detail_meta_.Text(format_message(L"{}  /  {} membres  /  ABC {}.{}", info.interface_type ? L"Interface" : tr(L"Classe"),
            info.members.size(), abc_->major_version(), abc_->minor_version()));
    }
    else if (view != View::frame && movie_)
    {
        if (selected_)
        {
            const auto& tag = movie_->tags()[*selected_];
            detail_meta_.Text(format_message(L"Tag {}  ·  0x{:X}  ·  {} octets", *selected_, tag.offset, tag.size));
        }
        else
            detail_meta_.Text(tr(L"Sélectionnez un tag pour l’inspecter"));
    }
    show_selection();
}

void Workspace::update_abc_visibility()
{
    const auto has_abc = movie_ && selected_ && is_abc(movie_->tags()[*selected_].code);
    const auto editing = has_abc && (view_ == View::source || view_ == View::instructions);
    const auto has_body = abc_ && abc_method_ && abc_->methods().at(*abc_method_).code_size.has_value();
    abc_tools_.Visibility(editing ? Visibility::Visible : Visibility::Collapsed);
    const bool class_open = mode_ == Mode::scripts && selected_script_ && script_entries_[*selected_script_].type;
    const bool pcode = has_abc && has_body && view_ == View::instructions;
    source_edit_.Content(box_value(pcode ? tr(L"Éditer le P-code") : tr(L"Éditer le code")));
    source_edit_.Visibility((pcode || (class_open && view_ == View::source)) && !source_session_ ?
        Visibility::Visible : Visibility::Collapsed);
    edit_class_.Visibility(class_open ? Visibility::Visible : Visibility::Collapsed);
    abc_owner_.Visibility(class_open ? Visibility::Collapsed : Visibility::Visible);
    abc_method_list_.Visibility(class_open ? Visibility::Collapsed : Visibility::Visible);
    abc_owner_.IsEnabled(static_cast<bool>(abc_));
    abc_method_list_.IsEnabled(static_cast<bool>(abc_));
    edit_pcode_.IsEnabled(has_body);
    edit_simplified_.IsEnabled(has_body && abc_->methods()[*abc_method_].handlers == 0);
    export_pcode_.IsEnabled(has_body);
    export_graph_.IsEnabled(has_body);
    if (class_open)
        export_assembly_.IsEnabled(has_body);
}

void Workspace::configure_abc(std::shared_ptr<const flash::AbcModule> module, unsigned method)
{
    const UpdatingSelection updating(refreshing_);
    abc_ = std::move(module);
    abc_groups_.clear();
    std::map<std::string, std::vector<unsigned>> classes;
    for (const auto& item : abc_->methods())
        classes[item.owner].push_back(item.id);
    std::size_t active_group = 0;
    abc_owner_.Items().Clear();
    for (auto& [name, methods] : classes)
    {
        if (std::ranges::find(methods, method) != methods.end())
            active_group = abc_groups_.size();
        abc_owner_.Items().Append(box_value(hstring(name.empty() ? tr(L"Scripts et méthodes libres") : widen(name))));
        abc_groups_.push_back({name, std::move(methods)});
    }
    abc_owner_.SelectedIndex(abc_groups_.empty() ? -1 : static_cast<int>(active_group));
    if (!abc_groups_.empty())
        show_abc_methods(active_group, method);
    update_abc_visibility();
}

void Workspace::show_abc_methods(std::size_t group, std::optional<unsigned> selected)
{
    const UpdatingSelection updating(refreshing_);
    const auto& methods = abc_groups_.at(group).methods;
    const auto items = abc_method_list_.Items();
    items.Clear();
    const auto chosen = selected ? std::ranges::find(methods, *selected) :
        std::ranges::find_if(methods, [&](unsigned id) { return abc_->methods()[id].code_size.has_value(); });
    const auto index = chosen == methods.end() ? 0 : static_cast<int>(chosen - methods.begin());
    for (const auto id : methods)
    {
        const auto& method = abc_->methods()[id];
        items.Append(box_value(hstring(std::format(L"{:04}  {}", id, widen(method.name)))));
    }
    abc_method_list_.SelectedIndex(index);
    abc_method_ = methods.at(index);
    update_abc_visibility();
}

void Workspace::preview_problem(std::uint64_t version, std::wstring_view message)
{
    if (GetCurrentThreadId() != ui_thread_ || closed_ || version != preview_version_)
        return;
    preview_error_.Text(localized_error(message));
    preview_error_.Visibility(Visibility::Visible);
    progress_.IsActive(false);
    progress_.Visibility(Visibility::Collapsed);
}

fire_and_forget Workspace::show_selection()
{
    if (source_session_)
        co_return;
    auto lifetime = shared_from_this();
    const auto version = ++preview_version_;
    hide_resource_preview();
    if (view_ == View::frame && movie_)
    {
        open_frame_player();
        co_return;
    }
    close_frame_player();
    const auto target_line = std::exchange(next_code_line_, std::nullopt);
    try
    {
        picture_view_.Visibility(Visibility::Collapsed);
        image_.Source(nullptr);
        if (audio_.Visibility() == Visibility::Visible)
        {
            audio_.Source(nullptr);
            audio_.Visibility(Visibility::Collapsed);
        }
        hide_code();
        preview_error_.Visibility(Visibility::Collapsed);
        progress_.Visibility(Visibility::Collapsed);
        progress_.IsActive(false);
        output_.Visibility(Visibility::Visible);
        if (!movie_)
            co_return;
        if (!selected_)
        {
            if (mode_ == Mode::scripts)
            {
                detail_title_.Text(L"ActionScript");
                detail_meta_.Text(tr(L"Ouvrez un package pour afficher ses classes."));
                output_.Text(L"");
                output_.Visibility(Visibility::Collapsed);
            }
            else
                output_.Text(widen(flash::summarize(*movie_)));
            co_return;
        }
        const auto movie = movie_;
        const auto selected = selected_;
        const auto index = selected.value_or(0);
        const auto view = view_;
        const auto& tag = movie->tags()[index];
        auto abc = abc_;
        auto method = abc_method_;
        const auto type = selected_script_ ? script_entries_[*selected_script_].type : std::nullopt;
        if (view == View::hex)
        {
            show_hex(movie, index, false);
            co_return;
        }
        if (view == View::details && tag.code == 87 && tag.size >= 6)
        {
            show_hex(movie, index, true);
            co_return;
        }
        const bool is_font = tag.category == flash::Category::fonts &&
            (tag.code == 10 || tag.code == 48 || tag.code == 75 || tag.code == 91);
        if (view == View::details)
        {
            output_.Text(widen(flash::inspect(*movie, index)));
            if (tag.category != flash::Category::images && tag.category != flash::Category::shapes &&
                !is_text(tag.code) && tag.code != 14 && !is_font)
                co_return;
        }
        else
            output_.Text(L"");
        progress_.Visibility(Visibility::Visible);
        progress_.IsActive(true);
        const apartment_context ui;
        co_await resume_background();
        std::optional<flash::Asset> asset;
        std::optional<flash::FontPreview> font;
        std::string text, failure;
        std::vector<flash::AbcClassSource::MemberRange> member_ranges;
        try
        {
            if ((view == View::source || view == View::instructions) && is_abc(tag.code))
            {
                if (!abc)
                    abc = std::make_shared<const flash::AbcModule>(flash::abc_bytecode(*movie, index));
                if (!type && !method && !abc->methods().empty())
                {
                    const auto found = std::ranges::find_if(abc->methods(), [](const flash::AbcMethod& item) { return item.code_size.has_value(); });
                    method = found == abc->methods().end() ? abc->methods().front().id : found->id;
                }
                if (type && view == View::source)
                {
                    auto source = abc->decompile_class(*type);
                    text = std::move(source.text);
                    member_ranges = std::move(source.members);
                }
                else if (type && !method)
                    text = "; Choisissez une méthode dans le panneau Membres.";
                else if (!method)
                    text = "Le bloc ABC ne contient aucune méthode.";
                else if (view == View::source)
                    text = abc->decompile(*method);
                else
                {
                    try { text = abc->pcode(*method); }
                    catch (const std::runtime_error&) { text = abc->disassemble(*method); }
                }
            }
            else if (view == View::source)
                text = flash::decompile(*movie, index);
            else if (view == View::instructions)
                text = flash::disassemble(*movie, index);
            else if (is_font)
                font = flash::font_preview(*movie, index);
            else if (is_text(tag.code))
            {
                const auto extracted = flash::extract_text(*movie, index);
                text = flash::inspect(*movie, index) + "\n" + extracted;
            }
            else
                asset = flash::extract(*movie, index);
        }
        catch (const std::exception& error) { failure = error.what(); }
        co_await ui;
        if (closed_ || version != preview_version_)
            co_return;
        try
        {
            if (abc && abc != abc_)
                configure_abc(std::move(abc), method.value_or(0));
            if (!type && abc_ && method && (view == View::source || view == View::instructions))
            {
                const auto& info = abc_->methods().at(*method);
                detail_meta_.Text(format_message(L"ABC {}.{}  ·  {} méthodes  ·  {}",
                    abc_->major_version(), abc_->minor_version(), abc_->methods().size(), widen(info.signature)));
            }
            if (!failure.empty())
                output_.Text(widen((view == View::details ? flash::inspect(*movie, index) + '\n' : "") + failure));
            else if (!text.empty())
            {
                if (view == View::source || view == View::instructions)
                {
                    member_ranges_ = std::move(member_ranges);
                    show_code(text, view == View::instructions);
                    root_.DispatcherQueue().TryEnqueue([this, lifetime, version, target_line, class_source = type && view == View::source]
                    {
                        if (!closed_ && version == preview_version_)
                        {
                            if (!target_line && class_source && members_.SelectedIndex() >= 0)
                                jump_to_member();
                            else
                                jump_to_code_line(target_line.value_or(1));
                        }
                    });
                }
                else
                    output_.Text(widen(text));
            }
            else if (font)
                show_font(movie, index, std::move(*font));
            else if (asset && (asset->extension == L".wav" || asset->extension == L".mp3"))
            {
                InMemoryRandomAccessStream stream;
                DataWriter writer(stream);
                writer.WriteBytes(asset->bytes);
                co_await writer.StoreAsync();
                writer.DetachStream();
                stream.Seek(0);
                if (closed_ || version != preview_version_)
                    co_return;
                const auto mime = asset->extension == L".wav" ? L"audio/wav" : L"audio/mpeg";
                audio_.Source(Windows::Media::Core::MediaSource::CreateFromStream(stream, mime));
                audio_.Visibility(Visibility::Visible);
                output_.Visibility(Visibility::Collapsed);
            }
            else if (asset && (asset->extension == L".png" || asset->extension == L".jpg" ||
                asset->extension == L".gif" || asset->extension == L".svg"))
            {
                InMemoryRandomAccessStream stream;
                DataWriter writer(stream);
                writer.WriteBytes(asset->bytes);
                co_await writer.StoreAsync();
                writer.DetachStream();
                stream.Seek(0);
                ImageSource source{nullptr};
                if (asset->extension == L".svg")
                {
                    SvgImageSource svg;
                    if (co_await svg.SetSourceAsync(stream) != SvgImageSourceLoadStatus::Success)
                        throw hresult_error(E_FAIL, tr(L"Impossible d’afficher la forme SVG."));
                    source = svg;
                }
                else
                {
                    BitmapImage bitmap;
                    bitmap.DecodePixelWidth(1200);
                    co_await bitmap.SetSourceAsync(stream);
                    source = bitmap;
                }
                if (closed_ || version != preview_version_)
                    co_return;
                image_.Source(source);
                picture_view_.Visibility(Visibility::Visible);
                picture_view_.ChangeView(0.0, 0.0, 1.0f, true);
                output_.Visibility(Visibility::Collapsed);
            }
        }
        catch (const hresult_error& error)
        {
            if (closed_ || version != preview_version_)
                co_return;
            preview_error_.Text(error.message());
            preview_error_.Visibility(Visibility::Visible);
        }
        progress_.IsActive(false);
        progress_.Visibility(Visibility::Collapsed);
    }
    catch (const hresult_error& error)
    {
        preview_problem(version, error.message());
    }
    catch (const std::exception& error)
    {
        preview_problem(version, widen(error.what()));
    }
}

fire_and_forget Workspace::export_tag(Export format)
{
    if (!selected_ || !movie_)
        co_return;
    auto lifetime = shared_from_this();
    const auto movie = movie_;
    const auto index = *selected_;
    const auto& tag = movie->tags()[index];
    if (format == Export::asset && (tag.code == 39 || (tag.category == flash::Category::shapes && flash::is_visual_tag(tag.code))))
    {
        export_image();
        co_return;
    }
    const auto abc = abc_;
    const auto method = abc_method_;
    const auto type = selected_script_ ? script_entries_[*selected_script_].type : std::nullopt;
    const apartment_context ui;
    try
    {
        co_await resume_background();
        std::optional<flash::Asset> asset;
        std::wstring extension = L".bin";
        std::string content;
        std::wstring failure;
        try
        {
            if (format == Export::asset)
            {
                asset = flash::extract(*movie, index);
                if (asset)
                    extension = asset->extension;
                else
                    failure = tr(L"Ce tag peut être exporté en contenu brut.");
            }
            else if (format == Export::pcode || format == Export::graph)
            {
                if (!abc || !method)
                    throw std::runtime_error("Sélectionnez une méthode ABC qui possède un corps.");
                extension = format == Export::pcode ? L".pcode" : L".dot";
                content = format == Export::pcode ? abc->pcode(*method) : abc->flow_graph(*method);
            }
            else if (format == Export::source || format == Export::instructions)
            {
                extension = format == Export::source ? L".as" : L".txt";
                if (abc && type && format == Export::source)
                    content = abc->decompile_class(*type).text;
                else if (abc && method)
                    content = format == Export::source ? abc->decompile(*method) : abc->disassemble(*method);
                else
                    content = format == Export::source ? flash::decompile(*movie, index) : flash::disassemble(*movie, index);
            }
        }
        catch (const std::exception& error) { failure = widen(error.what()); }
        co_await ui;
        if (closed_)
            co_return;
        if (!failure.empty())
        {
            status(failure, true);
            co_return;
        }
        const auto name = std::format(L"{}-{}{}{}", type && format == Export::source ?
            widen(abc->classes()[*type].name) : widen(flash::tag_name(movie->tags()[index].code)), index,
            (format == Export::pcode || format == Export::graph) && method ? format_message(L"-méthode-{}", *method) : L"", extension);
        const auto destination = dialog(window_, true, extension, name);
        if (!destination)
            co_return;
        co_await resume_background();
        try
        {
            if (asset)
                flash::write_file(*destination, asset->bytes);
            else if (format == Export::raw)
                flash::write_file(*destination, movie->data(index));
            else
                flash::write_text(*destination, content);
        }
        catch (const std::exception& error) { failure = widen(error.what()); }
        co_await ui;
        if (!closed_)
            status(failure.empty() ? tr(L"Exporté : ") + destination->filename().wstring() : failure, !failure.empty());
    }
    catch (const hresult_error& error)
    {
        if (!closed_)
            status(error.message(), true);
    }
}

Windows::Foundation::IAsyncOperation<double> Workspace::choose_export_scale()
{
    NumberBox scale;
    scale.Header(box_value(tr(L"Taille en pourcentage")));
    scale.Minimum(12.5);
    scale.Maximum(800);
    scale.SmallChange(25);
    scale.LargeChange(100);
    scale.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Inline);
    scale.Value(export_scale_ * 100);
    ContentDialog prompt;
    prompt.XamlRoot(root_.XamlRoot());
    prompt.Title(box_value(tr(L"Taille de l’export")));
    prompt.PrimaryButtonText(tr(L"Continuer"));
    prompt.CloseButtonText(tr(L"Annuler"));
    prompt.Content(scale);
    if (co_await prompt.ShowAsync() != ContentDialogResult::Primary || closed_ || !std::isfinite(scale.Value()))
        co_return 0;
    export_scale_ = scale.Value() / 100;
    co_return export_scale_;
}

fire_and_forget Workspace::export_image()
{
    if (!movie_ || busy_ || (!selected_ && view_ != View::frame))
        co_return;
    auto lifetime = shared_from_this();
    const auto movie = movie_;
    const auto index = selected_;
    const auto scene = view_ == View::frame;
    const auto frame = scene ? displayed_frame() : 1u;
    const auto vector = scene || movie->tags()[*index].category != flash::Category::images;
    double scale = 1;
    const apartment_context ui;
    std::optional<std::filesystem::path> target;
    try
    {
        if (scene || movie->tags()[*index].category == flash::Category::shapes || movie->tags()[*index].code == 39)
        {
            scale = co_await choose_export_scale();
            if (!scale || movie_ != movie)
                co_return;
        }
        const auto name = scene ? format_message(L"scene-{}", frame) : format_message(L"ressource-{}", *index);
        target = dialog(window_, true, vector ? L".svg" : L".png", name, nullptr, false, true, vector);
    }
    catch (const hresult_error& error) { status(error.message(), true); }
    if (!target)
        co_return;
    co_await resume_background();
    std::wstring failure;
    unsigned omitted = 0;
    try
    {
        auto asset = scene ? flash::render_frame(*movie, frame, index) : flash::FramePreview{flash::visual_asset(*movie, *index), 0};
        omitted = asset.omitted;
        const auto converted = flash::convert_image(asset.image, target->extension().wstring(), scale);
        flash::write_file(*target, converted.bytes);
    }
    catch (const std::exception& error) { failure = widen(error.what()); }
    co_await ui;
    if (!closed_)
        status(failure.empty() ? tr(L"Image exportée dans ") + target->filename().wstring() +
            (omitted ? format_message(L" · {} éléments ou effets omis", omitted) : L"") : failure, !failure.empty());
}

fire_and_forget Workspace::export_animation(flash::AnimationFormat format)
{
    if (!movie_ || busy_ || !selected_ || movie_->tags()[*selected_].code != 39)
        co_return;
    const auto lifetime = shared_from_this();
    const auto movie = movie_;
    const auto index = *selected_;
    const auto extension = flash::animation_extension(format);
    try
    {
        const auto scale = co_await choose_export_scale();
        if (!scale || movie_ != movie)
            co_return;
        auto path = dialog(window_, true, extension, std::format(L"sprite-{}", movie->tags()[index].character_id.value_or(0)));
        if (!path)
            co_return;
        if (lower(path->extension().wstring()) != extension)
        {
            status(format_message(L"Choisissez un nom de fichier avec l’extension {}.", extension), true);
            co_return;
        }
        status(tr(L"Export du sprite animé en cours…"));
        const apartment_context ui;
        co_await resume_background();
        unsigned omitted = 0;
        std::wstring failure;
        try
        {
            const auto animation = flash::export_sprite_animation(*movie, index, format, scale);
            flash::write_file(*path, animation.image.bytes);
            omitted = animation.frames_with_omissions;
        }
        catch (const std::exception& error) { failure = widen(error.what()); }
        co_await ui;
        if (!closed_ && movie == movie_)
            status(failure.empty() ? tr(L"Animation exportée dans ") + path->filename().wstring() +
                (omitted ? format_message(L" · {} images avec des éléments ou effets omis", omitted) : L"") : failure, !failure.empty());
    }
    catch (const hresult_error& error) { if (!closed_) status(error.message(), true); }
}

fire_and_forget Workspace::save_movie(bool choose_path)
{
    if (!active_document_ || busy_ || close_prompt_)
        co_return;
    const auto lifetime = shared_from_this();
    busy_ = true;
    co_await save_document(*active_document_, choose_path);
    busy_ = false;
}

Windows::Foundation::IAsyncOperation<bool> Workspace::save_document(std::size_t index, bool choose_path)
{
    const auto lifetime = shared_from_this();
    auto& document = documents_.at(index);
    if (!choose_path && !document.dirty)
        co_return true;
    const auto movie = document.movie;
    std::wstring failure;
    try
    {
        auto format = lower(document.path.extension().wstring()) == L".xml" ? DocumentFormat::xml :
            document.disk_compression == 'F' ? DocumentFormat::uncompressed : DocumentFormat::compressed;
        auto destination = document.path;
        if (choose_path)
        {
            const auto chosen = dialog(window_, true, L".swf", document.path.stem().wstring(), &format);
            if (!chosen)
                co_return false;
            destination = *chosen;
            for (std::size_t other = 0; other < documents_.size(); ++other)
            {
                if (other == index)
                    continue;
                std::error_code ignored;
                if (CompareStringOrdinal(destination.c_str(), -1, documents_[other].path.c_str(), -1, TRUE) == CSTR_EQUAL ||
                    std::filesystem::equivalent(destination, documents_[other].path, ignored))
                    throw std::runtime_error("Ce fichier est déjà ouvert dans un autre onglet.");
            }
        }
        const apartment_context ui;
        co_await resume_background();
        try { write_document_file(destination, *movie, format); }
        catch (const hresult_error& error) { failure = error.message(); }
        catch (const std::exception& error) { failure = widen(error.what()); }
        co_await ui;
        if (closed_)
            co_return false;
        if (failure.empty())
        {
            document.path = destination;
            document.disk_compression = format == DocumentFormat::compressed ? 'C' : 'F';
            document.dirty = false;
            update_tab_label(index);
            if (active_document_ == index)
            {
                file_ = destination;
                sync_folder_selection(false);
            }
            remember_recent(destination);
            status(tr(L"SWF enregistré : ") + destination.filename().wstring());
            co_return true;
        }
    }
    catch (const hresult_error& error) { failure = error.message(); }
    catch (const std::exception& error) { failure = widen(error.what()); }
    status(localized_error(failure), true);
    co_return false;
}

fire_and_forget Workspace::choose_visual_batch()
{
    if (!movie_ || busy_)
        co_return;
    const auto lifetime = shared_from_this();
    try
    {
        ComboBox formats;
        formats.Header(box_value(tr(L"Format de destination")));
        const wchar_t* labels[]{L"PNG", tr(L"SVG · ressources vectorielles"), L"BMP", L"JPEG", L"TIFF"};
        constexpr const wchar_t* extensions[]{L".png", L".svg", L".bmp", L".jpg", L".tiff"};
        for (const auto label : labels)
            formats.Items().Append(box_value(hstring(label)));
        formats.SelectedIndex(0);
        formats.HorizontalAlignment(HorizontalAlignment::Stretch);
        ContentDialog prompt;
        prompt.XamlRoot(root_.XamlRoot());
        prompt.Title(box_value(tr(L"Exporter les visuels")));
        prompt.Content(formats);
        prompt.PrimaryButtonText(tr(L"Choisir le dossier…"));
        prompt.CloseButtonText(tr(L"Annuler"));
        if (co_await prompt.ShowAsync() == ContentDialogResult::Primary && !closed_ && formats.SelectedIndex() >= 0)
            export_all(false, extensions[formats.SelectedIndex()]);
    }
    catch (const hresult_error& error) { if (!closed_) status(error.message(), true); }
}

fire_and_forget Workspace::export_folder_html5()
{
    if (busy_ || closed_)
        co_return;
    const auto source = folder_root();
    if (source.empty())
        co_return;
    const auto lifetime = shared_from_this();
    const auto selected = file_;
    flash::WebMovieOverrides open_movies;
    for (const auto& document : documents_)
        if (document.dirty)
            open_movies.emplace_back(document.path, document.movie);
    busy_ = true;
    folder_export_.IsEnabled(false);
    folder_button_.IsEnabled(false);
    const auto ready = [&]
    {
        busy_ = false;
        folder_export_.IsEnabled(true);
        folder_button_.IsEnabled(true);
    };
    const apartment_context ui;
    const auto previous_status = status_.Text();
    status(tr(L"Inventaire des SWF à exporter…"));
    co_await resume_background();
    flash::WebWorkspacePlan plan;
    std::wstring failure;
    try { plan = flash::plan_html5_workspace(source, open_movies); }
    catch (const std::exception& error) { failure = widen(error.what()); }
    co_await ui;
    if (closed_)
        co_return;
    if (!failure.empty() || plan.movies.empty())
    {
        ready();
        status(failure.empty() ? hstring(tr(L"Aucun SWF à exporter dans ce dossier.")) : hstring(failure), true);
        co_return;
    }

    std::optional<std::filesystem::path> destination;
    std::vector<std::size_t> selection;
    try
    {
        ContentDialog prompt;
        prompt.XamlRoot(root_.XamlRoot());
        prompt.Title(box_value(tr(L"SWF à exporter en HTML5")));
        prompt.PrimaryButtonText(tr(L"Choisir le dossier…"));
        prompt.CloseButtonText(tr(L"Annuler"));

        Grid content;
        content.RowSpacing(8);
        row(content);
        row(content);
        row(content);
        row(content, 1, GridUnitType::Star);
        TextBox search;
        search.PlaceholderText(tr(L"Filtrer les SWF…"));
        Automation::AutomationProperties::SetName(search, tr(L"Filtrer les SWF"));
        place(content, search, 0);
        StackPanel commands;
        commands.Orientation(Orientation::Horizontal);
        commands.Spacing(8);
        Button all, none;
        all.Content(box_value(tr(L"Tout cocher")));
        none.Content(box_value(tr(L"Tout décocher")));
        commands.Children().Append(all);
        commands.Children().Append(none);
        place(content, commands, 1);
        auto count = label(L"", 12, secondary);
        place(content, count, 2);
        ListView files;
        files.Height(std::clamp(root_.ActualHeight() - 285.0, 165.0, 440.0));
        files.SelectionMode(ListViewSelectionMode::None);
        files.ItemTemplate(Markup::XamlReader::Load(LR"(
            <DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
                <CheckBox MinWidth="0"/>
            </DataTemplate>)").as<DataTemplate>());
        Automation::AutomationProperties::SetName(files, tr(L"SWF à exporter en HTML5"));
        place(content, files, 3);
        prompt.Content(content);

        std::vector<std::wstring> names, keys;
        std::map<std::wstring, std::size_t> by_name;
        for (const auto& movie : plan.movies)
        {
            auto path = movie.path.lexically_relative(source);
            if (path.empty() || path.has_root_path() || *path.begin() == L"..")
                path = movie.path;
            auto& name = names.emplace_back(path.generic_wstring());
            auto& key = keys.emplace_back(lower(name));
            for (const auto& route : movie.routes)
                key += L" " + lower(widen(route));
            by_name.emplace(name, names.size() - 1);
        }
        std::vector<bool> checked(names.size(), true);
        std::vector<std::size_t> visible;
        const auto update_count = [&]
        {
            const auto total = std::ranges::count(checked, true);
            count.Text(format_message(L"{} SWF cochés sur {} · {} affichés", total, names.size(), visible.size()));
            prompt.IsPrimaryButtonEnabled(total != 0);
        };
        const auto rebuild = [&]
        {
            auto query = lower(search.Text().c_str());
            std::ranges::replace(query, L'\\', L'/');
            std::vector<IInspectable> items;
            visible.clear();
            for (std::size_t index = 0; index < names.size(); ++index)
                if (keys[index].find(query) != std::wstring::npos)
                {
                    visible.push_back(index);
                    items.push_back(box_value(hstring(names[index])));
                }
            files.ItemsSource(single_threaded_observable_vector<IInspectable>(std::move(items)));
            update_count();
        };
        std::vector<CheckBox::Click_revoker> toggles;
        const auto dialog_lifetime = std::make_shared<int>(0);
        using ItemPhase = Windows::Foundation::TypedEventHandler<ListViewBase, ContainerContentChangingEventArgs>;
        const ItemPhase prepare = [&, active = std::weak_ptr(dialog_lifetime)](
            const ListViewBase&, const ContainerContentChangingEventArgs& args)
        {
            if (active.expired() || args.InRecycleQueue())
                return;
            const auto box = args.ItemContainer().ContentTemplateRoot().try_as<CheckBox>();
            if (!box)
                return;
            const auto found = by_name.find(unbox_value<hstring>(args.Item()).c_str());
            if (found == by_name.end())
                return;
            if (!box.Tag())
            {
                toggles.push_back(box.Click(auto_revoke, [&](const IInspectable& sender, auto&&)
                {
                    const auto item = sender.as<CheckBox>();
                    const auto index = unbox_value<uint32_t>(item.Tag());
                    checked[index] = item.IsChecked().Value();
                    update_count();
                }));
            }
            box.Tag(box_value(static_cast<uint32_t>(found->second)));
            box.Content(args.Item());
            box.IsChecked(static_cast<bool>(checked[found->second]));
        };
        const auto prepared = files.ContainerContentChanging(auto_revoke,
            [&](const ListViewBase& sender, const ContainerContentChangingEventArgs& args)
        {
            if (!args.InRecycleQueue() && !args.ItemContainer().ContentTemplateRoot())
                args.RegisterUpdateCallback(prepare);
            else
                prepare(sender, args);
        });
        const auto set_all = [&](bool value)
        {
            std::fill(checked.begin(), checked.end(), value);
            for (std::size_t index = 0; index < visible.size(); ++index)
                if (const auto container = files.ContainerFromIndex(static_cast<int32_t>(index)).try_as<ListViewItem>())
                    if (const auto box = container.ContentTemplateRoot().try_as<CheckBox>())
                        box.IsChecked(value);
            update_count();
        };
        const auto filter = search.TextChanged(auto_revoke, [&](auto&&, auto&&) { rebuild(); });
        const auto select_all = all.Click(auto_revoke, [&](auto&&, auto&&) { set_all(true); });
        const auto deselect_all = none.Click(auto_revoke, [&](auto&&, auto&&) { set_all(false); });
        rebuild();
        if (co_await prompt.ShowAsync() == ContentDialogResult::Primary && !closed_)
        {
            for (std::size_t index = 0; index < checked.size(); ++index)
                if (checked[index])
                    selection.push_back(index);
            if (!selection.empty())
                destination = dialog(window_, false, L"", {}, nullptr, true);
        }
    }
    catch (const hresult_error& error) { failure = error.message(); }
    if (closed_)
        co_return;
    if (!destination)
    {
        ready();
        status(failure.empty() ? std::wstring_view(previous_status) : std::wstring_view(failure), !failure.empty());
        co_return;
    }

    const auto name = (source.filename().empty() ? std::wstring(L"Flash Studio") : source.filename().wstring()) + L"-html5";
    auto target = *destination / name;
    const auto dispatcher = root_.DispatcherQueue();
    const auto locale = language() == Language::english ? flash::WebLanguage::english : flash::WebLanguage::french;
    status(tr(L"Export HTML5…"));
    co_await resume_background();
    flash::WebWorkspaceReport report;
    try
    {
        for (unsigned sequence = 2; !std::filesystem::create_directory(target); ++sequence)
            target = *destination / std::format(L"{}-{}", name, sequence);
        report = flash::export_html5_workspace(plan, target, selection, selected, [dispatcher, lifetime](auto done, auto total)
        {
            dispatcher.TryEnqueue([lifetime, done, total]
            {
                if (!lifetime->closed_ && lifetime->busy_)
                    lifetime->status(format_message(L"Export HTML5 · {} sur {} SWF", done, total));
            });
        }, locale);
    }
    catch (const std::exception& error) { failure = widen(error.what()); }
    co_await ui;
    if (closed_)
        co_return;
    ready();
    if (!failure.empty())
        status(failure, true);
    else
        status(format_message(L"Espace HTML5 exporté dans {} · {} films · {} à compléter · {} échecs",
            target.wstring(), report.converted, report.partial, report.failed), report.failed != 0);
}

fire_and_forget Workspace::export_all(bool project, std::wstring visual_format, bool html5)
{
    if (!movie_ || busy_)
        co_return;
    const auto lifetime = shared_from_this();
    const auto movie = movie_;
    std::optional<std::filesystem::path> destination;
    try { destination = dialog(window_, false, L"", {}, nullptr, true); }
    catch (const hresult_error& error) { status(error.message(), true); }
    if (!destination)
        co_return;
    const auto source = file_;
    const auto folder = html5 ? folder_root() : std::filesystem::path{};
    const auto title = source.stem().wstring();
    const auto name = title + (html5 ? L"-html5" : !visual_format.empty() ? tr(L"-visuels-") + visual_format.substr(1) :
        project ? tr(L"-projet") : tr(L"-ressources"));
    auto target = *destination / name;
    busy_ = true;
    export_.IsEnabled(false);
    const auto locale = language() == Language::english ? flash::WebLanguage::english : flash::WebLanguage::french;
    status(html5 ? tr(L"Export HTML5…") : project ? tr(L"Export du projet…") : tr(L"Export des ressources…"));
    const apartment_context ui;
    co_await resume_background();
    std::wstring failure;
    flash::ExportReport report;
    try
    {
        for (unsigned sequence = 2; !std::filesystem::create_directory(target); ++sequence)
            target = *destination / std::format(L"{}-{}", name, sequence);
        report = html5 ? flash::export_html5(*movie, target, to_string(hstring(title)), source, folder, locale) :
            !visual_format.empty() ? flash::export_visuals(*movie, target, visual_format) :
            project ? flash::export_project(*movie, target) : flash::export_all(*movie, target);
    }
    catch (const std::exception& error) { failure = widen(error.what()); }
    co_await ui;
    if (closed_)
        co_return;
    busy_ = false;
    export_.IsEnabled(true);
    if (!failure.empty())
        status(failure, true);
    else if (html5)
        status(format_message(L"Projet HTML5 exporté dans {} · {} points de portage · LISEZ-MOI.txt",
            target.wstring(), report.failed), report.failed != 0);
    else
        status(format_message(L"{} fichiers exportés dans {} · {} tags en échec", report.files,
            target.filename().wstring(), report.failed), report.failed != 0);
}

fire_and_forget Workspace::edit_abc(AbcEdit operation)
{
    if (busy_ || !movie_ || !selected_ || !is_abc(movie_->tags()[*selected_].code))
        co_return;
    auto lifetime = shared_from_this();
    const auto movie = movie_;
    const auto abc = abc_;
    const auto tag = *selected_;
    const auto method = abc_method_;
    const auto script = selected_script_ ? std::optional(script_entries_[*selected_script_].qualified) : std::nullopt;
    if (operation != AbcEdit::import && !abc)
        co_return;
    std::optional<std::filesystem::path> input;
    flash::Bytes compiled;
    try
    {
        if (operation == AbcEdit::import)
            input = dialog(window_, false, L".abc");
        else
        {
            TextBox editor;
            editor.AcceptsReturn(true);
            editor.FontFamily(FontFamily(L"Consolas"));
            editor.TextWrapping(TextWrapping::NoWrap);
            editor.MinWidth(500);
            editor.Height(290);
            editor.MaxLength(1'000'000);
            ScrollViewer::SetHorizontalScrollBarVisibility(editor, ScrollBarVisibility::Auto);
            ScrollViewer::SetVerticalScrollBarVisibility(editor, ScrollBarVisibility::Auto);
            ContentDialog prompt;
            prompt.XamlRoot(root_.XamlRoot());
            prompt.PrimaryButtonText(tr(L"Valider"));
            prompt.CloseButtonText(tr(L"Annuler"));
            if (abc->strings().size() < 2)
            {
                status(tr(L"Le bloc ABC ne contient pas de constante chaîne modifiable."), true);
                co_return;
            }
            prompt.Title(box_value(tr(L"Modifier une constante ABC")));
            NumberBox id;
            id.Header(box_value(tr(L"Index de la constante")));
            id.Minimum(1);
            id.Maximum(static_cast<double>(abc->strings().size() - 1));
            id.Value(1);
            id.SpinButtonPlacementMode(NumberBoxSpinButtonPlacementMode::Compact);
            editor.Text(widen(abc->strings()[1]));
            Automation::AutomationProperties::SetName(editor, tr(L"Valeur de la constante"));
            id.ValueChanged([abc, editor](auto&&, const NumberBoxValueChangedEventArgs& args)
            {
                const auto value = args.NewValue();
                if (std::isfinite(value) && value >= 1 && value < static_cast<double>(abc->strings().size()))
                    editor.Text(widen(abc->strings()[static_cast<std::size_t>(value)]));
            });
            StackPanel content_panel;
            content_panel.Spacing(8);
            content_panel.Children().Append(id);
            auto error = label(L"", 12, danger);
            error.TextWrapping(TextWrapping::Wrap);
            content_panel.Children().Append(editor);
            content_panel.Children().Append(error);
            prompt.Content(content_panel);
            prompt.PrimaryButtonClick([&compiled, abc, editor, id, error](
                auto&&, const ContentDialogButtonClickEventArgs& args)
            {
                try
                {
                    const auto value = id.Value();
                    if (!std::isfinite(value) || value < 1 || value >= static_cast<double>(abc->strings().size()) ||
                        std::trunc(value) != value)
                        throw std::runtime_error("Sélectionnez un index de constante entier.");
                    compiled = abc->replace_string(static_cast<unsigned>(value), to_string(editor.Text()));
                }
                catch (const std::exception& problem)
                {
                    error.Text(localized_error(widen(problem.what())));
                    args.Cancel(true);
                }
            });
            if (co_await prompt.ShowAsync() != ContentDialogResult::Primary || closed_ || movie_ != movie)
                co_return;
        }
    }
    catch (const hresult_error& error) { status(error.message(), true); }
    catch (const std::exception& error) { status(widen(error.what()), true); }
    if (!compiled.size() && (operation != AbcEdit::import || !input))
        co_return;
    busy_ = true;
    open_.IsEnabled(false);
    save_.IsEnabled(false);
    status(tr(L"Validation du bloc ABC…"));
    const apartment_context ui;
    co_await resume_background();
    std::shared_ptr<flash::Movie> result;
    std::vector<Row> rows;
    std::wstring failure;
    try
    {
        if (operation == AbcEdit::import)
            compiled = flash::read_file(*input);
        result = std::make_shared<flash::Movie>(flash::replace_abc(*movie, tag, compiled));
        rows = index_rows(*result);
    }
    catch (const std::exception& error) { failure = widen(error.what()); }
    co_await ui;
    if (closed_)
        co_return;
    busy_ = false;
    open_.IsEnabled(true);
    save_.IsEnabled(true);
    if (!result || !failure.empty())
    {
        status(failure, true);
        co_return;
    }
    apply_document_edit(movie, std::move(result), std::move(rows), tag);
    if (script && mode_ == Mode::scripts)
        open_script_target({tag, {}, *script, operation == AbcEdit::import ? std::nullopt : method, {}, View::instructions});
    else if (mode_ == Mode::tags)
    {
        select_tag(tag);
        if (operation != AbcEdit::import)
            abc_method_ = method;
        refresh_tags();
        set_view(View::instructions);
    }
    status(tr(L"Modifications appliquées au SWF"));
}

bool Workspace::find_class_compiler(bool prompt)
{
    if (std::filesystem::is_regular_file(class_compiler_))
        return true;
    class_compiler_.clear();
    std::array<wchar_t, 32768> path{};
    const auto used = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (used && used < path.size())
    {
        const auto folder = std::filesystem::path(path.data()).parent_path();
        for (const auto& candidate : {folder / L"tools" / L"as3" / L"asc.jar", folder / L"asc.jar"})
            if (std::filesystem::is_regular_file(candidate))
            {
                class_compiler_ = candidate;
                return true;
            }
    }
    if (prompt)
    {
        status(tr(L"Sélectionnez asc.jar pour la compilation ActionScript."));
        if (const auto compiler = dialog(window_, false, L".jar"))
        {
            class_compiler_ = *compiler;
            return true;
        }
    }
    return false;
}

fire_and_forget Workspace::edit_tag()
{
    if (!movie_ || !selected_ || busy_)
        co_return;
    auto lifetime = shared_from_this();
    const auto movie = movie_;
    const auto index = *selected_;
    const auto code = movie->tags()[index].code;
    std::optional<std::filesystem::path> replacement;
    std::optional<std::string> text;
    try
    {
        if (code == 87 || code == 14 || flash::is_bitmap_tag(code))
        {
            const auto kind = code == 14 ? ResourceImport::audio :
                flash::is_bitmap_tag(code) ? ResourceImport::bitmap : ResourceImport::none;
            replacement = dialog(window_, false, L"", {}, nullptr, false, false, false, kind);
            if (!replacement)
                co_return;
        }
        else
        {
            TextBox editor;
            editor.AcceptsReturn(true);
            editor.TextWrapping(TextWrapping::Wrap);
            editor.MaxLength(500'000);
            editor.Height(230);
            editor.Text(widen(flash::extract_text(*movie, index)));
            Automation::AutomationProperties::SetName(editor, tr(L"Nouveau texte"));
            ContentDialog prompt;
            prompt.XamlRoot(root_.XamlRoot());
            prompt.Title(box_value(tr(L"Modifier le texte")));
            prompt.PrimaryButtonText(tr(L"Appliquer"));
            prompt.CloseButtonText(tr(L"Annuler"));
            prompt.Content(editor);
            if (co_await prompt.ShowAsync() != ContentDialogResult::Primary || closed_ || movie_ != movie)
                co_return;
            text = to_string(editor.Text());
            auto normalize_lines = [](std::string& value)
            {
                for (std::size_t i = 0; (i = value.find('\r', i)) != std::string::npos; ++i)
                    if (i + 1 < value.size() && value[i + 1] == '\n')
                        value.erase(i, 1);
                    else
                        value[i] = '\n';
            };
            auto original = flash::extract_text(*movie, index);
            normalize_lines(*text);
            normalize_lines(original);
            if (*text == original)
                co_return;
        }
    }
    catch (const hresult_error& error)
    {
        status(error.message(), true);
    }
    catch (const std::exception& error)
    {
        status(widen(error.what()), true);
    }
    if (!replacement && !text)
        co_return;
    busy_ = true;
    open_.IsEnabled(false);
    save_.IsEnabled(false);
    replace_.IsEnabled(false);
    status(tr(L"Modification du SWF…"));
    const apartment_context ui;
    co_await resume_background();
    std::shared_ptr<flash::Movie> updated;
    std::vector<Row> rows;
    std::wstring failure;
    try
    {
        updated = std::make_shared<flash::Movie>(text ? movie->replace_text(index, *text) :
            code == 14 ? movie->replace_sound(index, *replacement) :
            flash::is_bitmap_tag(code) ? movie->replace_image(index, flash::read_file(*replacement)) :
            movie->replace_binary(index, flash::read_file(*replacement)));
        rows = index_rows(*updated);
    }
    catch (const std::exception& error) { failure = widen(error.what()); }
    co_await ui;
    if (closed_)
        co_return;
    busy_ = false;
    open_.IsEnabled(true);
    save_.IsEnabled(true);
    replace_.IsEnabled(true);
    if (failure.empty())
    {
        apply_document_edit(movie, std::move(updated), std::move(rows), index);
        status(tr(L"Modifications appliquées au SWF"));
    }
    else
        status(failure, true);
}
}
