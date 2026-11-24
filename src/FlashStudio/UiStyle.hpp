#pragma once

#include "Workspace.hpp"
#include "UiLanguage.hpp"

#include <Windows.h>
#include <utility>

namespace studio::ui
{
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;

inline constexpr auto base = 0x121418u;
inline constexpr auto panel = 0x191C22u;
inline constexpr auto surface = 0x20242Cu;
inline constexpr auto stroke = 0x323842u;
inline constexpr auto primary = 0xF2F4F8u;
inline constexpr auto secondary = 0xB0BAC9u;
inline constexpr auto accent = 0xFFBA72u;
inline constexpr auto danger = 0xFF9299u;

inline bool resource(const flash::Tag& tag)
{
    return tag.character_id && tag.category != flash::Category::timeline && tag.category != flash::Category::scripts;
}

inline Windows::UI::Color color(unsigned rgb)
{
    return {255, static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8),
        static_cast<std::uint8_t>(rgb)};
}

inline SolidColorBrush brush(unsigned rgb) { return SolidColorBrush(color(rgb)); }

inline Thickness pad(double left, double top, double right, double bottom) { return {left, top, right, bottom}; }
inline Thickness pad(double amount) { return pad(amount, amount, amount, amount); }

inline std::wstring widen(std::string_view utf8)
{
    if (utf8.empty())
        return {};
    const auto count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), count);
    return result;
}

inline std::wstring lower(std::wstring value)
{
    if (!value.empty())
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

inline std::optional<std::size_t> number(std::wstring_view text)
{
    if (text.empty() || text.size() > 9)
        return {};
    std::size_t value = 0;
    for (const auto digit : text)
    {
        if (digit < L'0' || digit > L'9')
            return {};
        value = value * 10 + static_cast<unsigned>(digit - L'0');
    }
    return value;
}

struct UpdatingSelection
{
    explicit UpdatingSelection(bool& flag) : flag_(flag), previous_(std::exchange(flag, true)) {}
    ~UpdatingSelection() { flag_ = previous_; }
private:
    bool& flag_;
    bool previous_;
};

inline TextBlock label(std::wstring_view value, double size = 14, unsigned rgb = primary, bool strong = false)
{
    TextBlock text;
    text.Text(hstring(value));
    text.FontSize(size);
    text.Foreground(brush(rgb));
    if (strong)
        text.FontWeight(Windows::UI::Text::FontWeight{600});
    text.TextTrimming(TextTrimming::CharacterEllipsis);
    return text;
}

inline FontIcon glyph(wchar_t code, double size = 16, unsigned rgb = primary)
{
    FontIcon icon;
    icon.FontFamily(FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
    icon.Glyph(std::wstring(1, code));
    icon.FontSize(size);
    icon.Foreground(brush(rgb));
    return icon;
}

inline StackPanel horizontal(double spacing)
{
    StackPanel stack;
    stack.Orientation(Orientation::Horizontal);
    stack.Spacing(spacing);
    return stack;
}

inline Grid icon_label(wchar_t icon, std::wstring_view title, unsigned rgb = primary, double size = 13)
{
    Grid result;
    ColumnDefinition first;
    first.Width({26, GridUnitType::Pixel});
    result.ColumnDefinitions().Append(first);
    result.ColumnDefinitions().Append(ColumnDefinition());
    auto symbol = glyph(icon, 15, rgb);
    symbol.HorizontalAlignment(HorizontalAlignment::Left);
    auto text = label(title, size, rgb);
    text.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(text, 1);
    result.Children().Append(symbol);
    result.Children().Append(text);
    return result;
}

inline void row(Grid grid, double height = 1, GridUnitType type = GridUnitType::Auto)
{
    RowDefinition item;
    item.Height({height, type});
    grid.RowDefinitions().Append(item);
}
inline ColumnDefinition column(Grid grid, double width, GridUnitType type = GridUnitType::Pixel)
{
    ColumnDefinition item;
    item.Width({width, type});
    grid.ColumnDefinitions().Append(item);
    return item;
}
inline void place(Grid grid, const FrameworkElement& item, int grid_row, int grid_column = 0)
{
    Grid::SetRow(item, grid_row);
    Grid::SetColumn(item, grid_column);
    grid.Children().Append(item);
}

inline Border divider()
{
    Border line;
    line.Height(1);
    line.Background(brush(stroke));
    return line;
}

}
