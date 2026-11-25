#include "Workspace.hpp"
#include "UiStyle.hpp"

#include <algorithm>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>

namespace studio
{
using namespace ui;

namespace
{
std::wstring path_key(const std::filesystem::path& path)
{
    return lower(path.lexically_normal().generic_wstring());
}

std::optional<std::filesystem::path> swf_reference(std::wstring_view text, bool& rooted)
{
    if (text.empty() || text.size() > 4096 || text.find(L'\0') != std::wstring_view::npos)
        return {};
    std::wstring address(text.substr(0, text.find_first_of(L"?#")));
    std::ranges::replace(address, L'\\', L'/');
    const auto scheme = lower(address.substr(0, address.find(L"://")));
    if (scheme == L"http" || scheme == L"https")
    {
        const auto slash = address.find(L'/', scheme.size() + 3);
        if (slash == std::wstring::npos)
            return {};
        address.erase(0, slash);
    }
    if (address.starts_with(L"//") || address.find(L':') != std::wstring::npos)
        return {};
    rooted = address.starts_with(L'/');
    if (rooted)
        address.erase(0, 1);
    const std::filesystem::path path(address);
    if (lower(path.extension().wstring()) != L".swf" || path.has_root_path())
        return {};
    return path;
}

bool within(const std::filesystem::path& path)
{
    return !path.empty() && !path.has_root_path() && *path.begin() != L"..";
}
}

struct Workspace::FolderIndex
{
    struct File { std::wstring relative, key; };
    std::filesystem::path root;
    std::vector<File> files;

    explicit FolderIndex(const std::filesystem::path& path) : root(std::filesystem::canonical(path))
    {
        std::error_code error;
        auto iterator = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, error);
        if (error)
            throw std::filesystem::filesystem_error("Index du dossier", root, error);
        for (const std::filesystem::recursive_directory_iterator end; iterator != end;)
        {
            const auto& file = iterator->path();
            if (lower(file.extension().wstring()) == L".swf" &&
                std::filesystem::is_regular_file(iterator->symlink_status(error)) && !error)
            {
                const auto relative = file.lexically_relative(root);
                files.push_back({relative.wstring(), path_key(relative)});
            }
            iterator.increment(error);
            if (error)
                throw std::filesystem::filesystem_error("Index du dossier", root, error);
        }
        std::ranges::sort(files, {}, &File::key);
    }

    std::filesystem::path exact(const std::filesystem::path& relative) const
    {
        const auto path = relative.lexically_normal();
        if (!within(path))
            return {};
        const auto key = path_key(path);
        const auto found = std::ranges::lower_bound(files, key, {}, &File::key);
        return found != files.end() && found->key == key ? root / found->relative : std::filesystem::path{};
    }

    std::filesystem::path resolve(const std::filesystem::path& source, const std::filesystem::path& reference, bool rooted) const
    {
        if (!rooted)
        {
            const auto parent = source.parent_path().lexically_relative(root);
            if (within(parent))
                if (auto found = exact(parent / reference); !found.empty())
                    return found;
        }
        if (auto found = exact(reference); !found.empty())
            return found;
        if (rooted || !within(reference.lexically_normal()))
            return {};
        const auto suffix = L"/" + path_key(reference);
        const File* match = nullptr;
        for (const auto& candidate : files)
            if (candidate.key.ends_with(suffix))
            {
                if (match)
                    return {};
                match = &candidate;
            }
        return match ? root / match->relative : std::filesystem::path{};
    }
};

void Workspace::build_folder_panel(const Grid& main)
{
    folder_pane_ = SplitView();
    folder_pane_.OpenPaneLength(288);
    folder_pane_.DisplayMode(SplitViewDisplayMode::Inline);
    folder_pane_.PaneBackground(brush(panel));
    folder_pane_.Content(main);

    Grid pane;
    pane.Padding(pad(12, 14, 12, 8));
    pane.RowSpacing(9);
    pane.BorderThickness(pad(0, 0, 1, 0));
    pane.BorderBrush(brush(stroke));
    row(pane);
    row(pane);
    row(pane);
    row(pane, 1, GridUnitType::Star);
    Grid heading;
    heading.ColumnSpacing(6);
    column(heading, 1, GridUnitType::Star);
    column(heading, 1, GridUnitType::Auto);
    column(heading, 1, GridUnitType::Auto);
    folder_title_ = label(tr(L"Espace de travail"), 15, primary, true);
    folder_title_.VerticalAlignment(VerticalAlignment::Center);
    place(heading, folder_title_, 0);
    Button choose;
    choose.Content(glyph(L'\uE838', 14));
    Automation::AutomationProperties::SetName(choose, tr(L"Ouvrir un dossier"));
    ToolTipService::SetToolTip(choose, box_value(tr(L"Ouvrir un dossier (Ctrl+Maj+O)")));
    choose.Click([this](auto&&, auto&&) { open_folder_dialog(); });
    place(heading, choose, 0, 1);
    Button close;
    close.Content(glyph(L'\uE711', 12));
    Automation::AutomationProperties::SetName(close, tr(L"Masquer l’espace de travail"));
    close.Click([this](auto&&, auto&&) { folder_pane_.IsPaneOpen(false); });
    place(heading, close, 0, 2);
    place(pane, heading, 0);

    folder_search_ = TextBox();
    folder_search_.PlaceholderText(tr(L"Filtrer les SWF…"));
    folder_search_.MaxLength(512);
    Automation::AutomationProperties::SetName(folder_search_, tr(L"Filtrer les SWF"));
    folder_search_.TextChanged([this](auto&&, auto&&) { filter_folder(); });
    folder_search_.KeyDown([this](auto&&, const Microsoft::UI::Xaml::Input::KeyRoutedEventArgs& event)
    {
        if (!folder_ || busy_ || event.Key() != Windows::System::VirtualKey::Enter || folder_list_.Items().Size() != 1)
            return;
        auto path = folder_->exact(unbox_value<hstring>(folder_list_.Items().GetAt(0)).c_str());
        if (path.empty())
            return;
        if (folder_pane_.DisplayMode() == SplitViewDisplayMode::Overlay)
            folder_pane_.IsPaneOpen(false);
        open_file(std::move(path));
        event.Handled(true);
    });
    place(pane, folder_search_, 1);
    folder_count_ = label(L"", 12, secondary);
    place(pane, folder_count_, 2);
    folder_list_ = ListView();
    folder_list_.IsItemClickEnabled(true);
    folder_list_.SelectionMode(ListViewSelectionMode::Single);
    Automation::AutomationProperties::SetName(folder_list_, tr(L"SWF de l’espace de travail"));
    folder_list_.ItemClick([this](auto&&, const ItemClickEventArgs& event)
    {
        if (!folder_ || busy_)
            return;
        if (auto path = folder_->exact(std::filesystem::path(unbox_value<hstring>(event.ClickedItem()).c_str())); !path.empty())
        {
            if (folder_pane_.DisplayMode() == SplitViewDisplayMode::Overlay)
                folder_pane_.IsPaneOpen(false);
            open_file(std::move(path));
        }
    });
    place(pane, folder_list_, 3);
    folder_pane_.Pane(pane);
    place(root_, folder_pane_, 2);
}

void Workspace::toggle_folder_panel()
{
    if (busy_)
        return;
    if (!folder_)
        open_folder_dialog();
    else
    {
        folder_pane_.IsPaneOpen(!folder_pane_.IsPaneOpen());
        if (folder_pane_.IsPaneOpen())
        {
            sync_folder_selection();
            folder_search_.Focus(FocusState::Keyboard);
        }
    }
}

void Workspace::filter_folder()
{
    if (!folder_)
        return;
    auto query = lower(folder_search_.Text().c_str());
    std::ranges::replace(query, L'\\', L'/');
    std::vector<Windows::Foundation::IInspectable> visible;
    folder_visible_.clear();
    for (std::size_t index = 0; index < folder_->files.size(); ++index)
    {
        const auto& file = folder_->files[index];
        if (query.empty() || file.key.find(query) != std::wstring::npos)
        {
            folder_visible_.push_back(index);
            visible.push_back(box_value(hstring(file.relative)));
        }
    }
    folder_count_.Text(format_message(L"{} sur {} SWF", visible.size(), folder_->files.size()));
    folder_list_.ItemsSource(single_threaded_observable_vector<Windows::Foundation::IInspectable>(std::move(visible)));
    sync_folder_selection(false);
}

void Workspace::sync_folder_selection(bool scroll)
{
    if (!folder_)
        return;
    int selected = -1;
    if (!file_.empty())
    {
        const auto relative = std::filesystem::path(path_key(file_)).lexically_relative(path_key(folder_->root));
        if (within(relative))
        {
            const auto key = path_key(relative);
            const auto file_key = [&](std::size_t index) -> const std::wstring& { return folder_->files[index].key; };
            const auto found = std::ranges::lower_bound(folder_visible_, key, {}, file_key);
            if (found != folder_visible_.end() && file_key(*found) == key)
                selected = static_cast<int>(found - folder_visible_.begin());
            else if (scroll && folder_pane_.IsPaneOpen() && !folder_search_.Text().empty() && !folder_->exact(relative).empty())
            {
                folder_search_.Text(L"");
                if (folder_list_.SelectedIndex() >= 0)
                    folder_list_.ScrollIntoView(folder_list_.SelectedItem());
                return;
            }
        }
    }
    folder_list_.SelectedIndex(selected);
    if (selected >= 0 && scroll && folder_pane_.IsPaneOpen())
        folder_list_.ScrollIntoView(folder_list_.SelectedItem());
}

Windows::Foundation::IAsyncAction Workspace::open_folder(std::filesystem::path path)
{
    if (busy_ || closed_)
        co_return;
    const auto lifetime = shared_from_this();
    busy_ = true;
    folder_button_.IsEnabled(false);
    status(tr(L"Indexation du dossier…"));
    const apartment_context ui;
    co_await resume_background();
    std::shared_ptr<const FolderIndex> indexed;
    try { indexed = std::make_shared<const FolderIndex>(path); }
    catch (const std::exception&) {}
    co_await ui;
    if (closed_)
        co_return;
    busy_ = false;
    folder_button_.IsEnabled(true);
    if (!indexed)
    {
        status(tr(L"Impossible de parcourir ce dossier."), true);
        co_return;
    }
    folder_ = std::move(indexed);
    const auto name = folder_->root.filename();
    folder_title_.Text(name.empty() ? folder_->root.wstring() : name.wstring());
    ToolTipService::SetToolTip(folder_title_, box_value(hstring(folder_->root.wstring())));
    if (folder_search_.Text().empty())
        filter_folder();
    else
        folder_search_.Text(L"");
    folder_pane_.IsPaneOpen(true);
    sync_folder_selection();
    folder_search_.Focus(FocusState::Programmatic);
    status(format_message(L"{} SWF dans l’espace de travail", folder_->files.size()));
}

void Workspace::follow_swf_reference(std::wstring_view reference)
{
    if (busy_ || !movie_)
        return;
    bool rooted = false;
    const auto path = swf_reference(reference, rooted);
    if (!path)
        return;
    if (folder_)
    {
        if (auto found = folder_->resolve(file_, *path, rooted); !found.empty())
        {
            open_file(std::move(found));
            return;
        }
    }
    else if (!rooted && within(path->lexically_normal()))
    {
        const auto target = file_.parent_path() / *path;
        std::error_code error;
        if (std::filesystem::is_regular_file(target, error))
        {
            open_file(target);
            return;
        }
    }
    status(format_message(L"Référence SWF introuvable ou ambiguë · {}", path->wstring()), true);
}
}
