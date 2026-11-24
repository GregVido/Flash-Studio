#include "Workspace.hpp"
#include "UiStyle.hpp"

#include <Windows.h>
#include <shlobj.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <algorithm>

namespace studio
{
using namespace ui;

namespace
{
std::filesystem::path history_file()
{
    PWSTR root{};
    check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &root));
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(root, CoTaskMemFree);
    return std::filesystem::path(root) / L"Flash Studio" / L"recent.txt";
}

std::vector<std::filesystem::path> read_recent(const std::filesystem::path& file)
{
    std::vector<std::filesystem::path> paths;
    if (!std::filesystem::exists(file) || std::filesystem::file_size(file) > 1024 * 1024)
        return paths;
    const auto bytes = flash::read_file(file);
    auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (!text.empty() && paths.size() < 12)
    {
        const auto end = text.find('\n');
        const auto entry = text.substr(0, end);
        if (!entry.empty())
            paths.emplace_back(widen(entry));
        if (end == std::string_view::npos)
            break;
        text.remove_prefix(end + 1);
    }
    return paths;
}
}

void Workspace::build_documents()
{
    documents_bar_ = TabView();
    documents_bar_.IsAddTabButtonVisible(true);
    documents_bar_.TabWidthMode(TabViewWidthMode::SizeToContent);
    Automation::AutomationProperties::SetName(documents_bar_, tr(L"Documents SWF ouverts"));
    documents_bar_.AddTabButtonClick([this](auto&&, auto&&) { open_dialog(); });
    documents_bar_.SelectionChanged([this](auto&&, auto&&)
    {
        const auto index = documents_bar_.SelectedIndex();
        if (!refreshing_ && index >= 0)
            activate_document(static_cast<std::size_t>(index));
    });
    documents_bar_.TabCloseRequested([this](auto&&, const TabViewTabCloseRequestedEventArgs& args)
    {
        const auto found = std::ranges::find(documents_, args.Tab(), &OpenDocument::tab);
        if (found != documents_.end())
            close_document(static_cast<std::size_t>(found - documents_.begin()));
    });

    recent_ = DropDownButton();
    recent_.Padding(pad(10, 8, 10, 8));
    recent_.Content(glyph(L'\uE823', 16));
    Automation::AutomationProperties::SetName(recent_, tr(L"Fichiers récents"));
    ToolTipService::SetToolTip(recent_, box_value(tr(L"Fichiers récents")));
    MenuFlyout recent_menu;
    recent_menu.Opening([this](auto&&, auto&&) { show_recent(); });
    recent_.Flyout(recent_menu);
}

void Workspace::show_recent()
{
    const auto items = recent_.Flyout().as<MenuFlyout>().Items();
    items.Clear();
    try
    {
        for (const auto& path : read_recent(history_file()))
        {
            MenuFlyoutItem item;
            item.Text(path.filename().wstring());
            ToolTipService::SetToolTip(item, box_value(hstring(path.wstring())));
            item.Click([this, path](auto&&, auto&&) { open_file(path); });
            items.Append(item);
        }
    }
    catch (const std::exception&) {}
    if (!items.Size())
    {
        MenuFlyoutItem empty;
        empty.Text(tr(L"Aucun fichier récent"));
        empty.IsEnabled(false);
        items.Append(empty);
    }
}

void Workspace::remember_recent(const std::filesystem::path& path)
{
    try
    {
        const auto file = history_file();
        auto paths = read_recent(file);
        std::erase_if(paths, [&](const auto& item) { return CompareStringOrdinal(path.c_str(), -1, item.c_str(), -1, TRUE) == CSTR_EQUAL; });
        paths.insert(paths.begin(), path);
        if (paths.size() > 12)
            paths.resize(12);
        std::string text;
        for (const auto& item : paths)
            text += to_string(hstring(item.wstring())) + '\n';
        std::filesystem::create_directories(file.parent_path());
        flash::write_text(file, text);
    }
    catch (const std::exception&) {}
}

void Workspace::add_document(std::filesystem::path path, std::shared_ptr<const flash::Movie> movie, std::vector<Row> rows,
    std::optional<std::size_t> initial_tag)
{
    for (std::size_t i = 0; i < documents_.size(); ++i)
        if (active_document_ != i && CompareStringOrdinal(documents_[i].path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
        {
            auto& document = documents_[i];
            document = {std::move(path), std::move(movie), std::move(rows), document.tab,
                initial_tag ? Mode::tags : Mode::resources, View::details, initial_tag, {}, {}, std::move(document.script_tabs)};
            activate_document(i);
            return;
        }
    TabViewItem tab;
    tab.Header(box_value(hstring(path.filename().wstring())));
    const auto index = documents_.size();
    documents_.push_back({std::move(path), std::move(movie), std::move(rows), tab,
        initial_tag ? Mode::tags : Mode::resources, View::details, initial_tag, {}, {}, {}});
    {
        const UpdatingSelection updating(refreshing_);
        documents_bar_.TabItems().Append(tab);
    }
    activate_document(index);
}

void Workspace::activate_document(std::size_t index)
{
    if (index >= documents_.size() || closed_)
        return;
    {
        const UpdatingSelection updating(refreshing_);
        documents_bar_.SelectedIndex(static_cast<int>(busy_ ? active_document_.value_or(index) : index));
    }
    if (busy_ || active_document_ == index)
        return;
    if (active_document_)
    {
        remember_script_tab();
        auto& previous = documents_[*active_document_];
        previous.path = file_;
        previous.movie = movie_;
        previous.rows = std::move(indexed_rows_);
        previous.mode = mode_;
        previous.view = view_;
        previous.tag = selected_;
        previous.filter = (mode_ == Mode::scripts ? script_search_ : mode_ == Mode::resources ? gallery_search_ : search_).Text();
        previous.script.reset();
        if (selected_script_ && script_entries_[*selected_script_].type)
            previous.script = ScriptSelection{*selected_, script_entries_[*selected_script_].qualified, abc_method_};
    }

    auto& next = documents_[index];
    active_document_ = index;
    movie_ = next.movie;
    file_ = next.path;
    indexed_rows_ = std::move(next.rows);
    mode_ = next.mode;
    update_document();
    {
        const UpdatingSelection updating(refreshing_);
        (mode_ == Mode::scripts ? script_search_ : mode_ == Mode::resources ? gallery_search_ : search_).Text(next.filter);
    }
    if (mode_ == Mode::scripts && next.script)
    {
        const auto match = std::ranges::find_if(next.script_tabs, [&](const ScriptTab& tab)
        {
            return tab.script.tag == next.script->tag && tab.script.qualified == next.script->qualified;
        });
        if (match != next.script_tabs.end())
            activate_script_tab(*match);
        else
            open_script_target({next.script->tag, {}, next.script->qualified, next.script->method, {}, next.view});
    }
    else if (mode_ == Mode::tags)
    {
        if (next.tag)
            select_tag(next.tag, next.view == View::details ? std::nullopt : std::optional(next.view));
        refresh_tags();
    }
    else if (mode_ == Mode::resources)
    {
        if (next.tag)
            select_tag(next.tag, next.view);
        refresh_gallery();
    }
    save_.IsEnabled(true);
}

void Workspace::close_document(std::size_t index)
{
    if (busy_ || index >= documents_.size())
        return;
    const auto previous = active_document_;
    const bool current = previous == index;
    {
        const UpdatingSelection updating(refreshing_);
        documents_bar_.TabItems().RemoveAt(static_cast<unsigned>(index));
        documents_.erase(documents_.begin() + index);
        if (previous && !current)
        {
            active_document_ = *previous - (index < *previous);
            documents_bar_.SelectedIndex(static_cast<int>(*active_document_));
            return;
        }
    }
    active_document_.reset();
    if (!documents_.empty())
    {
        activate_document((std::min)(index, documents_.size() - 1));
        return;
    }
    ++preview_version_;
    close_frame_player();
    movie_.reset();
    indexed_rows_.clear();
    selected_.reset();
    abc_.reset();
    abc_method_.reset();
    file_.clear();
    reset_explorer();
    refresh_script_tabs();
    workarea_.Visibility(Visibility::Collapsed);
    empty_.Visibility(Visibility::Visible);
    window_.Title(L"Flash Studio");
    file_name_.Text(tr(L"Explorateur SWF"));
    file_info_.Text(tr(L"Ouvrez une animation Flash"));
    save_.IsEnabled(false);
    global_search_.IsEnabled(false);
    for (const auto& tab : mode_tabs_)
        tab.IsEnabled(false);
    status(tr(L"Aucun fichier ouvert"));
}
}
