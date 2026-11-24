#pragma once
#include <format>
#include <string>
#include <string_view>

namespace studio::ui
{
enum class Language { english, french };

void initialize_language();
Language language();
void use_language(Language language);
void save_language(Language language);
const wchar_t* tr(const wchar_t* french);
std::wstring localized_error(std::wstring_view message);

template<typename... Args>
std::wstring format_message(const wchar_t* text, Args&&... args)
{
    return std::vformat(tr(text), std::make_wformat_args(args...));
}
}
