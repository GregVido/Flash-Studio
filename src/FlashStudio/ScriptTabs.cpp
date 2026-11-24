#include "UiStyle.hpp"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <algorithm>

namespace studio
{
using namespace ui;

void Workspace::build_script_tabs()
{
    script_tabs_bar_ = TabView();
    script_tabs_bar_.Height(42);
    script_tabs_bar_.Margin(pad(9, 3, 9, 0));
    script_tabs_bar_.IsAddTabButtonVisible(false);
    script_tabs_bar_.TabWidthMode(TabViewWidthMode::SizeToContent);
    script_tabs_bar_.Visibility(Visibility::Collapsed);
    Automation::AutomationProperties::SetName(script_tabs_bar_, tr(L"Onglets de classes"));
    script_tabs_bar_.SelectionChanged([this](auto&&, auto&&)
    {
        if (refreshing_ || !active_document_)
            return;
        auto& tabs = documents_[*active_document_].script_tabs;
        const auto index = script_tabs_bar_.SelectedIndex();
        if (busy_)
            refresh_script_tabs();
        else if (index >= 0 && static_cast<std::size_t>(index) < tabs.size())
            activate_script_tab(tabs[index]);
    });
    script_tabs_bar_.TabCloseRequested([this](auto&&, const TabViewTabCloseRequestedEventArgs& args)
    {
        if (busy_ || !active_document_)
            return;
        auto& tabs = documents_[*active_document_].script_tabs;
        const auto tab = std::ranges::find(tabs, args.Tab(), &ScriptTab::item);
        if (tab == tabs.end() || tab->pinned)
            return;
        const auto index = static_cast<std::size_t>(tab - tabs.begin());
        const bool active = current_script_tab() == &*tab;
        tabs.erase(tab);
        if (active && !tabs.empty())
            activate_script_tab(tabs[std::min(index, tabs.size() - 1)]);
        else if (active)
        {
            selected_script_.reset();
            previous_script_.reset();
            member_lines_.clear();
            select_tag({});
            refresh_scripts();
        }
        refresh_script_tabs();
    });
    place(inspector_, script_tabs_bar_, 0);
}

Workspace::ScriptTab* Workspace::current_script_tab()
{
    if (!active_document_ || !selected_script_ || *selected_script_ >= script_entries_.size())
        return nullptr;
    const auto& script = script_entries_[*selected_script_];
    auto& tabs = documents_[*active_document_].script_tabs;
    const auto found = std::ranges::find_if(tabs, [&](const ScriptTab& tab)
    {
        return tab.script.tag == script.tag && tab.script.qualified == script.qualified;
    });
    return found == tabs.end() ? nullptr : &*found;
}

void Workspace::remember_script_tab()
{
    if (auto* tab = current_script_tab())
    {
        tab->script.method = abc_method_;
        tab->view = view_;
        if (code_visible_)
            tab->line = code_line_;
    }
}

void Workspace::track_script_tab(const ScriptEntry& entry)
{
    if (active_document_ && entry.type && !current_script_tab())
        documents_[*active_document_].script_tabs.push_back({{entry.tag, entry.qualified, abc_method_}, view_});
    refresh_script_tabs();
}

void Workspace::activate_script_tab(const ScriptTab& tab)
{
    open_script_target({tab.script.tag, {}, tab.script.qualified, tab.script.method, tab.line, tab.view});
}

void Workspace::toggle_script_pin(std::size_t tag, std::wstring_view qualified)
{
    if (busy_ || !active_document_)
        return;
    auto& tabs = documents_[*active_document_].script_tabs;
    const auto item = std::ranges::find_if(tabs, [&](const ScriptTab& tab)
    {
        return tab.script.tag == tag && tab.script.qualified == qualified;
    });
    if (item == tabs.end())
        return;
    item->pinned = !item->pinned;
    item->item = nullptr;
    std::stable_partition(tabs.begin(), tabs.end(), [](const ScriptTab& tab) { return tab.pinned; });
    refresh_script_tabs();
}

void Workspace::update_script_pin()
{
    const auto* tab = mode_ == Mode::scripts ? current_script_tab() : nullptr;
    script_pin_.Visibility(tab ? Visibility::Visible : Visibility::Collapsed);
    if (!tab)
        return;
    const auto action = tab->pinned ? tr(L"Désépingler la classe") : tr(L"Épingler la classe");
    script_pin_.Content(glyph(L'\uE718', 13, tab->pinned ? accent : primary));
    Automation::AutomationProperties::SetName(script_pin_, action);
    ToolTipService::SetToolTip(script_pin_, box_value(action));
}

void Workspace::refresh_script_tabs()
{
    update_script_pin();
    const UpdatingSelection updating(refreshing_);
    const auto items = script_tabs_bar_.TabItems();
    if (mode_ != Mode::scripts || !active_document_)
    {
        items.Clear();
        script_tabs_bar_.Visibility(Visibility::Collapsed);
        return;
    }

    auto& tabs = documents_[*active_document_].script_tabs;
    if (scripts_indexed_)
        std::erase_if(tabs, [&](const ScriptTab& tab)
        {
            return std::ranges::none_of(script_entries_, [&](const ScriptEntry& entry)
            {
                return entry.type && entry.tag == tab.script.tag && entry.qualified == tab.script.qualified;
            });
        });
    const auto* active = current_script_tab();
    int selected = -1;
    bool rebuild = items.Size() != tabs.size();
    for (std::size_t i = 0; i < tabs.size(); ++i)
    {
        auto& tab = tabs[i];
        if (&tab == active)
            selected = static_cast<int>(i);
        if (!tab.item)
        {
            const auto& name = tab.script.qualified;
            const auto separator = name.rfind(L'.');
            const auto short_name = separator == std::wstring::npos ? std::wstring_view(name) : std::wstring_view(name).substr(separator + 1);
            const auto header = horizontal(7);
            auto caption = label(short_name, 13);
            caption.MaxWidth(145);
            caption.VerticalAlignment(VerticalAlignment::Center);
            header.Children().Append(caption);

            const auto action = tab.pinned ? tr(L"Désépingler la classe") : tr(L"Épingler la classe");
            if (tab.pinned)
                header.Children().Append(glyph(L'\uE718', 11, accent));
            MenuFlyout context;
            MenuFlyoutItem pin;
            pin.Text(action);
            pin.Click([this, tag = tab.script.tag, qualified = name](auto&&, auto&&)
            {
                toggle_script_pin(tag, qualified);
            });
            context.Items().Append(pin);

            tab.item = TabViewItem();
            tab.item.Header(header);
            tab.item.IsClosable(!tab.pinned);
            tab.item.ContextFlyout(context);
            ToolTipService::SetToolTip(tab.item, box_value(hstring(name)));
            Automation::AutomationProperties::SetName(tab.item, name);
        }
        rebuild = rebuild || items.GetAt(static_cast<unsigned>(i)) != tab.item;
    }
    if (rebuild)
    {
        items.Clear();
        for (const auto& tab : tabs)
            items.Append(tab.item);
    }
    script_tabs_bar_.SelectedIndex(selected);
    script_tabs_bar_.Visibility(tabs.empty() ? Visibility::Collapsed : Visibility::Visible);
}
}
