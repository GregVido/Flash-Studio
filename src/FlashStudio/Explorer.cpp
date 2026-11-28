#include "Workspace.hpp"
#include "UiStyle.hpp"
#include "../FlashCore/ClassEditing.hpp"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <algorithm>
#include <format>
#include <map>
#include <tuple>

namespace studio
{
using namespace ui;
using namespace winrt::Microsoft::UI::Xaml::Controls::Primitives;
using namespace winrt::Microsoft::UI::Xaml::Media::Imaging;
using namespace winrt::Windows::Storage::Streams;

namespace
{
constexpr unsigned code_accent = 0x83DCCC;
constexpr unsigned image_accent = 0x9FB5FF;
constexpr unsigned art_surface = 0x232C3B;
constexpr std::size_t assets_per_page = 24;

bool has_thumbnail(const flash::Tag& tag)
{
    return tag.category == flash::Category::shapes || tag.category == flash::Category::images ||
        tag.category == flash::Category::sprites || tag.code == 11 || tag.code == 33;
}

wchar_t resource_icon(flash::Category category)
{
    switch (category)
    {
    case flash::Category::images: return L'\uEB9F';
    case flash::Category::sounds: return L'\uE8D6';
    case flash::Category::sprites: return L'\uE819';
    case flash::Category::text: case flash::Category::fonts: return L'\uE8D2';
    case flash::Category::shapes: return L'\uE81E';
    default: return L'\uE7C3';
    }
}

std::wstring package_title(std::wstring_view package)
{
    return package.empty() ? tr(L"Package par défaut") : std::wstring(package);
}

TreeViewNode navigation_node(wchar_t icon, std::wstring_view title, unsigned tint)
{
    TreeViewNode node;
    auto visual = icon_label(icon, title, tint);
    Automation::AutomationProperties::SetName(visual, hstring(title));
    ToolTipService::SetToolTip(visual, box_value(hstring(title)));
    node.Content(visual);
    return node;
}
}

void Workspace::build_modes(const Grid& workspace)
{
    StackPanel tabs;
    tabs.Padding(pad(6, 10, 6, 6));
    tabs.Spacing(6);
    tabs.Background(brush(base));
    const wchar_t* names[]{tr(L"Ressources"), L"Scripts", L"Tags"};
    static constexpr wchar_t icons[]{L'\uE8B7', L'\uE943', L'\uE8A9'};
    for (std::size_t i = 0; i < mode_tabs_.size(); ++i)
    {
        auto& tab = mode_tabs_[i];
        tab.Width(44);
        tab.Height(44);
        tab.Padding(pad(0));
        tab.CornerRadius({8, 8, 8, 8});
        tab.Content(glyph(icons[i], 19));
        Automation::AutomationProperties::SetName(tab, names[i]);
        ToolTipService::SetToolTip(tab, box_value(names[i]));
        tab.IsEnabled(false);
        tab.Click([this, i](auto&&, auto&&)
        {
            if (mode_ == static_cast<Mode>(i))
                mode_tabs_[i].IsChecked(true);
            else
                switch_mode(static_cast<Mode>(i));
        });
        tabs.Children().Append(tab);
    }
    place(workspace, tabs, 0);
}

void Workspace::build_script_browser(const Grid& sidebar)
{
    script_navigation_ = Grid();
    script_navigation_.RowSpacing(9);
    row(script_navigation_);
    row(script_navigation_);
    row(script_navigation_);
    row(script_navigation_);
    row(script_navigation_, 1, GridUnitType::Star);
    script_search_ = TextBox();
    script_search_.PlaceholderText(tr(L"Rechercher une classe…"));
    script_search_.MaxLength(256);
    Automation::AutomationProperties::SetName(script_search_, tr(L"Rechercher une classe"));
    script_search_.TextChanged([this](auto&&, auto&&) { if (!refreshing_ && scripts_indexed_) refresh_scripts(); });
    place(script_navigation_, script_search_, 0);
    script_status_ = label(L"", 12, secondary);
    script_status_.Margin(pad(4, 0, 4, 0));
    place(script_navigation_, script_status_, 1);
    document_class_ = Button();
    document_class_.HorizontalAlignment(HorizontalAlignment::Stretch);
    document_class_.Content(icon_label(L'\uE8A5', tr(L"Classe du document")));
    document_class_.Visibility(Visibility::Collapsed);
    document_class_.Click([this](auto&&, auto&&)
    {
        if (movie_ && movie_->document_class())
            open_script_target({{}, {}, widen(*movie_->document_class()), {}, {}, View::source});
    });
    place(script_navigation_, document_class_, 2);
    source_create_ = Button();
    source_create_.HorizontalAlignment(HorizontalAlignment::Stretch);
    source_create_.Content(icon_label(L'\uE710', tr(L"Nouvelle classe")));
    source_create_.Click([this](auto&&, auto&&) { create_class(); });
    place(script_navigation_, source_create_, 3);
    scripts_ = TreeView();
    scripts_.SelectionMode(TreeViewSelectionMode::Single);
    scripts_.ItemTemplate(Markup::XamlReader::Load(LR"(
        <DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
            <ContentPresenter Content="{Binding Content}" HorizontalAlignment="Stretch" />
        </DataTemplate>)").as<DataTemplate>());
    Automation::AutomationProperties::SetName(scripts_, tr(L"Packages et classes ActionScript"));
    scripts_.Expanding([this](auto&&, const TreeViewExpandingEventArgs& args)
    {
        const auto node = args.Node();
        if (node.HasUnrealizedChildren())
        {
            const auto group = unbox_value<std::int64_t>(node.Content().as<FrameworkElement>().Tag());
            fill_package(node, static_cast<std::size_t>(group));
        }
    });
    scripts_.ItemInvoked([this](auto&&, const TreeViewItemInvokedEventArgs& args)
    {
        const auto node = args.InvokedItem().try_as<TreeViewNode>();
        if (node && (node.HasChildren() || node.HasUnrealizedChildren()))
        {
            node.IsExpanded(!node.IsExpanded());
            return;
        }
        const auto visual = (node ? node.Content() : args.InvokedItem()).try_as<FrameworkElement>();
        if (visual && visual.Tag())
        {
            const auto index = unbox_value_or<std::uint64_t>(visual.Tag(), UINT64_MAX);
            if (index < script_entries_.size())
                select_script(static_cast<std::size_t>(index));
        }
    });
    place(script_navigation_, scripts_, 4);
    place(sidebar, script_navigation_, 1);
}

void Workspace::build_outline(const Grid& preview)
{
    column(preview, 1, GridUnitType::Star);
    outline_column_ = column(preview, 0);
    outline_ = Grid();
    outline_.Background(brush(surface));
    outline_.Margin(pad(0, 12, 12, 12));
    outline_.CornerRadius({7, 7, 7, 7});
    row(outline_);
    row(outline_);
    row(outline_, 1, GridUnitType::Star);
    auto heading = horizontal(8);
    heading.Margin(pad(11, 12, 7, 8));
    heading.Children().Append(glyph(L'\uE8A9', 13, code_accent));
    outline_count_ = label(tr(L"Membres"), 13, primary, true);
    heading.Children().Append(outline_count_);
    place(outline_, heading, 0);
    place(outline_, divider(), 1);
    members_ = ListView();
    members_.SelectionMode(ListViewSelectionMode::Single);
    members_.Padding(pad(4));
    Automation::AutomationProperties::SetName(members_, tr(L"Membres de la classe"));
    members_.SelectionChanged([this](auto&&, auto&&)
    {
        if (refreshing_ || !selected_script_ || !abc_)
            return;
        const auto type = script_entries_[*selected_script_].type;
        if (!type)
            return;
        const auto& list = abc_->classes()[*type].members;
        const auto index = members_.SelectedIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= list.size())
            return;
        abc_method_ = list[index].method;
        update_abc_visibility();
        if (view_ == View::source)
            jump_to_member();
        else if (view_ == View::instructions)
            show_selection();
    });
    place(outline_, members_, 2);
    place(preview, outline_, 0, 1);
}

void Workspace::build_gallery()
{
    gallery_ = Grid();
    gallery_.Background(brush(base));
    gallery_.Padding(pad(17, 16, 17, 9));
    gallery_.RowSpacing(11);
    row(gallery_);
    row(gallery_);
    row(gallery_, 1, GridUnitType::Star);
    row(gallery_);

    Grid title;
    column(title, 1, GridUnitType::Star);
    StackPanel titles;
    titles.Spacing(3);
    gallery_title_ = label(tr(L"Ressources"), 22, primary, true);
    gallery_count_ = label(L"", 12, secondary);
    titles.Children().Append(gallery_title_);
    titles.Children().Append(gallery_count_);
    place(title, titles, 0);
    gallery_back_ = Button();
    gallery_back_.Content(glyph(L'\uE72B', 16));
    gallery_back_.VerticalAlignment(VerticalAlignment::Center);
    Automation::AutomationProperties::SetName(gallery_back_, tr(L"Retour à la galerie"));
    ToolTipService::SetToolTip(gallery_back_, box_value(tr(L"Retour à la galerie")));
    gallery_back_.Click([this](auto&&, auto&&)
    {
        const UpdatingSelection updating(refreshing_);
        tiles_.SelectedIndex(-1);
        select_tag({});
    });
    place(gallery_, title, 0);

    gallery_search_ = TextBox();
    gallery_search_.PlaceholderText(tr(L"Nom, type ou identifiant de ressource…"));
    gallery_search_.MaxLength(256);
    gallery_search_.TextChanged([this](auto&&, auto&&) { if (!refreshing_) refresh_gallery(); });
    Automation::AutomationProperties::SetName(gallery_search_, tr(L"Rechercher une ressource"));
    place(gallery_, gallery_search_, 1);

    tiles_ = GridView();
    tiles_.SelectionMode(ListViewSelectionMode::Single);
    tiles_.SelectionChanged([this](auto&&, auto&&)
    {
        if (refreshing_ || mode_ != Mode::resources)
            return;
        const auto tile = tiles_.SelectedItem().try_as<GridViewItem>();
        if (!tile)
            return;
        const auto index = static_cast<std::size_t>(unbox_value<std::uint64_t>(tile.Tag()));
        select_tag(index, movie_->tags()[index].category == flash::Category::sprites ? View::frame : View::details);
    });
    Automation::AutomationProperties::SetName(tiles_, tr(L"Galerie des ressources"));
    place(gallery_, tiles_, 2);
    auto navigation = horizontal(8);
    gallery_previous_ = Button();
    gallery_previous_.Content(box_value(tr(L"Précédent")));
    gallery_previous_.Click([this](auto&&, auto&&) { if (gallery_page_) { --gallery_page_; refresh_gallery(true); } });
    gallery_next_ = Button();
    gallery_next_.Content(box_value(tr(L"Suivant")));
    gallery_next_.Click([this](auto&&, auto&&) { ++gallery_page_; refresh_gallery(true); });
    gallery_page_label_ = label(L"", 12, secondary);
    gallery_page_label_.VerticalAlignment(VerticalAlignment::Center);
    navigation.Children().Append(gallery_previous_);
    navigation.Children().Append(gallery_next_);
    navigation.Children().Append(gallery_page_label_);
    place(gallery_, navigation, 3);
}

void Workspace::update_layout()
{
    if (!outline_ || !gallery_ || !listing_)
        return;
    const bool script_mode = mode_ == Mode::scripts;
    const bool gallery_mode = mode_ == Mode::resources;
    const bool class_open = script_mode && selected_script_ && script_entries_[*selected_script_].type;
    const auto outline_checked = toggle_outline_.IsChecked();
    const bool outline_open = class_open && outline_checked && outline_checked.Value();
    const bool gallery_selection = gallery_mode && selected_.has_value();
    const auto visibility = [](bool visible) { return visible ? Visibility::Visible : Visibility::Collapsed; };
    listing_.Visibility(visibility(mode_ == Mode::tags));
    gallery_.Visibility(visibility(gallery_mode && !gallery_selection));
    gallery_back_.Visibility(visibility(gallery_selection));
    categories_.Visibility(visibility(!script_mode));
    script_navigation_.Visibility(visibility(script_mode));
    source_create_.IsEnabled(movie_ && flash::supports_as3_class_creation(*movie_) && !source_session_);
    outline_.Visibility(visibility(outline_open));
    toggle_outline_.Visibility(visibility(class_open));
    outline_column_.Width({outline_open ? (root_.ActualWidth() < 990 ? 173.0 : 233.0) : 0.0, GridUnitType::Pixel});
    inspector_.Visibility(visibility(!gallery_mode || gallery_selection));
    Grid::SetColumn(inspector_, mode_ == Mode::tags ? 2 : 1);
    Grid::SetColumnSpan(inspector_, mode_ == Mode::tags ? 1 : 2);
    Grid::SetColumnSpan(gallery_, 2);
}

void Workspace::switch_mode(Mode mode)
{
    if (source_session_)
        return;
    if (thumbnails_cancel_)
        thumbnails_cancel_->store(true, std::memory_order_relaxed);
    if (mode != mode_ && mode_ == Mode::scripts)
    {
        remember_script_tab();
        previous_script_ = selected_script_;
        previous_method_ = abc_method_;
        previous_script_view_ = view_;
    }
    if (mode != mode_ && mode_ == Mode::resources)
    {
        previous_asset_ = selected_;
        previous_category_ = category_filter();
    }
    mode_ = mode;
    if (mode != Mode::scripts)
        pending_script_target_.reset();
    selected_script_.reset();
    member_ranges_.clear();
    for (std::size_t i = 0; i < mode_tabs_.size(); ++i)
    {
        mode_tabs_[i].IsEnabled(static_cast<bool>(movie_));
        const auto checked = i == static_cast<std::size_t>(mode);
        mode_tabs_[i].IsChecked(checked);
        mode_tabs_[i].BorderBrush(brush(checked ? accent : base));
        mode_tabs_[i].Background(brush(checked ? selected_surface : base));
    }
    sidebar_title_.Text(mode == Mode::scripts ? tr(L"Explorateur de scripts") : mode == Mode::tags ? tr(L"Catégories") : tr(L"Bibliothèque"));
    select_tag({});
    if (movie_ && mode != Mode::scripts)
        populate_categories();
    if (mode == Mode::tags)
        refresh_tags();
    else if (mode == Mode::resources)
    {
        if (previous_asset_)
        {
            const auto tag = *previous_asset_;
            select_tag(tag, movie_->tags()[tag].category == flash::Category::sprites ? View::frame : View::details);
        }
        refresh_gallery(true);
    }
    else if (scripts_indexed_)
    {
        if (previous_script_)
            select_script(*previous_script_, previous_method_, previous_script_view_);
        refresh_scripts();
    }
    else if (!scripts_loading_)
        index_scripts();
    refresh_script_tabs();
}

void Workspace::reset_explorer()
{
    const UpdatingSelection updating(refreshing_);
    if (thumbnails_cancel_)
        thumbnails_cancel_->store(true, std::memory_order_relaxed);
    selected_script_.reset();
    previous_script_.reset();
    previous_asset_.reset();
    previous_method_.reset();
    previous_category_.reset();
    edited_script_.reset();
    pending_script_target_.reset();
    next_code_line_.reset();
    global_search_term_.clear();
    script_entries_.clear();
    scripts_.RootNodes().Clear();
    packages_.clear();
    members_.Items().Clear();
    member_ranges_.clear();
    script_search_.Text(L"");
    gallery_search_.Text(L"");
    visible_assets_.clear();
    gallery_page_ = 0;
    script_errors_ = 0;
    scripts_indexed_ = false;
    scripts_loading_ = false;
}

fire_and_forget Workspace::index_scripts()
{
    if (!movie_)
        co_return;
    auto lifetime = shared_from_this();
    const auto movie = movie_;
    scripts_loading_ = true;
    script_status_.Text(tr(L"Indexation des classes…"));
    const apartment_context ui;
    try
    {
        co_await resume_background();
        std::vector<ScriptEntry> entries;
        std::size_t errors = 0;
        const auto add = [&](std::size_t tag, std::optional<std::size_t> type, std::shared_ptr<const flash::AbcModule> module,
            std::wstring package, std::wstring name, std::wstring qualified)
        {
            auto search = lower(qualified);
            entries.push_back({tag, type, std::move(module), std::move(package), std::move(name), std::move(qualified), std::move(search)});
        };
        for (std::size_t tag = 0; tag < movie->tags().size(); ++tag)
        {
            const auto code = movie->tags()[tag].code;
            if (code == 72 || code == 82)
            {
                try
                {
                    const auto module = std::make_shared<const flash::AbcModule>(flash::abc_bytecode(*movie, tag));
                    const auto& types = module->classes();
                    for (std::size_t i = 0; i < types.size(); ++i)
                        add(tag, i, module, widen(types[i].package), widen(types[i].name), widen(types[i].qualified));
                    if (types.empty() || std::ranges::any_of(module->methods(), [](const flash::AbcMethod& method) { return method.owner.empty(); }))
                    {
                        const auto name = format_message(L"Bloc ABC #{}", tag);
                        add(tag, {}, {}, tr(L"Scripts et méthodes libres"), name, name);
                    }
                }
                catch (const std::runtime_error&)
                {
                    ++errors;
                    const auto name = format_message(L"Bloc ABC #{}", tag);
                    add(tag, {}, {}, tr(L"Blocs ABC illisibles"), name, name);
                }
            }
            else if (flash::has_avm1_actions(*movie, tag))
            {
                auto name = widen(flash::tag_name(code)) + std::format(L" #{}", tag);
                add(tag, {}, {}, L"ActionScript 1/2", name, name);
            }
        }
        std::ranges::sort(entries, [](const ScriptEntry& left, const ScriptEntry& right)
        {
            return std::tie(left.package, left.name, left.tag) < std::tie(right.package, right.name, right.tag);
        });
        co_await ui;
        if (closed_)
            co_return;
        if (movie != movie_)
        {
            const auto document = std::ranges::find(documents_, movie, &OpenDocument::movie);
            if (document != documents_.end())
                document->catalog = ScriptCatalog{std::move(entries), errors};
            co_return;
        }
        script_entries_ = std::move(entries);
        script_errors_ = errors;
        scripts_loading_ = false;
        scripts_indexed_ = true;
        refresh_script_tabs();
        if (mode_ == Mode::scripts)
        {
            if (edited_script_)
            {
                const auto selection = std::exchange(edited_script_, {});
                const auto match = std::ranges::find_if(script_entries_, [&](const ScriptEntry& entry)
                {
                    return entry.tag == selection->tag && entry.qualified == selection->qualified;
                });
                if (match != script_entries_.end())
                    select_script(static_cast<std::size_t>(match - script_entries_.begin()), selection->method,
                        selection->method && match->type ? View::instructions : View::source);
            }
            if (pending_script_target_)
                resolve_script_target();
            else
                refresh_scripts();
        }
    }
    catch (const hresult_error&) {}
    catch (const std::exception&)
    {
        if (GetCurrentThreadId() == ui_thread_ && !closed_ && movie == movie_)
        {
            scripts_loading_ = false;
            script_status_.Text(tr(L"Index des scripts indisponible"));
        }
    }
}

void Workspace::refresh_scripts()
{
    scripts_.RootNodes().Clear();
    packages_.clear();
    packages_.emplace_back();
    const auto query = lower(script_search_.Text().c_str());
    std::size_t count = 0, groups = 0;
    for (std::size_t i = 0; i < script_entries_.size(); ++i)
    {
        const auto& entry = script_entries_[i];
        if (entry.search_key.find(query) == std::wstring::npos && lower(entry.package).find(query) == std::wstring::npos)
            continue;
        std::size_t parent = 0;
        auto remainder = std::wstring_view(entry.package);
        for (unsigned level = 0; ; ++level)
        {
            const auto separator = entry.type && level < 31 ? remainder.find(L'.') : std::wstring_view::npos;
            auto segment = std::wstring(remainder.substr(0, separator));
            const auto [it, inserted] = packages_[parent].children.try_emplace(segment, packages_.size());
            parent = it->second;
            if (inserted)
                packages_.push_back(Package{segment.empty() ? package_title(segment) : std::move(segment), {}, {}});
            packages_[parent].contains_selection |= selected_script_ == i;
            if (separator == std::wstring_view::npos)
                break;
            remainder.remove_prefix(separator + 1);
        }
        groups += packages_[parent].entries.empty();
        packages_[parent].entries.push_back(i);
        count += entry.type.has_value();
    }
    const auto summary = std::format(L"{} {} · {} {}", count, count == 1 ? tr(L"classe") : tr(L"classes"),
        groups, groups == 1 ? tr(L"groupe") : tr(L"groupes"));
    script_status_.Text(!groups ? tr(L"Aucun script correspondant") : script_errors_ ?
        summary + format_message(L" · {} illisibles", script_errors_) : summary);
    expand_filtered_packages_ = !query.empty() && groups <= 8;
    for (const auto& [caption, index] : packages_.front().children)
    {
        const auto node = package_node(index);
        scripts_.RootNodes().Append(node);
        if (expand_filtered_packages_ || packages_[index].contains_selection)
        {
            fill_package(node, index);
            node.IsExpanded(true);
        }
    }
}

TreeViewNode Workspace::package_node(std::size_t index)
{
    auto node = navigation_node(L'\uE8B7', packages_[index].caption, image_accent);
    node.Content().as<FrameworkElement>().Tag(box_value(static_cast<std::int64_t>(index)));
    node.HasUnrealizedChildren(true);
    return node;
}

void Workspace::fill_package(const TreeViewNode& node, std::size_t index)
{
    node.HasUnrealizedChildren(false);
    const auto& package = packages_.at(index);
    for (const auto& [caption, child] : package.children)
    {
        const auto folder = package_node(child);
        node.Children().Append(folder);
        if (expand_filtered_packages_ || packages_[child].contains_selection)
        {
            fill_package(folder, child);
            folder.IsExpanded(true);
        }
    }
    for (const auto entry_index : package.entries)
    {
        const auto& entry = script_entries_[entry_index];
        auto item = navigation_node(entry.type ? L'\uE943' : L'\uE8A5', entry.name, entry.type ? code_accent : primary);
        item.Content().as<FrameworkElement>().Tag(box_value(static_cast<std::uint64_t>(entry_index)));
        node.Children().Append(item);
        if (entry_index == selected_script_)
            scripts_.SelectedNode(item);
    }
}

void Workspace::select_script(std::size_t index, std::optional<unsigned> method, View view)
{
    if (source_session_ || mode_ != Mode::scripts || index >= script_entries_.size())
        return;
    remember_script_tab();
    const auto& entry = script_entries_[index];
    if (method && entry.module && *method >= entry.module->methods().size())
        method.reset();
    selected_script_ = index;
    member_ranges_.clear();
    {
        const UpdatingSelection updating(refreshing_);
        members_.Items().Clear();
        if (entry.type)
        {
            const auto& type = entry.module->classes()[*entry.type];
            outline_count_.Text(format_message(L"Membres · {}", type.members.size()));
            for (std::size_t i = 0; i < type.members.size(); ++i)
            {
                const auto& member = type.members[i];
                ListViewItem item;
                item.Padding(pad(5, 0, 3, 0));
                item.MinHeight(33);
                const auto title = member.declaration.empty() && member.name == "Initialiseur statique" ?
                    std::wstring(tr(L"Initialiseur statique")) : widen(member.name);
                const auto signature = member.declaration.empty() ? title : widen(member.declaration);
                item.Content(icon_label(member.method ? L'\uE943' : L'\uE8B7', title, member.method ? code_accent : image_accent, 12));
                Automation::AutomationProperties::SetName(item, signature);
                ToolTipService::SetToolTip(item, box_value(hstring(signature)));
                members_.Items().Append(item);
                if (method && member.method == method)
                    members_.SelectedIndex(static_cast<int>(i));
            }
        }
    }
    if (selected_ != entry.tag)
    {
        abc_.reset();
        abc_groups_.clear();
    }
    selected_ = entry.tag;
    abc_ = entry.module;
    abc_method_ = method;
    select_tag(entry.tag, view);
    track_script_tab(entry);
}

void Workspace::jump_to_member()
{
    const auto index = members_.SelectedIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= member_ranges_.size())
        return;
    jump_to_code_line(member_ranges_[index].first_line);
}

void Workspace::select_source_member(std::size_t index)
{
    if (busy_ || view_ != View::source || !selected_script_ || !abc_ || index >= member_ranges_.size())
        return;
    const auto type = script_entries_[*selected_script_].type;
    if (!type)
        return;
    const auto& members = abc_->classes()[*type].members;
    if (index >= members.size() || !members[index].method || members_.SelectedIndex() == static_cast<int>(index))
        return;
    const UpdatingSelection updating(refreshing_);
    members_.SelectedIndex(static_cast<int>(index));
    abc_method_ = members[index].method;
    if (outline_.Visibility() == Visibility::Visible)
        members_.ScrollIntoView(members_.SelectedItem());
    update_abc_visibility();
    remember_script_tab();
}

CodeReference Workspace::code_references(std::wstring_view source)
{
    if (!selected_script_ || !script_entries_[*selected_script_].type)
        return {};
    const auto current = *selected_script_;
    const auto& entry = script_entries_[current];
    const auto& info = entry.module->classes()[*entry.type];
    std::map<std::wstring, std::optional<std::size_t>, std::less<>> classes;
    const auto add_name = [&](std::wstring_view key, std::size_t index)
    {
        const auto [item, inserted] = classes.emplace(key, index);
        if (!inserted && item->second != index)
            item->second.reset();
    };
    for (std::size_t i = 0; i < script_entries_.size(); ++i)
    {
        const auto& type = script_entries_[i];
        if (!type.type)
            continue;
        add_name(type.qualified, i);
        if (!entry.package.empty() && type.package == entry.package)
            add_name(type.name, i);
    }
    for (auto start = source.find(L"\n    import "); start != std::wstring_view::npos; start = source.find(L"\n    import ", start + 1))
    {
        const auto first = start + std::wstring_view(L"\n    import ").size();
        const auto last = source.find_first_of(L";\n", first);
        if (last == std::wstring_view::npos || source[last] != L';')
            continue;
        const auto qualified = source.substr(first, last - first);
        if (const auto found = classes.find(qualified); found != classes.end() && found->second)
            classes.insert_or_assign(script_entries_[*found->second].name, found->second);
    }
    std::optional<std::size_t> base_class;
    if (const auto marker = info.declaration.find(" extends "); marker != std::string::npos)
    {
        const auto parent = info.declaration.substr(marker + 9);
        if (const auto found = classes.find(widen(parent.substr(0, parent.find(' ')))); found != classes.end())
            base_class = found->second;
    }
    const auto weak = weak_from_this();
    return [this, current, base_class, weak, classes = std::move(classes)](std::wstring_view receiver, std::wstring_view name,
        CodeReferenceKind kind) -> std::function<void()>
    {
        const auto open = [&](std::size_t index, std::optional<unsigned> method = {})
        {
            const auto& script = script_entries_[index];
            return [weak, destination = ScriptTarget{script.tag, script.type, script.qualified, method, {}, View::source}]
            {
                if (const auto live = weak.lock(); live && !live->closed_ && !live->busy_)
                    live->open_script_target(destination);
            };
        };
        if (kind == CodeReferenceKind::type)
        {
            constexpr std::wstring_view primitives[]{L"int", L"uint", L"Number", L"String", L"Boolean",
                L"Object", L"void", L"float", L"float4"};
            if (std::ranges::find(primitives, name) != std::end(primitives))
                return {};
            const auto type = classes.find(name);
            if (type == classes.end() || !type->second)
                return {};
            return open(*type->second);
        }
        const bool constructor = kind == CodeReferenceKind::constructor;
        const bool local = receiver.empty() || receiver == L"this";
        auto target = current;
        if (constructor || !local)
        {
            if (receiver == L"super" && !constructor)
            {
                if (!base_class)
                    return {};
                target = *base_class;
            }
            else
            {
                const auto owner = constructor ? (receiver.empty() ? std::wstring(name) : std::wstring(receiver) + L'.' + std::wstring(name)) :
                    std::wstring(receiver);
                const auto found = classes.find(owner);
                if (found == classes.end() || !found->second)
                    return {};
                target = *found->second;
            }
        }
        const auto locate = [&](std::size_t index) -> std::optional<unsigned>
        {
            const auto& script = script_entries_[index];
            const auto& type = script.module->classes()[*script.type];
            std::optional<unsigned> method;
            for (const auto& member : type.members)
                if (member.method && widen(member.name) == name &&
                    (constructor || local || receiver == L"super" || member.declaration.find("static ") != std::string::npos))
                {
                    if (method)
                        return {};
                    method = member.method;
                }
            return method;
        };
        auto method = locate(target);
        if (!method && local && !constructor && base_class && *base_class != target)
        {
            target = *base_class;
            method = locate(target);
        }
        if (!method && !constructor)
            return {};
        return open(target, method);
    };
}

void Workspace::open_script_target(ScriptTarget target)
{
    if (source_session_)
        return;
    if (mode_ != Mode::scripts)
        switch_mode(Mode::scripts);
    pending_script_target_ = std::move(target);
    if (scripts_indexed_)
        resolve_script_target();
    else if (!scripts_loading_)
        index_scripts();
}

void Workspace::resolve_script_target()
{
    const auto target = std::exchange(pending_script_target_, std::nullopt);
    if (!target)
        return;
    const auto match = std::ranges::find_if(script_entries_, [&](const ScriptEntry& entry)
    {
        return (entry.type || !target->qualified.empty()) && (!target->tag || entry.tag == *target->tag) &&
            (!target->type || entry.type == target->type) &&
            (target->qualified.empty() || entry.qualified == target->qualified);
    });
    if (match == script_entries_.end())
    {
        refresh_scripts();
        status(tr(L"Classe introuvable dans les blocs ABC du document."), true);
        return;
    }
    {
        const UpdatingSelection updating(refreshing_);
        const auto query = lower(script_search_.Text().c_str());
        if (match->search_key.find(query) == std::wstring::npos && lower(match->package).find(query) == std::wstring::npos)
            script_search_.Text(L"");
    }
    next_code_line_ = target->line;
    select_script(static_cast<std::size_t>(match - script_entries_.begin()), target->method, target->view);
    refresh_scripts();
}

void Workspace::refresh_gallery(bool keep_page)
{
    if (!movie_ || mode_ != Mode::resources)
        return;
    if (thumbnails_cancel_)
        thumbnails_cancel_->store(true, std::memory_order_relaxed);
    const UpdatingSelection updating(refreshing_);
    const auto query = lower(gallery_search_.Text().c_str());
    const auto named = number(query.starts_with(L'#') ? std::wstring_view(query).substr(1) : std::wstring_view(query));
    const auto category = category_filter();
    if (!keep_page)
        gallery_page_ = 0;
    visible_assets_.clear();
    for (std::size_t i = 0; i < movie_->tags().size(); ++i)
    {
        const auto& tag = movie_->tags()[i];
        if (!resource(tag) || (category && tag.category != category))
            continue;
        if (named != tag.character_id && indexed_rows_[i].search_key.find(query) == std::wstring::npos)
            continue;
        visible_assets_.push_back(i);
    }
    if (!category && query.empty())
        std::ranges::stable_partition(visible_assets_, [&](std::size_t index) { return has_thumbnail(movie_->tags()[index]); });
    const auto first = std::min(gallery_page_ * assets_per_page, visible_assets_.size());
    const auto end = std::min(first + assets_per_page, visible_assets_.size());
    const auto page = std::span(visible_assets_).subspan(first, end - first);
    if (std::ranges::find(page, selected_.value_or(SIZE_MAX)) == page.end())
        select_tag({});
    gallery_title_.Text(category ? hstring(tr(flash::category_name(*category).data())) : hstring(tr(L"Ressources")));
    gallery_count_.Text(visible_assets_.empty() ? tr(L"Aucune ressource dans cette sélection") :
        std::format(L"{} {}", visible_assets_.size(), visible_assets_.size() == 1 ? tr(L"ressource") : tr(L"ressources")));
    gallery_page_label_.Text(visible_assets_.empty() ? L"" : std::format(L"{}–{} / {}", first + 1, end, visible_assets_.size()));
    gallery_previous_.IsEnabled(gallery_page_ != 0);
    gallery_next_.IsEnabled(end < visible_assets_.size());
    tiles_.Items().Clear();
    std::vector<std::pair<std::size_t, Image>> images;
    for (auto slot = first; slot < end; ++slot)
    {
        const auto index = visible_assets_[slot];
        const auto& tag = movie_->tags()[index];
        GridViewItem tile;
        tile.Tag(box_value(static_cast<std::uint64_t>(index)));
        tile.Margin(pad(0, 0, 9, 9));
        tile.Padding(pad(0));
        tile.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        tile.VerticalContentAlignment(VerticalAlignment::Stretch);
        const auto type = widen(flash::tag_name(tag.code));
        const auto identity = std::format(L"{}  #{}", type, *tag.character_id);
        const auto name = tag.name.empty() ? identity : widen(tag.name);
        Automation::AutomationProperties::SetName(tile, tag.name.empty() ? identity : name + L" · " + identity);
        ToolTipService::SetToolTip(tile, box_value(hstring(tag.name.empty() ? identity : name + L" · " + identity)));

        Grid card;
        card.Width(154);
        card.Background(brush(surface));
        card.BorderBrush(brush(stroke));
        card.BorderThickness(pad(1));
        card.CornerRadius({8, 8, 8, 8});
        row(card, 96, GridUnitType::Pixel);
        row(card);
        Grid art;
        art.Background(brush(art_surface));
        art.CornerRadius({7, 7, 0, 0});
        art.Children().Append(glyph(resource_icon(tag.category), 26, image_accent));
        if (has_thumbnail(tag))
        {
            Image thumbnail;
            thumbnail.Stretch(Stretch::Uniform);
            thumbnail.Margin(pad(8));
            art.Children().Append(thumbnail);
            images.emplace_back(index, thumbnail);
        }
        place(card, art, 0);
        auto caption = label(name, 12, primary, true);
        caption.Margin(pad(9, 8, 9, 8));
        place(card, caption, 1);
        tile.Content(card);
        tiles_.Items().Append(tile);
        if (selected_ == index)
            tiles_.SelectedIndex(static_cast<int>(slot - first));
    }
    load_thumbnails(std::move(images));
}

fire_and_forget Workspace::load_thumbnails(std::vector<std::pair<std::size_t, Image>> tiles)
{
    if (tiles.empty())
        co_return;
    auto lifetime = shared_from_this();
    const auto movie = movie_;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    thumbnails_cancel_ = cancelled;
    const apartment_context ui;
    try
    {
        for (const auto& [index, image] : tiles)
        {
            co_await resume_background();
            if (cancelled->load(std::memory_order_relaxed))
                co_return;
            std::optional<flash::Asset> asset;
            try
            {
                const auto code = movie->tags()[index].code;
                asset = code == 11 || code == 33 ? flash::text_svg(*movie, index) : flash::extract(*movie, index);
            }
            catch (const std::runtime_error&) {}
            co_await ui;
            if (closed_ || cancelled->load(std::memory_order_relaxed))
                co_return;
            if (!asset || asset->bytes.size() > 2 * 1024 * 1024)
                continue;
            try
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
                    if (co_await svg.SetSourceAsync(stream) == SvgImageSourceLoadStatus::Success)
                        source = svg;
                }
                else
                {
                    BitmapImage bitmap;
                    bitmap.DecodePixelWidth(230);
                    co_await bitmap.SetSourceAsync(stream);
                    source = bitmap;
                }
                if (source && !closed_ && !cancelled->load(std::memory_order_relaxed))
                {
                    auto art = image.Parent().as<Grid>();
                    image.Source(source);
                    art.Children().GetAt(0).as<UIElement>().Visibility(Visibility::Collapsed);
                }
            }
            catch (const hresult_error&) {}
        }
    }
    catch (const hresult_error&) {}
    catch (const std::exception&) {}
}
}
