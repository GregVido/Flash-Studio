#include "CodeColors.hpp"

#include <algorithm>
#include <array>
#include <Windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

namespace studio
{
namespace
{
enum class Token { plain, command, literal, number, comment, label, type };

bool digit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool identifier(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'_' || c == L'$';
}

bool keyword(std::wstring_view word)
{
    constexpr std::wstring_view words[]{
        L"as", L"break", L"case", L"catch", L"class", L"const", L"continue", L"default", L"delete", L"do",
        L"dynamic", L"else", L"extends", L"false", L"final", L"finally", L"for", L"function", L"get", L"if",
        L"implements", L"import", L"in", L"instanceof", L"interface", L"internal", L"is", L"namespace",
        L"new", L"null", L"override", L"package", L"private", L"protected", L"public", L"return", L"set",
        L"static", L"super", L"switch", L"this", L"throw", L"true", L"try", L"typeof", L"undefined",
        L"use", L"var", L"void", L"while", L"with"
    };
    for (const auto candidate : words)
        if (word == candidate)
            return true;
    return false;
}
}

winrt::Microsoft::UI::Xaml::Documents::Paragraph color_code(std::wstring_view text, bool instructions,
    const CodeReference& reference, const CodeType& known_type)
{
    using namespace winrt::Microsoft::UI::Xaml::Documents;
    using winrt::Microsoft::UI::Xaml::Media::SolidColorBrush;
    const std::array<SolidColorBrush, 7> colors{
        nullptr, SolidColorBrush({255, 174, 157, 255}), SolidColorBrush({255, 154, 217, 164}),
        SolidColorBrush({255, 255, 190, 116}), SolidColorBrush({255, 156, 174, 190}),
        SolidColorBrush({255, 124, 205, 243}), SolidColorBrush({255, 128, 219, 190})
    };
    Paragraph paragraph;
    auto inlines = paragraph.Inlines();
    auto append = [&](std::wstring_view value, Token token)
    {
        Run run;
        run.Text(winrt::hstring(value));
        if (token != Token::plain)
            run.Foreground(colors[static_cast<std::size_t>(token)]);
        inlines.Append(run);
    };
    const auto name_character = [](wchar_t c) { return identifier(c) || digit(c); };
    const auto qualifier_start = [&](std::size_t first)
    {
        while (first && (name_character(text[first - 1]) || text[first - 1] == L'.'))
            --first;
        return first;
    };
    const auto type_position = [&](std::wstring_view name, std::size_t first, std::size_t last)
    {
        if (known_type && known_type(name))
            return true;
        constexpr std::wstring_view builtins[]{L"Array", L"Boolean", L"Class", L"Date", L"Error", L"Function", L"int", L"Namespace",
            L"Number", L"Object", L"QName", L"RegExp", L"String", L"uint", L"Vector", L"void", L"XML", L"XMLList"};
        if (std::ranges::find(builtins, name) != std::end(builtins))
            return true;
        auto context = first;
        while (context && (text[context - 1] == L' ' || text[context - 1] == L'\t' ||
            text[context - 1] == L'.' || name_character(text[context - 1])))
        {
            if (text[context - 1] == L' ' || text[context - 1] == L'\t')
                break;
            --context;
        }
        while (context && (text[context - 1] == L' ' || text[context - 1] == L'\t'))
            --context;
        if (context && text[context - 1] == L'<' && context > 1 && text[context - 2] == L'.')
            return true;
        const auto end = context;
        while (context && name_character(text[context - 1]))
            --context;
        const auto previous = text.substr(context, end - context);
        if (previous == L"new" || previous == L"as" || previous == L"is" || previous == L"extends" ||
            previous == L"implements" || previous == L"class" || previous == L"interface" || previous == L"instanceof")
            return true;
        return last > first && name.front() >= L'A' && name.front() <= L'Z' && end && text[end - 1] == L':';
    };
    const auto call_target = [&](std::size_t first, std::size_t last) -> std::function<void()>
    {
        if (!reference)
            return {};
        const auto next = text.find_first_not_of(L" \t", last);
        if (next == std::wstring_view::npos || text[next] != L'(')
            return {};
        auto receiver_start = first;
        std::wstring_view receiver;
        if (first && text[first - 1] == L'.')
        {
            --receiver_start;
            receiver_start = qualifier_start(receiver_start);
            receiver = text.substr(receiver_start, first - receiver_start - 1);
            if (receiver.empty())
                return {};
        }
        auto prefix = receiver_start;
        while (prefix && (text[prefix - 1] == L' ' || text[prefix - 1] == L'\t'))
            --prefix;
        auto keyword_start = prefix;
        while (keyword_start && name_character(text[keyword_start - 1]))
            --keyword_start;
        const auto before = text.substr(keyword_start, prefix - keyword_start);
        if (before == L"function" || before == L"get" || before == L"set")
            return {};
        return reference(receiver, text.substr(first, last - first),
            before == L"new" ? CodeReferenceKind::constructor : CodeReferenceKind::call);
    };
    std::size_t start = 0, i = 0;
    bool mnemonic = instructions;
    bool block_comment = false;
    while (i < text.size())
    {
        const auto c = text[i];
        if (c == L'\n')
        {
            if (i > start)
                append(text.substr(start, i - start), Token::plain);
            inlines.Append(LineBreak());
            start = ++i;
            mnemonic = instructions;
            continue;
        }
        auto end = i + 1;
        auto token = Token::plain;
        std::function<void()> navigate;
        std::function<void()> open_type;
        if (block_comment || (!instructions && text.substr(i).starts_with(L"/*")))
        {
            const auto close = text.find(L"*/", block_comment ? i : i + 2);
            const auto newline = text.find(L'\n', i);
            const auto limit = close == std::wstring_view::npos ? text.size() : close + 2;
            end = (std::min)(limit, newline);
            block_comment = close == std::wstring_view::npos || newline < limit;
            token = Token::comment;
        }
        else if ((c == L'/' && i + 1 < text.size() && text[i + 1] == L'/') || (instructions && c == L';'))
        {
            end = text.find(L'\n', i);
            if (end == std::wstring_view::npos)
                end = text.size();
            token = Token::comment;
        }
        else if (c == L'"' || c == L'\'')
        {
            while (end < text.size() && text[end] != L'\n')
            {
                const auto next = text[end++];
                if (next == c)
                    break;
                if (next == L'\\' && end < text.size() && text[end] != L'\n')
                    ++end;
            }
            token = Token::literal;
        }
        else if (digit(c) || (c == L'-' && end < text.size() && digit(text[end])))
        {
            while (end < text.size() && (digit(text[end]) || identifier(text[end]) || text[end] == L'.'))
                ++end;
            token = Token::number;
        }
        else if (identifier(c) || (instructions && c == L'.'))
        {
            while (end < text.size() && (identifier(text[end]) || digit(text[end])))
                ++end;
            const auto first = text.substr(i, end - i);
            if (instructions && end < text.size() && text[end] == L':')
                token = Token::label;
            else if (instructions && mnemonic)
            {
                token = Token::command;
                mnemonic = false;
            }
            else if (!instructions && type_position(first, i, end))
            {
                token = Token::type;
                navigate = call_target(i, end);
                if (reference)
                {
                    const auto start_of_type = qualifier_start(i);
                    open_type = reference({}, text.substr(start_of_type, end - start_of_type), CodeReferenceKind::type);
                }
            }
            else if (!instructions && keyword(first))
                token = Token::command;
            else if (!instructions)
                navigate = call_target(i, end);
        }
        if (navigate || open_type)
        {
            if (i > start)
                append(text.substr(start, i - start), Token::plain);
            Hyperlink link;
            link.Foreground(colors[static_cast<std::size_t>(token == Token::type ? Token::type : Token::label)]);
            link.UnderlineStyle(UnderlineStyle::None);
            Run name;
            name.Text(winrt::hstring(text.substr(i, end - i)));
            link.Inlines().Append(name);
            link.Click([action = std::move(navigate), type = std::move(open_type)](auto&&, auto&&)
            {
                if (type && GetKeyState(VK_CONTROL) < 0)
                    type();
                else if (action)
                    action();
            });
            inlines.Append(link);
            start = end;
        }
        else if (token != Token::plain)
        {
            if (i > start)
                append(text.substr(start, i - start), Token::plain);
            append(text.substr(i, end - i), token);
            start = end;
        }
        i = end;
    }
    if (start < text.size())
        append(text.substr(start), Token::plain);
    return paragraph;
}
}
