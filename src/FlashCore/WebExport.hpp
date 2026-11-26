#pragma once

#include "Swf.hpp"

#include <map>
#include <set>
#include <functional>

namespace flash::web
{
std::string json_string(std::string_view value);
void omission(ExportReport& report, std::string message);
std::string frame_action(ByteView input);

struct ScriptAsset
{
    std::string legacy_initializer;
    std::string type;
};

struct WebSlot
{
    std::string type, value;
    bool private_access{};
};

struct WebMethod
{
    std::string function, diagnostic;
    bool private_access{};
};

struct WebClass
{
    std::string base;
    std::vector<std::string> interfaces;
    WebMethod constructor, initialize;
    std::map<std::string, WebMethod> methods, statics;
    std::map<std::string, WebSlot> slots, static_slots;
};

struct WebProgram
{
    std::map<unsigned, ScriptAsset> clips;
    std::map<std::string, WebClass, std::less<>> classes;
    std::size_t as3_bodies{}, compiled_bodies{};
};

struct WebFunction
{
    std::string parameters, body;
};

struct WebScope
{
    std::set<std::string, std::less<>> fields, methods, statics, types;
    std::string owner, package;
    bool constructor{}, static_method{};
};

WebFunction compile_as3(std::string_view source, const WebScope& scope);
std::string text_field_json(const Movie& movie, std::size_t index);
WebProgram movie_scripts(const Movie& movie, const std::filesystem::path& sources,
    ExportReport& report, std::set<std::string>* references);
std::string export_timeline(const Movie& movie, const std::filesystem::path& destination, ExportReport& report,
    std::vector<std::string>& actions, const std::map<unsigned, ScriptAsset>& scripts,
    const std::function<std::string(const Asset&)>& share_visual = {});
}
