#pragma once

#include <functional>
#include <string_view>
#include <winrt/Microsoft.UI.Xaml.Documents.h>

namespace studio
{
enum class CodeReferenceKind { call, constructor, type };
using CodeReference = std::function<std::function<void()>(std::wstring_view receiver, std::wstring_view name, CodeReferenceKind kind)>;
using CodeType = std::function<bool(std::wstring_view)>;
winrt::Microsoft::UI::Xaml::Documents::Paragraph color_code(std::wstring_view code, bool instructions,
    const CodeReference& reference = {}, const CodeType& known_type = {});
}
