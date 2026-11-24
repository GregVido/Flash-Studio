#include "Workspace.hpp"
#include "UiStyle.hpp"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <format>

namespace studio
{
using namespace ui;
using namespace winrt::Microsoft::UI::Xaml::Input;
using winrt::Microsoft::UI::Xaml::Documents::Run;

namespace
{
constexpr std::array areas{flash::SearchArea::all, flash::SearchArea::source, flash::SearchArea::instructions,
    flash::SearchArea::text, flash::SearchArea::constants, flash::SearchArea::tags};

struct SearchPanel
{
    explicit SearchPanel(std::shared_ptr<const flash::Movie> file) : movie(std::move(file)) {}

    flash::SearchOptions options() const
    {
        const auto index = scope.SelectedIndex();
        return {index >= 0 ? areas.at(static_cast<std::size_t>(index)) : flash::SearchArea::all,
            match_case.IsChecked().Value(), regex.IsChecked().Value()};
    }

    std::shared_ptr<const flash::Movie> movie;
    ContentDialog dialog;
    TextBox query;
    ComboBox scope;
    CheckBox match_case, regex;
    TextBlock summary;
    ListView results;
    ProgressRing progress;
    flash::SearchReport report;
    std::stop_source cancellation;
    std::uint64_t version = 0;
    bool visible = true;
};

void append_excerpt(TextBlock line, std::wstring_view text, bool highlighted)
{
    if (text.empty())
        return;
    Run run;
    run.Text(hstring(text));
    if (highlighted)
    {
        run.Foreground(brush(accent));
        run.FontWeight(Windows::UI::Text::FontWeight{600});
    }
    line.Inlines().Append(run);
}

ListViewItem result_row(const flash::SearchHit& hit)
{
    const auto location = hit.location + std::format(L"   ·   Tag {}", hit.tag);
    const auto detail = std::wstring(tr(flash::search_area_name(hit.area).data())) +
        (hit.area == flash::SearchArea::tags ? L"" : std::format(L"   ·   L. {}:{}", hit.line, hit.column));
    StackPanel rows;
    rows.Padding(pad(4, 6, 5, 6));
    rows.Spacing(3);
    rows.Children().Append(label(location + L"   ·   " + detail, 12, secondary));
    auto excerpt = label(L"", 13);
    excerpt.FontFamily(FontFamily(L"Consolas"));
    const auto text = std::wstring_view(hit.excerpt);
    append_excerpt(excerpt, text.substr(0, hit.highlight), false);
    append_excerpt(excerpt, text.substr(hit.highlight, hit.length), true);
    append_excerpt(excerpt, text.substr(hit.highlight + hit.length), false);
    rows.Children().Append(excerpt);
    ListViewItem item;
    item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    item.Content(rows);
    Automation::AutomationProperties::SetName(item, location + L" · " + detail + L" · " + hit.excerpt);
    return item;
}

fire_and_forget run_search(std::shared_ptr<SearchPanel> search_view)
{
    const auto query = std::wstring(search_view->query.Text());
    if (query.empty())
        co_return;
    const auto options = search_view->options();
    search_view->cancellation.request_stop();
    search_view->cancellation = std::stop_source();
    const auto token = search_view->cancellation.get_token();
    const auto version = ++search_view->version;
    search_view->results.Items().Clear();
    search_view->dialog.IsPrimaryButtonEnabled(false);
    search_view->summary.Text(tr(L"Recherche en cours…"));
    search_view->summary.Foreground(brush(secondary));
    search_view->progress.IsActive(true);
    search_view->progress.Visibility(Visibility::Visible);
    const apartment_context ui;
    try
    {
        co_await resume_background();
        flash::SearchReport report;
        std::wstring failure;
        try { report = flash::search_movie(*search_view->movie, query, options, token); }
        catch (const std::exception& error) { failure = widen(error.what()); }
        co_await ui;
        if (!search_view->visible || version != search_view->version || token.stop_requested())
            co_return;
        search_view->progress.IsActive(false);
        search_view->progress.Visibility(Visibility::Collapsed);
        if (!failure.empty())
        {
            search_view->summary.Text(localized_error(failure));
            search_view->summary.Foreground(brush(danger));
            co_return;
        }
        search_view->report = std::move(report);
        const auto count = search_view->report.hits.size();
        auto status = count ? format_message(L"{} résultat{}", count, count == 1 ? L"" : L"s") : tr(L"Aucun résultat");
        if (search_view->report.truncated)
            status += tr(L"  ·  limite atteinte");
        if (search_view->report.skipped)
            status += format_message(L"  ·  {} non analysés", search_view->report.skipped);
        search_view->summary.Text(status);
        for (const auto& hit : search_view->report.hits)
            search_view->results.Items().Append(result_row(hit));
        if (count)
            search_view->results.SelectedIndex(0);
    }
    catch (const hresult_error&) {}
}
}

fire_and_forget Workspace::show_global_search(std::optional<flash::SearchArea> area)
{
    if (!movie_ || global_search_open_ || busy_)
        co_return;
    auto lifetime = shared_from_this();
    auto search_view = std::make_shared<SearchPanel>(movie_);
    const auto weak = std::weak_ptr<SearchPanel>(search_view);
    global_search_open_ = true;
    try
    {
        search_view->dialog.XamlRoot(root_.XamlRoot());
        search_view->dialog.Title(box_value(tr(L"Rechercher dans le SWF")));
        search_view->dialog.PrimaryButtonText(tr(L"Ouvrir le résultat"));
        search_view->dialog.CloseButtonText(tr(L"Fermer"));
        search_view->dialog.IsPrimaryButtonEnabled(false);

        Grid content;
        content.Width(500);
        content.Height(412);
        content.RowSpacing(9);
        row(content);
        row(content);
        row(content);
        row(content, 1, GridUnitType::Star);

        Grid entry;
        entry.ColumnSpacing(7);
        column(entry, 1, GridUnitType::Star);
        column(entry, 1, GridUnitType::Auto);
        search_view->query.PlaceholderText(tr(L"Texte, instruction, classe ou constante…"));
        search_view->query.MaxLength(256);
        search_view->query.Text(global_search_term_);
        search_view->query.IsSpellCheckEnabled(false);
        Automation::AutomationProperties::SetName(search_view->query, tr(L"Motif de recherche"));
        search_view->query.KeyDown([weak](auto&&, const KeyRoutedEventArgs& args)
        {
            if (args.Key() == Windows::System::VirtualKey::Enter)
                if (auto live = weak.lock())
                {
                    args.Handled(true);
                    run_search(std::move(live));
                }
        });
        place(entry, search_view->query, 0);
        Button search;
        search.Content(box_value(tr(L"Rechercher")));
        search.Click([weak](auto&&, auto&&) { if (auto live = weak.lock()) run_search(std::move(live)); });
        place(entry, search, 0, 1);
        place(content, entry, 0);

        auto filters = horizontal(9);
        search_view->scope.Width(184);
        Automation::AutomationProperties::SetName(search_view->scope, tr(L"Champ de recherche"));
        for (const auto option : areas)
            search_view->scope.Items().Append(box_value(tr(flash::search_area_name(option).data())));
        search_view->scope.SelectedIndex(static_cast<int>(area.value_or(global_search_options_.area)));
        search_view->match_case.Content(box_value(tr(L"Respecter la casse")));
        search_view->match_case.IsChecked(global_search_options_.match_case);
        search_view->regex.Content(box_value(tr(L"Expression régulière")));
        search_view->regex.IsChecked(global_search_options_.regex);
        filters.Children().Append(search_view->scope);
        filters.Children().Append(search_view->match_case);
        filters.Children().Append(search_view->regex);
        place(content, filters, 1);

        auto feedback = horizontal(6);
        search_view->progress.Width(16);
        search_view->progress.Height(16);
        search_view->progress.Visibility(Visibility::Collapsed);
        search_view->summary = label(L"", 12, secondary);
        feedback.Children().Append(search_view->progress);
        feedback.Children().Append(search_view->summary);
        place(content, feedback, 2);

        search_view->results.SelectionMode(ListViewSelectionMode::Single);
        search_view->results.SelectionChanged([weak](auto&&, auto&&)
        {
            if (const auto live = weak.lock())
                live->dialog.IsPrimaryButtonEnabled(live->results.SelectedIndex() >= 0);
        });
        Automation::AutomationProperties::SetName(search_view->results, tr(L"Résultats de recherche"));
        place(content, search_view->results, 3);
        search_view->dialog.Content(content);
        if (!global_search_term_.empty())
            run_search(search_view);
        const auto result = co_await search_view->dialog.ShowAsync();
        search_view->visible = false;
        search_view->cancellation.request_stop();
        if (!closed_ && movie_ == search_view->movie)
        {
            global_search_term_ = search_view->query.Text();
            global_search_options_ = search_view->options();
            const auto selection = search_view->results.SelectedIndex();
            if (result == ContentDialogResult::Primary && selection >= 0 &&
                static_cast<std::size_t>(selection) < search_view->report.hits.size())
            {
                auto hit = std::move(search_view->report.hits[selection]);
                root_.DispatcherQueue().TryEnqueue([this, lifetime, file = search_view->movie, hit = std::move(hit)]
                {
                    if (!closed_ && movie_ == file)
                        open_search_hit(hit);
                });
            }
        }
    }
    catch (const hresult_error& error) { if (!closed_) status(error.message(), true); }
    search_view->visible = false;
    search_view->cancellation.request_stop();
    global_search_open_ = false;
}

void Workspace::open_search_hit(const flash::SearchHit& hit)
{
    if (!movie_ || hit.tag >= movie_->tags().size())
        return;
    if (hit.constant)
    {
        inspect_constant(hit.tag, *hit.constant);
        return;
    }
    const auto view = hit.area == flash::SearchArea::source ? View::source :
        hit.area == flash::SearchArea::instructions ? View::instructions :
        hit.area == flash::SearchArea::tags && movie_->tags()[hit.tag].code == 39 ? View::frame : View::details;
    if (hit.type)
    {
        open_script_target({hit.tag, hit.type, {}, hit.method, hit.line, view});
        return;
    }
    if (mode_ != Mode::tags)
        switch_mode(Mode::tags);
    {
        const UpdatingSelection guard(refreshing_);
        categories_.SelectedIndex(0);
        search_.Text(L"");
    }
    if (!hit.method && (view == View::source || view == View::instructions))
        next_code_line_ = hit.line;
    select_tag(hit.tag, view);
    refresh_tags();
    if (hit.method)
    {
        if (abc_)
            configure_abc(abc_, *hit.method);
        abc_method_ = hit.method;
        next_code_line_ = hit.line;
        show_selection();
    }
}

fire_and_forget Workspace::inspect_constant(std::size_t tag, unsigned index)
{
    auto lifetime = shared_from_this();
    const auto movie = movie_;
    const apartment_context ui;
    try
    {
        co_await resume_background();
        std::wstring value, failure;
        try
        {
            const flash::AbcModule module(flash::abc_bytecode(*movie, tag));
            value = widen(module.strings().at(index));
        }
        catch (const std::exception& error) { failure = widen(error.what()); }
        co_await ui;
        if (closed_ || movie_ != movie)
            co_return;
        if (!failure.empty())
        {
            status(failure, true);
            co_return;
        }
        TextBox text;
        text.IsReadOnly(true);
        text.AcceptsReturn(true);
        text.FontFamily(FontFamily(L"Consolas"));
        text.Height(220);
        text.Text(value);
        ContentDialog dialog;
        dialog.XamlRoot(root_.XamlRoot());
        dialog.Title(box_value(hstring(format_message(L"Constante ABC #{} · tag {}", index, tag))));
        dialog.Content(text);
        dialog.CloseButtonText(tr(L"Fermer"));
        co_await dialog.ShowAsync();
    }
    catch (const hresult_error& error)
    {
        if (GetCurrentThreadId() == ui_thread_ && !closed_)
            status(error.message(), true);
    }
}
}
