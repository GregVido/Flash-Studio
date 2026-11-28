#include "Workspace.hpp"
#include "UiStyle.hpp"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <algorithm>
#include <cwctype>
#include <limits>
#include <numeric>

namespace studio
{
using namespace ui;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
namespace automation = Microsoft::UI::Xaml::Automation;

namespace
{
constexpr std::size_t glyphs_per_page = 80, bytes_per_page = 1024;

void copy_text(std::wstring_view value)
{
    DataPackage data;
    data.RequestedOperation(DataPackageOperation::Copy);
    data.SetText(hstring(value));
    Clipboard::SetContent(data);
}

TextBlock mono(std::wstring_view text, unsigned tint = primary)
{
    auto value = label(text, 12, tint);
    value.FontFamily(FontFamily(L"Consolas"));
    return value;
}

void property(const StackPanel& properties, const wchar_t* name, std::wstring_view value)
{
    if (value.empty()) return;
    StackPanel entry;
    entry.Spacing(3);
    entry.Children().Append(label(tr(name), 11, secondary, true));
    auto content = label(value, 13);
    content.TextWrapping(TextWrapping::Wrap);
    content.IsTextSelectionEnabled(true);
    entry.Children().Append(content);
    properties.Children().Append(entry);
}

std::wstring unicode(wchar_t value, std::size_t index)
{
    return value == L'\uFFFD' ? std::format(L"#{}", index) : std::format(L"U+{:04X}", static_cast<unsigned>(value));
}

Button pager(wchar_t symbol, const wchar_t* name)
{
    Button button;
    button.Content(glyph(symbol, 12));
    automation::AutomationProperties::SetName(button, tr(name));
    ToolTipService::SetToolTip(button, box_value(tr(name)));
    return button;
}

std::optional<std::size_t> hex_address(std::wstring text)
{
    const auto first = text.find_first_not_of(L" \t");
    if (first == text.npos) return {};
    text = lower(text.substr(first, text.find_last_not_of(L" \t") - first + 1));
    if (text.starts_with(L"0x")) text.erase(0, 2);
    if (text.empty() || text.size() > sizeof(std::size_t) * 2 || text.find_first_not_of(L"0123456789abcdef") != text.npos)
        return {};
    return static_cast<std::size_t>(std::stoull(text, nullptr, 16));
}
}

struct Workspace::FontView
{
    std::shared_ptr<const flash::Movie> movie;
    std::size_t tag{}, page{};
    std::uint64_t revision{};
    flash::FontPreview font;
    std::vector<std::size_t> filtered;
    GridView tiles;
    TextBox filter;
    TextBlock page_text, hint;
    Grid toolbar;
    Button previous{nullptr}, next{nullptr};
    ProgressRing progress;
};

struct Workspace::HexView
{
    std::shared_ptr<const flash::Movie> movie;
    flash::ByteView bytes;
    std::size_t origin{};
    TextBox address, body;
    TextBlock range;
    Button previous{nullptr}, next{nullptr};
    std::wstring text;
};

void Workspace::hide_resource_preview()
{
    font_view_.reset();
    hex_view_.reset();
    resource_preview_.Children().Clear();
    resource_preview_.Visibility(Visibility::Collapsed);
}

void Workspace::show_font(std::shared_ptr<const flash::Movie> movie, std::size_t tag, flash::FontPreview font)
{
    auto state = std::make_shared<FontView>();
    state->movie = std::move(movie);
    state->tag = tag;
    state->font = std::move(font);
    font_view_ = state;
    const auto& info = state->font;
    const auto name = info.name.empty() ? std::format(L"#{}", state->movie->tags()[tag].character_id.value_or(0)) : widen(info.name);
    if (!info.name.empty())
        detail_title_.Text(name);
    output_.Visibility(Visibility::Collapsed);
    resource_preview_.Visibility(Visibility::Visible);

    Grid canvas;
    canvas.Margin(pad(16, 10, 16, 16));
    canvas.ColumnSpacing(16);
    canvas.RowSpacing(8);
    column(canvas, 1, GridUnitType::Star);
    const auto info_column = column(canvas, 242);
    row(canvas, 1, GridUnitType::Star);
    row(canvas);
    place(resource_preview_, canvas, 0);

    Grid browser;
    browser.RowSpacing(10);
    row(browser);
    row(browser, 1, GridUnitType::Star);
    row(browser);
    auto heading = label(tr(L"Glyphes incorporés"), 17, primary, true);
    heading.VerticalAlignment(VerticalAlignment::Center);
    column(state->toolbar, 1, GridUnitType::Star);
    column(state->toolbar, 1, GridUnitType::Auto);
    state->toolbar.ColumnSpacing(10);
    place(state->toolbar, heading, 0);
    state->filter.PlaceholderText(tr(L"Caractère ou U+…"));
    state->filter.MaxLength(30);
    state->filter.Width(174);
    automation::AutomationProperties::SetName(state->filter, tr(L"Rechercher un glyphe"));
    place(state->toolbar, state->filter, 0, 1);
    place(browser, state->toolbar, 0);
    state->tiles.SelectionMode(ListViewSelectionMode::Single);
    automation::AutomationProperties::SetName(state->tiles, tr(L"Glyphes incorporés"));
    place(browser, state->tiles, 1);
    state->progress.Width(26);
    state->progress.Height(26);
    state->progress.HorizontalAlignment(HorizontalAlignment::Center);
    state->progress.VerticalAlignment(VerticalAlignment::Center);
    state->progress.Visibility(Visibility::Collapsed);
    place(browser, state->progress, 1);
    auto bottom = horizontal(7);
    state->previous = pager(L'\uE76B', L"Précédent");
    state->next = pager(L'\uE76C', L"Suivant");
    state->page_text = label(L"", 12, secondary);
    state->page_text.VerticalAlignment(VerticalAlignment::Center);
    bottom.Children().Append(state->previous);
    bottom.Children().Append(state->next);
    bottom.Children().Append(state->page_text);
    place(browser, bottom, 2);
    state->hint = label(tr(L"Aucun glyphe correspondant"), 13, secondary);
    state->hint.Margin(pad(3, 12, 3, 0));
    state->hint.VerticalAlignment(VerticalAlignment::Top);
    state->hint.Visibility(Visibility::Collapsed);
    place(browser, state->hint, 1);
    place(canvas, browser, 0);

    Border properties_card;
    properties_card.Background(brush(surface));
    properties_card.BorderBrush(brush(stroke));
    properties_card.BorderThickness(pad(1));
    properties_card.CornerRadius({10, 10, 10, 10});
    ScrollViewer details;
    details.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    StackPanel properties;
    properties.Spacing(13);
    properties.Padding(pad(16));
    properties.Children().Append(label(tr(L"Informations"), 16, primary, true));
    property(properties, L"Nom dans le SWF", name);
    property(properties, L"Nom complet", widen(info.full_name));
    property(properties, L"Droits d’auteur", widen(info.copyright));
    std::wstring style = info.bold ? tr(L"Gras") : tr(L"Normal");
    if (info.italic) style = info.bold ? style + L" · " + tr(L"Italique") : tr(L"Italique");
    property(properties, L"Style", style);
    property(properties, L"Nombre de glyphes", std::to_wstring(info.characters.size()));
    if (info.metrics)
    {
        property(properties, L"Ascendante", std::to_wstring((*info.metrics)[0]));
        property(properties, L"Descendante", std::to_wstring((*info.metrics)[1]));
        property(properties, L"Interligne", std::to_wstring((*info.metrics)[2]));
        property(properties, L"Unités par em", std::to_wstring(info.units));
    }
    std::wstring characters;
    for (const auto character : info.characters)
        if (iswprint(character) && character != L'\uFFFD')
            characters.push_back(character);
    if (!characters.empty())
    {
        Button copy;
        copy.Content(icon_label(L'\uE8C8', tr(L"Copier les caractères")));
        copy.Click([weak = weak_from_this(), characters = std::move(characters)](auto&&, auto&&)
        {
            try { copy_text(characters); }
            catch (const hresult_error& error) { if (const auto owner = weak.lock()) owner->status(error.message(), true); }
        });
        properties.Children().Append(copy);
    }
    details.Content(properties);
    properties_card.Child(details);
    place(canvas, properties_card, 0, 1);
    canvas.SizeChanged([properties_card, info_column, details](auto&&, const SizeChangedEventArgs& args)
    {
        const auto wide = args.NewSize().Width >= 650;
        info_column.Width({wide ? 242.0 : 0.0, GridUnitType::Pixel});
        Grid::SetRow(properties_card, wide ? 0 : 1);
        Grid::SetColumn(properties_card, wide ? 1 : 0);
        Grid::SetColumnSpan(properties_card, wide ? 1 : 2);
        details.MaxHeight(wide ? std::numeric_limits<double>::infinity() : 133.0);
    });

    if (info.characters.empty())
    {
        state->filter.Visibility(Visibility::Collapsed);
        state->tiles.Visibility(Visibility::Collapsed);
        bottom.Visibility(Visibility::Collapsed);
        const auto message = info.cff ? tr(L"Police CFF incorporée. Tracés indisponibles.") :
            tr(L"Aucun glyphe incorporé. Aperçu de la police système si elle est installée.");
        heading.Text(tr(L"Échantillon"));
        StackPanel sample;
        sample.Spacing(18);
        sample.Margin(pad(4, 18, 8, 0));
        auto notice = label(message, 13, secondary);
        notice.TextWrapping(TextWrapping::Wrap);
        sample.Children().Append(notice);
        if (!info.cff && !info.name.empty())
        {
            auto specimen = label(L"Aa Bb Cc · 0123456789\nÀ É È Œ æ & @ ?", 32);
            specimen.FontFamily(FontFamily(name));
            specimen.TextWrapping(TextWrapping::Wrap);
            if (info.bold) specimen.FontWeight(Windows::UI::Text::FontWeight{700});
            if (info.italic) specimen.FontStyle(Windows::UI::Text::FontStyle::Italic);
            sample.Children().Append(specimen);
        }
        place(browser, sample, 1);
        return;
    }

    state->filter.TextChanged([this](auto&&, auto&&)
    {
        const auto current = font_view_;
        if (!current) return;
        const auto query = lower(current->filter.Text().c_str());
        current->filtered.clear();
        for (std::size_t i = 0; i < current->font.characters.size(); ++i)
        {
            const auto c = current->font.characters[i];
            if (query.empty() || (query.size() == 1 && towlower(c) == query.front()) ||
                (query.size() > 1 && lower(unicode(c, i)).starts_with(query)))
                current->filtered.push_back(i);
        }
        current->page = 0;
        render_glyphs();
    });
    state->previous.Click([this](auto&&, auto&&) { if (font_view_ && font_view_->page) { --font_view_->page; render_glyphs(); } });
    state->next.Click([this](auto&&, auto&&) { if (font_view_) { ++font_view_->page; render_glyphs(); } });
    state->filtered.resize(info.characters.size());
    std::iota(state->filtered.begin(), state->filtered.end(), std::size_t{});
    render_glyphs();
}

fire_and_forget Workspace::render_glyphs()
{
    const auto lifetime = shared_from_this();
    const auto state = font_view_;
    if (!state) co_return;
    const auto revision = ++state->revision;
    const auto begin = std::min(state->page * glyphs_per_page, state->filtered.size());
    const auto end = std::min(begin + glyphs_per_page, state->filtered.size());
    state->previous.IsEnabled(begin != 0);
    state->next.IsEnabled(end < state->filtered.size());
    state->tiles.Items().Clear();
    state->page_text.Text(begin == end ? L"" : std::format(L"{}–{} / {}", begin + 1, end, state->filtered.size()));
    state->hint.Visibility(begin == end ? Visibility::Visible : Visibility::Collapsed);
    state->hint.Text(tr(L"Aucun glyphe correspondant"));
    state->progress.Visibility(begin == end ? Visibility::Collapsed : Visibility::Visible);
    state->progress.IsActive(begin != end);
    if (begin == end) co_return;
    const std::vector<std::size_t> glyphs(state->filtered.begin() + begin, state->filtered.begin() + end);
    const apartment_context ui;
    co_await resume_background();
    std::vector<std::string> paths;
    std::wstring failure;
    try { paths = flash::font_outlines(*state->movie, state->tag, glyphs); }
    catch (const std::exception& error) { failure = widen(error.what()); }
    co_await ui;
    if (closed_ || state != font_view_ || revision != state->revision) co_return;
    state->progress.IsActive(false);
    state->progress.Visibility(Visibility::Collapsed);
    if (!failure.empty())
    {
        state->hint.Text(failure);
        state->hint.Visibility(Visibility::Visible);
        co_return;
    }
    const auto scale = 39.0 / state->font.units;
    for (std::size_t slot = 0; slot < paths.size(); ++slot)
    {
        const auto character = state->font.characters[glyphs[slot]];
        const auto code = unicode(character, glyphs[slot]);
        GridViewItem tile;
        tile.Width(74);
        tile.Height(72);
        tile.Padding(pad(3, 1, 3, 4));
        tile.Margin(pad(0, 0, 5, 5));
        tile.Background(brush(surface));
        tile.CornerRadius({7, 7, 7, 7});
        automation::AutomationProperties::SetName(tile, hstring(code));
        Grid layout;
        row(layout, 1, GridUnitType::Star);
        row(layout);
        if (!paths[slot].empty())
        {
            try
            {
                const auto xaml = std::format(L"<Path xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" Data=\"{}\"/>",
                    widen(paths[slot]));
                auto outline = Markup::XamlReader::Load(xaml).as<Shapes::Path>();
                const auto geometry = outline.Data();
                const auto bounds = geometry.Bounds();
                const auto fit = std::min({scale, 62.0 / std::max(1.0f, bounds.Width), 47.0 / std::max(1.0f, bounds.Height)});
                MatrixTransform normalize;
                normalize.Matrix({fit, 0, 0, fit, -bounds.X * fit, -bounds.Y * fit});
                geometry.Transform(normalize);
                outline.Fill(brush(primary));
                outline.Stretch(Stretch::None);
                outline.HorizontalAlignment(HorizontalAlignment::Center);
                outline.VerticalAlignment(VerticalAlignment::Center);
                place(layout, outline, 0);
            }
            catch (const hresult_error&)
            {
                auto fallback = label(L"·", 25, secondary);
                fallback.HorizontalAlignment(HorizontalAlignment::Center);
                place(layout, fallback, 0);
            }
        }
        auto identifier = mono(code, secondary);
        identifier.FontSize(10);
        identifier.HorizontalAlignment(HorizontalAlignment::Center);
        place(layout, identifier, 1);
        tile.Content(layout);
        state->tiles.Items().Append(tile);
    }
    if (state->tiles.Items().Size())
        state->tiles.ScrollIntoView(state->tiles.Items().GetAt(0));
}

void Workspace::show_hex(std::shared_ptr<const flash::Movie> movie, std::size_t index, bool payload)
{
    auto state = std::make_shared<HexView>();
    state->movie = std::move(movie);
    state->bytes = state->movie->data(index);
    if (payload) state->bytes = state->bytes.subspan(6);
    hex_view_ = state;
    const auto& tag = state->movie->tags()[index];
    if (payload)
        detail_title_.Text(std::format(L"{} #{}", tr(L"Données binaires"), tag.character_id.value_or(0)));
    output_.Visibility(Visibility::Collapsed);
    resource_preview_.Visibility(Visibility::Visible);
    Grid reader;
    reader.Margin(pad(16, 10, 16, 16));
    reader.RowSpacing(9);
    row(reader);
    row(reader);
    row(reader, 1, GridUnitType::Star);
    row(reader);
    auto information = horizontal(10);
    auto summary = label(format_message(L"{} octets · Offset SWF 0x{:X}", state->bytes.size(), tag.offset + (payload ? 6 : 0)), 13, secondary);
    summary.VerticalAlignment(VerticalAlignment::Center);
    information.Children().Append(summary);
    place(reader, information, 0);
    Grid tools;
    column(tools, 1, GridUnitType::Star);
    column(tools, 1, GridUnitType::Auto);
    row(tools);
    row(tools);
    tools.RowSpacing(6);
    auto traverse = horizontal(6);
    state->address.Width(141);
    state->address.MaxLength(20);
    state->address.PlaceholderText(L"0x00000000");
    automation::AutomationProperties::SetName(state->address, tr(L"Adresse hexadécimale"));
    Button go;
    go.Content(box_value(tr(L"Aller à l’adresse")));
    const auto seek = [this]
    {
        const auto current = hex_view_;
        if (!current) return;
        const auto address = hex_address(current->address.Text().c_str());
        if (!address || *address >= current->bytes.size())
        {
            status(tr(L"Adresse hexadécimale hors limites."), true);
            return;
        }
        current->origin = *address - *address % 16;
        render_hex();
    };
    go.Click([seek](auto&&, auto&&) { seek(); });
    state->address.KeyDown([seek](auto&&, const Input::KeyRoutedEventArgs& args)
    {
        if (args.Key() == Windows::System::VirtualKey::Enter) { seek(); args.Handled(true); }
    });
    traverse.Children().Append(state->address);
    traverse.Children().Append(go);
    place(tools, traverse, 0);
    Button copy;
    copy.Content(icon_label(L'\uE8C8', tr(L"Copier la page")));
    copy.Click([this](auto&&, auto&&)
    {
        try { if (hex_view_) copy_text(hex_view_->text); }
        catch (const hresult_error& error) { status(error.message(), true); }
    });
    place(tools, copy, 0, 1);
    tools.SizeChanged([traverse, copy](auto&&, const SizeChangedEventArgs& args)
    {
        const auto wide = args.NewSize().Width >= 420;
        Grid::SetColumnSpan(traverse, wide ? 1 : 2);
        Grid::SetRow(copy, wide ? 0 : 1);
        Grid::SetColumn(copy, wide ? 1 : 0);
        copy.HorizontalAlignment(wide ? HorizontalAlignment::Right : HorizontalAlignment::Left);
    });
    place(reader, tools, 1);
    state->body.FontFamily(FontFamily(L"Consolas"));
    state->body.FontSize(13);
    state->body.IsReadOnly(true);
    state->body.AcceptsReturn(true);
    state->body.TextWrapping(TextWrapping::NoWrap);
    state->body.Background(brush(base));
    state->body.Foreground(brush(primary));
    state->body.BorderBrush(brush(stroke));
    state->body.Padding(pad(12));
    state->body.Header(box_value(tr(L"Adresse, octets hexadécimaux et ASCII")));
    ScrollViewer::SetHorizontalScrollBarVisibility(state->body, ScrollBarVisibility::Auto);
    ScrollViewer::SetVerticalScrollBarVisibility(state->body, ScrollBarVisibility::Auto);
    automation::AutomationProperties::SetName(state->body, tr(L"Vue hexadécimale"));
    place(reader, state->body, 2);
    auto pagination = horizontal(7);
    state->previous = pager(L'\uE76B', L"Précédent");
    state->next = pager(L'\uE76C', L"Suivant");
    state->range = label(L"", 12, secondary);
    state->range.VerticalAlignment(VerticalAlignment::Center);
    pagination.Children().Append(state->previous);
    pagination.Children().Append(state->next);
    pagination.Children().Append(state->range);
    state->previous.Click([this](auto&&, auto&&)
    {
        if (!hex_view_) return;
        const auto origin = hex_view_->origin;
        hex_view_->origin = origin >= bytes_per_page ? origin - bytes_per_page : 0;
        render_hex();
    });
    state->next.Click([this](auto&&, auto&&)
    {
        if (!hex_view_ || hex_view_->origin + bytes_per_page >= hex_view_->bytes.size()) return;
        hex_view_->origin += bytes_per_page;
        render_hex();
    });
    place(reader, pagination, 3);
    place(resource_preview_, reader, 0);
    render_hex();
    if (payload)
    {
        const auto read_embedded = [](std::weak_ptr<Workspace> weak, std::shared_ptr<HexView> active,
            StackPanel heading) -> fire_and_forget
        {
            const apartment_context ui;
            co_await resume_background();
            std::wstring nested;
            try { nested = widen(flash::binary_swf_summary(active->bytes)); }
            catch (const std::exception&) {}
            co_await ui;
            const auto owner = weak.lock();
            if (!owner || owner->closed_ || owner->hex_view_ != active || nested.empty())
                co_return;
            try
            {
                DropDownButton details;
                details.Content(box_value(tr(L"SWF incorporé")));
                Flyout flyout;
                auto text = mono(nested);
                text.IsTextSelectionEnabled(true);
                flyout.Content(text);
                details.Flyout(flyout);
                heading.Children().Append(details);
            }
            catch (const hresult_error&) {}
        };
        read_embedded(weak_from_this(), state, information);
    }
}

void Workspace::render_hex()
{
    auto& state = *hex_view_;
    const auto end = std::min(state.origin + bytes_per_page, state.bytes.size());
    state.text = L"          00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F   ASCII\n\n";
    for (auto line = state.origin; line < end; line += 16)
    {
        state.text += std::format(L"{:08X}  ", line);
        std::wstring characters;
        for (std::size_t i = 0; i < 16; ++i)
        {
            if (i == 8) state.text += L' ';
            if (line + i >= end) { state.text += L"   "; continue; }
            const auto value = state.bytes[line + i];
            state.text += std::format(L"{:02X} ", value);
            characters.push_back(value >= 32 && value <= 126 ? static_cast<wchar_t>(value) : L'.');
        }
        state.text += L"  " + characters + L'\n';
    }
    state.body.Text(state.text);
    state.body.SelectionStart(0);
    state.address.Text(std::format(L"0x{:08X}", state.origin));
    state.range.Text(end ? std::format(L"0x{:08X}–0x{:08X}   ·   {}", state.origin, end - 1, state.bytes.size()) : L"");
    state.previous.IsEnabled(state.origin != 0);
    state.next.IsEnabled(end < state.bytes.size());
}
}
