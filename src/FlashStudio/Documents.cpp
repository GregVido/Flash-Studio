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
std::filesystem::path settings_file(std::wstring_view name)
{
    PWSTR root{};
    check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &root));
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> storage(root, CoTaskMemFree);
    return std::filesystem::path(root) / L"Flash Studio" / name;
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
        for (const auto& path : read_recent(settings_file(L"recent.txt")))
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
        const auto file = settings_file(L"recent.txt");
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
        if (CompareStringOrdinal(documents_[i].path.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL)
        {
            activate_document(i);
            return;
        }
    TabViewItem tab;
    tab.Header(box_value(hstring(path.filename().wstring())));
    const auto index = documents_.size();
    const auto compression = movie->header().compression;
    documents_.push_back({std::move(path), std::move(movie), std::move(rows), tab,
        initial_tag ? Mode::tags : Mode::resources, View::details, initial_tag, {}, {}, {}, {}, compression});
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
        if (scripts_indexed_)
            previous.catalog = ScriptCatalog{std::move(script_entries_), script_errors_};
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
    save_as_.IsEnabled(true);
}

void Workspace::update_tab_label(std::size_t index)
{
    const auto& document = documents_[index];
    const auto caption = document.path.filename().wstring() + (document.dirty ? L" *" : L"");
    document.tab.Header(box_value(hstring(caption)));
    ToolTipService::SetToolTip(document.tab, box_value(hstring(document.path.wstring())));
    if (active_document_ == index)
    {
        file_name_.Text(caption);
        window_.Title(L"Flash Studio — " + caption);
    }
}

bool Workspace::apply_document_edit(const std::shared_ptr<const flash::Movie>& previous,
    std::shared_ptr<const flash::Movie> edited, std::vector<Row> rows, std::optional<std::size_t> tag, bool scripts_changed)
{
    const auto found = std::ranges::find(documents_, previous, &OpenDocument::movie);
    if (found == documents_.end())
        return false;
    const auto index = static_cast<std::size_t>(found - documents_.begin());
    found->movie = std::move(edited);
    found->dirty = true;
    if (tag)
        found->tag = tag;
    found->catalog.reset();
    if (scripts_changed)
    {
        found->script.reset();
        found->script_tabs.clear();
    }
    update_tab_label(index);
    if (active_document_ == index)
    {
        const auto selected = tag ? tag : selected_;
        const auto view = view_;
        const auto filter = (mode_ == Mode::scripts ? script_search_ : mode_ == Mode::resources ? gallery_search_ : search_).Text();
        const auto script = !scripts_changed && selected_script_ && script_entries_[*selected_script_].type ?
            std::optional<ScriptSelection>{{*selected_, script_entries_[*selected_script_].qualified, abc_method_}} : std::nullopt;
        movie_ = found->movie;
        indexed_rows_ = std::move(rows);
        update_document();
        {
            const UpdatingSelection managing(refreshing_);
            (mode_ == Mode::scripts ? script_search_ : mode_ == Mode::resources ? gallery_search_ : search_).Text(filter);
        }
        if (mode_ == Mode::scripts && script)
            open_script_target({script->tag, {}, script->qualified, script->method, {}, view});
        else if (mode_ == Mode::tags)
        {
            select_tag(selected);
            refresh_tags();
        }
        else if (mode_ == Mode::resources)
        {
            select_tag(selected);
            refresh_gallery();
        }
    }
    else
        found->rows = std::move(rows);
    return true;
}

Windows::Foundation::IAsyncOperation<bool> Workspace::confirm_document_close(std::size_t index)
{
    if (!documents_[index].dirty)
        co_return true;
    ContentDialog prompt;
    prompt.XamlRoot(root_.XamlRoot());
    prompt.Title(box_value(tr(L"Enregistrer les modifications ?")));
    auto message = label(documents_[index].path.filename().wstring());
    message.TextWrapping(TextWrapping::Wrap);
    prompt.Content(message);
    prompt.PrimaryButtonText(tr(L"Enregistrer"));
    prompt.SecondaryButtonText(tr(L"Ne pas enregistrer"));
    prompt.CloseButtonText(tr(L"Annuler"));
    prompt.DefaultButton(ContentDialogButton::Primary);
    const auto answer = co_await prompt.ShowAsync();
    if (answer == ContentDialogResult::Primary)
        co_return co_await save_document(index, false);
    co_return answer == ContentDialogResult::Secondary;
}

Windows::Foundation::IAsyncOperation<bool> Workspace::confirm_editor_close()
{
    if (!has_source_draft() && !has_shape_draft())
        co_return true;
    ContentDialog prompt;
    prompt.XamlRoot(root_.XamlRoot());
    prompt.Title(box_value(tr(L"Un brouillon est encore ouvert")));
    auto message = label(tr(L"Appliquez le brouillon au SWF pour pouvoir enregistrer ses modifications."));
    message.TextWrapping(TextWrapping::Wrap);
    prompt.Content(message);
    prompt.PrimaryButtonText(tr(L"Ignorer le brouillon"));
    prompt.CloseButtonText(tr(L"Annuler"));
    prompt.DefaultButton(ContentDialogButton::Close);
    co_return co_await prompt.ShowAsync() == ContentDialogResult::Primary;
}

fire_and_forget Workspace::close_document(std::size_t index)
{
    if (busy_ || close_prompt_ || index >= documents_.size())
        co_return;
    const auto lifetime = shared_from_this();
    busy_ = true;
    bool confirmed = false;
    const auto owns_shape_editor = shape_editor_for(index);
    try
    {
        if (!owns_shape_editor || co_await confirm_editor_close())
            confirmed = co_await confirm_document_close(index);
    }
    catch (const hresult_error& error) { status(error.message(), true); }
    busy_ = false;
    if (!confirmed || closed_)
        co_return;
    if (owns_shape_editor)
        close_shape_editor();
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
            co_return;
        }
    }
    active_document_.reset();
    if (!documents_.empty())
    {
        activate_document((std::min)(index, documents_.size() - 1));
        co_return;
    }
    ++preview_version_;
    hide_code();
    close_frame_player();
    movie_.reset();
    indexed_rows_.clear();
    selected_.reset();
    abc_.reset();
    abc_method_.reset();
    file_.clear();
    sync_folder_selection(false);
    reset_explorer();
    refresh_script_tabs();
    workarea_.Visibility(Visibility::Collapsed);
    empty_.Visibility(Visibility::Visible);
    window_.Title(L"Flash Studio");
    file_name_.Text(tr(L"Explorateur SWF"));
    file_info_.Text(tr(L"Ouvrez une animation Flash"));
    save_.IsEnabled(false);
    save_as_.IsEnabled(false);
    global_search_.IsEnabled(false);
    for (const auto& tab : mode_tabs_)
        tab.IsEnabled(false);
    status(tr(L"Aucun fichier ouvert"));
}

fire_and_forget Workspace::request_quit()
{
    if (close_prompt_ || (busy_ && !source_session_) || source_save_in_progress())
        co_return;
    const auto lifetime = shared_from_this();
    close_prompt_ = true;
    const auto prior_busy = std::exchange(busy_, true);
    bool confirmed = false;
    try
    {
        confirmed = co_await confirm_editor_close();
        for (std::size_t index = 0; confirmed && index < documents_.size(); ++index)
            confirmed = co_await confirm_document_close(index);
    }
    catch (const hresult_error& error) { status(error.message(), true); }
    busy_ = prior_busy;
    close_prompt_ = false;
    if (confirmed && !closed_)
    {
        remember_session();
        close_confirmed_ = true;
        window_.Close();
    }
}

void Workspace::remember_session()
{
    using namespace Windows::Data::Json;
    try
    {
        JsonObject session;
        JsonArray opened;
        for (const auto& document : documents_)
            opened.Append(JsonValue::CreateStringValue(document.path.wstring()));
        session.SetNamedValue(L"files", opened);
        if (active_document_)
            session.SetNamedValue(L"active", JsonValue::CreateStringValue(documents_[*active_document_].path.wstring()));
        const auto file = settings_file(L"session.json");
        std::filesystem::create_directories(file.parent_path());
        flash::write_text(file, to_string(session.Stringify()));
    }
    catch (const hresult_error&) {}
    catch (const std::exception&) {}
}

fire_and_forget Workspace::restore_session()
{
    using namespace Windows::Data::Json;
    const auto lifetime = shared_from_this();
    try
    {
        const auto file = settings_file(L"session.json");
        if (!std::filesystem::is_regular_file(file) || std::filesystem::file_size(file) > 4 * 1024 * 1024)
            co_return;
        const auto bytes = flash::read_file(file);
        const auto session = JsonObject::Parse(widen({reinterpret_cast<const char*>(bytes.data()), bytes.size()}));
        const auto active = session.GetNamedString(L"active", L"");
        for (const auto& value : session.GetNamedArray(L"files"))
        {
            if (closed_ || close_prompt_)
                co_return;
            const std::filesystem::path path(value.GetString().c_str());
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error))
                co_await open_file(path, {}, false);
        }
        for (std::size_t i = 0; i < documents_.size(); ++i)
            if (CompareStringOrdinal(documents_[i].path.c_str(), -1, active.c_str(), -1, TRUE) == CSTR_EQUAL)
            {
                activate_document(i);
                break;
            }
    }
    catch (const hresult_error&) {}
    catch (const std::exception&) {}
}
}
