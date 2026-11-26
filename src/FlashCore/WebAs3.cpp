#include "WebExport.hpp"
#include "ActionScript.hpp"

#include <cctype>
#include <charconv>
#include <format>

namespace flash::web
{
namespace
{
bool word_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool word_part(char c) { return word_start(c) || digit(c); }

bool in(std::string_view value, std::initializer_list<std::string_view> options)
{
    return std::ranges::find(options, value) != options.end();
}

bool browser_type(std::string_view name)
{
    return in(name, {"Sprite", "MovieClip", "Shape", "SimpleButton", "Bitmap", "BitmapData", "GlowFilter", "BlurFilter",
        "DropShadowFilter", "Timer", "Point", "Rectangle", "Matrix", "ColorTransform", "EventDispatcher",
        "TextField", "TextFormat", "StyleSheet", "Font", "ByteArray", "LocalConnection", "URLRequest", "URLRequestHeader",
        "URLVariables", "URLLoader", "Socket", "XMLSocket", "Sound", "SoundTransform", "SoundLoaderContext",
        "Loader", "LoaderContext", "ApplicationDomain", "XML", "XMLList"});
}

bool flash_event(std::string_view name)
{
    return in(name, {"Event", "MouseEvent", "KeyboardEvent", "FocusEvent", "TextEvent", "TimerEvent", "IOErrorEvent",
        "ErrorEvent", "StatusEvent", "ProgressEvent", "DataEvent", "HTTPStatusEvent", "SecurityErrorEvent", "AsyncErrorEvent"});
}

std::vector<std::string_view> scan(std::string_view source)
{
    if (source.size() > 32768)
        throw std::runtime_error("Corps ActionScript trop long pour la conversion.");
    std::vector<std::string_view> tokens;
    for (std::size_t i = 0; i < source.size();)
    {
        const auto start = i;
        const auto first = source[i++];
        if (std::isspace(static_cast<unsigned char>(first)))
            continue;
        if (source.substr(start).starts_with("//"))
        {
            i = source.find('\n', start);
            continue;
        }
        if (first == '"' || first == '\'')
        {
            while (i < source.size() && source[i] != first)
            {
                if (source[i++] == '\\' && i < source.size())
                    ++i;
            }
            if (i == source.size())
                throw std::runtime_error("Chaîne ActionScript incomplète.");
            ++i;
        }
        else if (word_start(first))
        {
            while (i < source.size() && word_part(source[i]))
                ++i;
        }
        else if (digit(first) || (first == '.' && i < source.size() && digit(source[i])))
        {
            if (first == '0' && i < source.size() && (source[i] == 'x' || source[i] == 'X'))
            {
                ++i;
                while (i < source.size() && std::isxdigit(static_cast<unsigned char>(source[i])))
                    ++i;
            }
            else
            {
                while (i < source.size() && digit(source[i]))
                    ++i;
                if (first != '.' && i < source.size() && source[i] == '.')
                {
                    ++i;
                    while (i < source.size() && digit(source[i]))
                        ++i;
                }
                if (i < source.size() && (source[i] == 'e' || source[i] == 'E'))
                {
                    ++i;
                    if (i < source.size() && (source[i] == '+' || source[i] == '-'))
                        ++i;
                    while (i < source.size() && digit(source[i]))
                        ++i;
                }
            }
        }
        else
        {
            constexpr std::string_view operators[]{"===", "!==", ">>>", "<<", ">>", "==", "!=", "<=", ">=",
                "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "%=", "|=", "&=", "^="};
            for (const auto op : operators)
                if (source.substr(start).starts_with(op))
                {
                    i = start + op.size();
                    break;
                }
        }
        tokens.push_back(source.substr(start, i - start));
    }
    return tokens;
}

std::string convert(std::string_view type, std::string value)
{
    if (type == "int") return "((" + value + ") | 0)";
    if (type == "uint") return "((" + value + ") >>> 0)";
    if (type == "String") return "clip.string(" + value + ')';
    if (type == "Number" || type == "Boolean") return std::string(type) + '(' + value + ')';
    return value;
}

std::string default_value(std::string_view type)
{
    if (type == "*") return "undefined";
    if (type == "Boolean") return "false";
    if (type == "Number") return "NaN";
    if (type == "int" || type == "uint") return "0";
    return "null";
}

unsigned precedence(std::string_view op)
{
    if (in(op, {"=", "+=", "-=", "*=", "/=", "%=", "|=", "&=", "^="})) return 1;
    if (op == "?") return 2;
    if (op == "||") return 3;
    if (op == "&&") return 4;
    if (op == "|") return 5;
    if (op == "^") return 6;
    if (op == "&") return 7;
    if (in(op, {"==", "!=", "===", "!=="})) return 8;
    if (in(op, {"<", "<=", ">", ">=", "as", "is", "in", "instanceof"})) return 9;
    if (in(op, {"<<", ">>", ">>>"})) return 10;
    if (op == "+" || op == "-") return 11;
    if (in(op, {"*", "/", "%"})) return 12;
    return 0;
}

struct Expression
{
    std::string text;
    std::string_view type;
    bool writable = false;
};

class Compiler
{
    std::vector<std::string_view> tokens_;
    const WebScope& scope_;
    std::map<std::string_view, std::string_view> locals_;
    std::size_t cursor_{};
    unsigned depth_{}, loops_{}, switches_{}, indent_{};
    bool called_super_{};
    std::string_view return_type_;
    std::string body_;

    struct Nest
    {
        unsigned& depth;
        explicit Nest(unsigned& depth) : depth(depth)
        {
            if (++depth > 64)
                throw std::runtime_error("Imbrication ActionScript trop profonde.");
        }
        ~Nest() { --depth; }
    };

    std::string_view peek(std::size_t offset = 0) const
    {
        return cursor_ + offset < tokens_.size() ? tokens_[cursor_ + offset] : std::string_view{};
    }
    std::string_view take() { const auto value = peek(); if (!value.empty()) ++cursor_; return value; }
    bool eat(std::string_view value) { if (peek() != value) return false; ++cursor_; return true; }
    [[noreturn]] void fail(std::string_view detail)
    {
        throw std::runtime_error(std::format("{} près de « {:.60} ».", detail, peek()));
    }
    void expect(std::string_view value) { if (!eat(value)) fail(std::format("Élément attendu : {}", value)); }
    std::string_view name()
    {
        const auto value = take();
        if (value.empty() || !word_start(value.front()) || in(value, {"constructor", "prototype", "__proto__"}))
            fail("Identifiant à porter");
        return value;
    }
    std::string_view type()
    {
        if (eat("*")) return "*";
        auto value = name();
        while (eat(".")) value = name();
        return value;
    }
    std::string qualified_class(std::string_view first)
    {
        std::string path(first), chosen;
        if (scope_.types.contains(path))
            chosen = path;
        if (!scope_.package.empty() && scope_.types.contains(scope_.package + '.' + path))
            chosen = scope_.package + '.' + path;
        if (first == std::string_view(scope_.owner).substr(scope_.owner.find_last_of('.') + 1))
            chosen = scope_.owner;
        std::size_t length = 0;
        for (std::size_t next = 0; peek(next) == "." && !peek(next + 1).empty() && word_start(peek(next + 1).front()); next += 2)
        {
            path += '.' + std::string(peek(next + 1));
            if (scope_.types.contains(path))
            {
                chosen = path;
                length = next + 2;
            }
        }
        cursor_ += length;
        if (chosen.empty() && !scope_.methods.contains(first) && !scope_.fields.contains(first) && !scope_.statics.contains(first))
            for (const auto& candidate : scope_.types)
                if (std::string_view(candidate).substr(candidate.find_last_of('.') + 1) == first)
                {
                    if (!chosen.empty()) return {};
                    chosen = candidate;
                }
        return chosen;
    }
    std::string new_native_name(std::string_view first)
    {
        std::string name(first);
        while (eat("."))
            name += '.' + std::string(this->name());
        return name;
    }
    void line(std::string_view value)
    {
        if (body_.size() > 131072) fail("Fonction JavaScript trop longue");
        body_ += std::string(indent_ * 4, ' ') + std::string(value) + '\n';
    }

    std::string arguments(std::string_view close)
    {
        std::string items;
        if (eat(close)) return items;
        do
        {
            if (!items.empty()) items += ", ";
            items += expression().text;
        } while (eat(","));
        expect(close);
        return items;
    }

    Expression primary()
    {
        const auto token = peek();
        if (eat("("))
        {
            auto value = expression();
            expect(")");
            value.text = '(' + value.text + ')';
            return value;
        }
        if (eat("[")) return {'[' + arguments("]") + ']'};
        if (eat("{"))
        {
            std::string literal = "({";
            if (!eat("}"))
            {
                do
                {
                    if (literal.size() > 2) literal += ", ";
                    const auto key = peek();
                    if (key.empty()) fail("Propriété d’objet attendue");
                    const auto quoted = key.front() == '"' || key.front() == '\'';
                    literal += '[' + (quoted ? std::string(take()) : json_string(name())) + "]: ";
                    expect(":");
                    literal += expression().text;
                } while (eat(","));
                expect("}");
            }
            return {literal + "})"};
        }
        if (eat("new"))
        {
            if (eat("("))
            {
                auto value = expression().text;
                expect(")");
                expect("(");
                auto args = arguments(")");
                return {"clip.construct(" + value + (args.empty() ? "" : ", " + args) + ')'};
            }
            const auto first = name();
            const auto local = first == "this" || locals_.contains(first);
            const auto known_class = local ? std::string{} : qualified_class(first);
            if (local || (known_class.empty() && (scope_.fields.contains(first) || scope_.statics.contains(first))))
            {
                auto value = first == "this" ? std::string("clip.view") : locals_.contains(first) ? "$" + std::string(first) :
                    (scope_.fields.contains(first) && !scope_.static_method ? "clip.view[" : "clip.staticView[") + json_string(first) + ']';
                for (;;)
                {
                    if (eat(".")) value += '[' + json_string(name()) + ']';
                    else if (eat("["))
                    {
                        value += '[' + expression().text + ']';
                        expect("]");
                    }
                    else break;
                }
                expect("(");
                auto args = arguments(")");
                return {"clip.construct(" + value + (args.empty() ? "" : ", " + args) + ')'};
            }
            const auto type_name = known_class.empty() ? new_native_name(first) : known_class;
            expect("(");
            if (!known_class.empty())
            {
                auto args = arguments(")");
                return {"clip.create(" + json_string(type_name) + (args.empty() ? "" : ", " + args) + ')'};
            }
            constexpr std::string_view flash_events = "flash.events.";
            const auto event = std::string_view(type_name).starts_with(flash_events) ?
                std::string_view(type_name).substr(flash_events.size()) : std::string_view(type_name);
            if (flash_event(event))
            {
                auto args = arguments(")");
                return {"clip.event(" + json_string(event) + (args.empty() ? "" : ", " + args) + ')'};
            }
            if (in(type_name, {"Array", "Object", "Date", "RegExp", "Error", "TypeError", "RangeError", "ReferenceError"}))
                return {"new " + std::string(type_name) + '(' + arguments(")") + ')'};
            const auto native = std::string_view(type_name).substr(type_name.find_last_of('.') + 1);
            if (browser_type(native) && (native == type_name || std::string_view(type_name).starts_with("flash.")))
            {
                auto args = arguments(")");
                return {"clip.native(" + json_string(native) + (args.empty() ? "" : ", " + args) + ')'};
            }
            fail(std::format("Construction de {} à porter", type_name));
        }
        if (!token.empty() && (token.front() == '"' || token.front() == '\''))
            return {std::string(take())};
        if (!token.empty() && (digit(token.front()) || token.front() == '.'))
        {
            if (token.starts_with("0x") || token.starts_with("0X"))
            {
                if (token.size() == 2) fail("Nombre hexadécimal incomplet");
            }
            else
            {
                double value{};
                const auto end = token.data() + token.size();
                const auto read = std::from_chars(token.data(), end, value);
                if (read.ec != std::errc{} || read.ptr != end || (token.size() > 1 && token.front() == '0' && digit(token[1])))
                    fail("Nombre ActionScript à porter");
            }
            return {std::string(take())};
        }
        auto id = name();
        if (in(id, {"true", "false", "null", "undefined", "NaN", "Infinity"})) return {std::string(id)};
        if (id == "this") return {"clip.self"};
        if (id == "super") return {"clip.superView"};
        if (id == "trace") return {"console.log"};
        if (id == "Array" && peek() == ".")
        {
            constexpr std::string_view flags[]{"CASEINSENSITIVE", "DESCENDING", "UNIQUESORT", "RETURNINDEXEDARRAY", "NUMERIC"};
            const auto found = std::ranges::find(flags, peek(1));
            if (found != std::end(flags))
            {
                take();
                take();
                return {std::to_string(1u << (found - std::begin(flags)))};
            }
        }
        if (in(id, {"Math", "parseInt", "parseFloat", "isNaN", "isFinite", "Array", "Boolean", "Number", "String", "Date", "RegExp",
            "encodeURI", "decodeURI", "encodeURIComponent", "decodeURIComponent", "escape", "unescape"}))
            return {std::string(id)};
        if (id == "Object")
        {
            expect("(");
            auto value = expression();
            expect(")");
            return value;
        }
        if (id == "int" || id == "uint")
        {
            if (eat("."))
            {
                const auto field = name();
                if (field == "MIN_VALUE") return {id == "int" ? "-2147483648" : "0"};
                if (field == "MAX_VALUE") return {id == "int" ? "2147483647" : "4294967295"};
                fail("Constante entière à porter");
            }
            expect("(");
            auto value = convert(id, expression().text);
            expect(")");
            return {std::move(value)};
        }
        if (const auto local = locals_.find(id); local != locals_.end())
            return {"$" + std::string(id), local->second, true};
        if (const auto known = qualified_class(id); !known.empty())
            return {"clip.type(" + json_string(known) + ')'};
        if (id == "flash")
        {
            expect(".");
            const auto package = name();
            if (!in(package, {"events", "external", "net", "utils", "display", "filters", "system", "media", "text", "geom", "ui"}))
                fail("Paquet Flash à porter");
            expect(".");
            id = name();
        }
        if (flash_event(id) && eat("."))
            return {"clip.eventName(" + json_string(name()) + ')'};
        if (in(id, {"URLRequestMethod", "URLLoaderDataFormat", "Endian", "SoundMixer", "Security", "SecurityDomain",
            "StageScaleMode", "StageAlign", "TextFieldType", "TextFieldAutoSize", "BlendMode", "Mouse", "Keyboard", "navigateToURL", "getTimer",
            "ApplicationDomain", "getDefinitionByName", "getQualifiedClassName", "Capabilities", "ExternalInterface", "SharedObject", "BitmapDataChannel"}))
            return {"clip.nativeSymbol(" + json_string(id) + ')'};
        if (in(id, {"XML", "XMLList"}) && peek() == "(")
        {
            expect("(");
            auto args = arguments(")");
            return {"clip.native(" + json_string(id) + (args.empty() ? "" : ", " + args) + ')'};
        }
        if ((browser_type(id) || flash_event(id) ||
            in(id, {"Class", "DisplayObject", "InteractiveObject", "DisplayObjectContainer", "BitmapFilter"})) && peek() == "(")
        {
            expect("(");
            auto value = expression().text;
            expect(")");
            return {"clip.cast(" + value + ", " + json_string(id) + ')'};
        }
        if (browser_type(id) || in(id, {"DisplayObject", "InteractiveObject", "DisplayObjectContainer", "BitmapFilter"}))
            return {"clip.nativeClass(" + json_string(id) + ')'};
        if (scope_.static_method && scope_.statics.contains(id))
            return {"clip.staticView[" + json_string(id) + ']', {}, true};
        if (!scope_.static_method && (scope_.fields.contains(id) || scope_.methods.contains(id) ||
            in(id, {"stage", "parent", "root", "currentFrame", "totalFrames", "addEventListener", "removeEventListener", "dispatchEvent",
                "play", "stop", "gotoAndPlay", "gotoAndStop", "nextFrame", "prevFrame", "addFrameScript", "loaderInfo", "tabChildren",
                "numChildren", "addChild", "addChildAt", "removeChild", "removeChildAt", "getChildAt", "getChildByName", "getChildIndex",
                "setChildIndex", "swapChildren", "swapChildrenAt", "contains", "graphics", "mouseX", "mouseY",
                "getBounds", "getRect", "localToGlobal", "globalToLocal",
                "cacheAsBitmap", "mouseEnabled", "mouseChildren", "buttonMode", "x", "y", "alpha", "visible",
                "width", "height", "scaleX", "scaleY", "rotation", "filters", "blendMode", "transform",
                "connect", "close", "connected", "flush", "bytesAvailable", "bytesPending", "endian", "send"})))
            return {"clip.view[" + json_string(id) + ']', {}, true};
        if (scope_.statics.contains(id))
            return {"clip.staticView[" + json_string(id) + ']', {}, true};
        fail(std::format("Référence {} à porter", id));
    }

    Expression unary()
    {
        const Nest nest(depth_);
        if (in(peek(), {"!", "-", "+", "~", "typeof", "delete"}))
        {
            const auto op = take();
            const auto value = unary();
            if (op == "delete" && (!value.writable || !value.text.ends_with(']'))) fail("Propriété à supprimer attendue");
            return {"(" + std::string(op) + " " + value.text + ')'};
        }
        auto value = primary();
        for (;;)
        {
            if (eat("."))
            {
                if (eat("."))
                {
                    const auto key = eat("*") ? "*" : name();
                    value = {value.text + ".descendants(" + json_string(key) + ')'};
                    continue;
                }
                const auto attribute = eat("@");
                const auto key = eat("*") ? "*" : name();
                if (value.text == "clip.self") value.text = "clip.view";
                if (!attribute && key == "sortOn")
                    value = {"clip.sortOn.bind(null, " + value.text + ')'};
                else
                    value = {value.text + '[' + json_string((attribute ? "@" : "") + std::string(key)) + ']', {}, true};
            }
            else if (eat("["))
            {
                value = {value.text + '[' + expression().text + ']', {}, true};
                expect("]");
            }
            else if (eat("("))
                value = {value.text + '(' + arguments(")") + ')'};
            else if (in(peek(), {"++", "--"}))
            {
                if (!value.writable) fail("Incrément invalide");
                const auto op = take();
                if (!in(value.type, {"int", "uint"}))
                    return {'(' + value.text + std::string(op) + ')'};
                const auto initial = "Number(previous)" + std::string(op == "++" ? " + 1" : " - 1");
                return {"((previous) => (" + value.text + " = " + convert(value.type, initial) + ", previous))(" + value.text + ')'};
            }
            else return value;
        }
    }

    Expression expression(unsigned minimum = 1)
    {
        const Nest nest(depth_);
        auto left = unary();
        while (const auto rank = precedence(peek()))
        {
            if (rank < minimum) break;
            const auto op = take();
            if (op == "as" || op == "is")
            {
                const auto first = name();
                if (locals_.contains(first))
                {
                    left = {"clip.dynamicType(" + left.text + ", $" + std::string(first) + ", " +
                        (op == "as" ? "true" : "false") + ')'};
                    continue;
                }
                const auto known = qualified_class(first);
                auto annotation = known.empty() ? new_native_name(first) : known;
                if (const auto dot = annotation.rfind('.'); dot != std::string::npos && known.empty())
                    annotation.erase(0, dot + 1);
                if (!in(annotation, {"Object", "Array", "Number", "String", "Boolean", "int", "uint",
                    "MovieClip", "Sprite", "SimpleButton", "DisplayObject", "InteractiveObject", "DisplayObjectContainer",
                    "TextField", "Error", "TypeError", "RangeError",
                    "ArgumentError", "Date", "RegExp", "Class", "SoundChannel", "BitmapFilter"}) && !browser_type(annotation) &&
                    !flash_event(annotation) && known.empty())
                    fail(std::format("Conversion de type {} à porter", annotation));
                left = {"clip." + std::string(op == "as" ? "asType" : "isType") + '(' + left.text + ", " + json_string(annotation) + ')'};
                continue;
            }
            if (rank == 2)
            {
                auto yes = expression().text;
                expect(":");
                left = {'(' + left.text + " ? " + yes + " : " + expression(rank).text + ')'};
                continue;
            }
            auto right = expression(rank == 1 ? rank : rank + 1);
            if (rank == 1)
            {
                if (!left.writable) fail("Affectation invalide");
                if (!left.type.empty() && op != "=") fail("Affectation composée avec coercition à porter");
                left = {'(' + left.text + " " + std::string(op) + " " + convert(left.type, std::move(right.text)) + ')'};
            }
            else
                left = {'(' + left.text + " " + std::string(op) + " " + right.text + ')'};
        }
        return left;
    }

    std::string local()
    {
        const auto key = name();
        const auto annotation = eat(":") ? type() : "*";
        locals_.insert_or_assign(key, annotation);
        return "$" + std::string(key) + " = " +
            (eat("=") ? convert(annotation, expression().text) : default_value(annotation));
    }

    void block(bool guarded = false)
    {
        const Nest nest(depth_);
        expect("{");
        line("{");
        ++indent_;
        if (guarded) line("if ((++loopCount & 4095) === 0 && performance.now() > loopDeadline) throw new Error(\"ActionScript: boucle trop longue\");");
        while (!eat("}")) statement();
        --indent_;
        line("}");
    }

    void enumeration(std::string_view key, std::string_view annotation, const std::string& items, bool each)
    {
        const auto variable = "$" + std::string(key);
        line("for (const entry of Object." + std::string(each ? "values" : "keys") + '(' + items + "))");
        expect("{");
        line("{");
        ++indent_;
        line("if ((++loopCount & 4095) === 0 && performance.now() > loopDeadline) throw new Error(\"ActionScript: boucle trop longue\");");
        line("var " + variable + " = " + convert(annotation, "entry") + ';');
        ++loops_;
        while (!eat("}")) statement();
        --loops_;
        --indent_;
        line("}");
    }

    void statement()
    {
        const auto start = peek();
        if (start.empty()) fail("Bloc ActionScript incomplet");
        if (start == "{") { block(); return; }
        if (eat(";")) return;
        if (eat("var") || eat("const"))
        {
            auto vars = local();
            while (eat(",")) vars += ", " + local();
            expect(";");
            line("var " + vars + ';');
        }
        else if (eat("if") || eat("while"))
        {
            expect("(");
            line(std::string(start) + " (" + expression().text + ')');
            expect(")");
            const auto loop = start == "while";
            loops_ += loop;
            block(loop);
            loops_ -= loop;
            if (!loop && eat("else"))
            {
                line("else");
                if (peek() == "if") statement();
                else block();
            }
        }
        else if (eat("for"))
        {
            const auto each = eat("each");
            expect("(");
            const auto checkpoint = cursor_;
            const auto declared = eat("var");
            if (declared || (!peek().empty() && word_start(peek().front()) && peek(1) == "in"))
            {
                const auto key = name();
                const auto annotation = declared ? (eat(":") ? type() : "*") :
                    (locals_.contains(key) ? locals_.at(key) : std::string_view{});
                if (eat("in"))
                {
                    if (annotation.empty()) fail("Variable d’énumération à porter");
                    const auto items = expression().text;
                    expect(")");
                    locals_.insert_or_assign(key, annotation);
                    enumeration(key, annotation, items, each);
                    return;
                }
            }
            if (each) fail("Énumération ActionScript à porter");
            cursor_ = checkpoint;
            std::string initialization;
            if (eat("var"))
            {
                initialization = "var " + local();
                while (eat(",")) initialization += ", " + local();
            }
            else if (peek() != ";")
                initialization = expression().text;
            expect(";");
            const auto test = peek() == ";" ? std::string{} : expression().text;
            expect(";");
            const auto update = peek() == ")" ? std::string{} : expression().text;
            expect(")");
            line("for (" + initialization + "; " + test + "; " + update + ')');
            ++loops_;
            block(true);
            --loops_;
        }
        else if (eat("do"))
        {
            line("do");
            ++loops_;
            block(true);
            --loops_;
            expect("while");
            expect("(");
            line("while (" + expression().text + ");");
            expect(")");
            expect(";");
        }
        else if (eat("switch"))
        {
            expect("(");
            line("switch (" + expression().text + ')');
            expect(")");
            expect("{");
            line("{");
            ++indent_;
            ++switches_;
            while (!eat("}"))
            {
                if (eat("case")) { line("case " + expression().text + ':'); expect(":"); }
                else if (eat("default")) { line("default:"); expect(":"); }
                else statement();
            }
            --switches_;
            --indent_;
            line("}");
        }
        else if (eat("return"))
        {
            if (eat(";")) line("return;");
            else
            {
                line("return " + convert(return_type_, expression().text) + ';');
                expect(";");
            }
        }
        else if (eat("throw"))
        {
            line("throw " + expression().text + ';');
            expect(";");
        }
        else if (eat("try"))
        {
            line("try");
            block();
            if (eat("catch"))
            {
                line("catch (caught)");
                line("{");
                ++indent_;
                bool first = true, catches_all = false;
                do
                {
                    if (catches_all) fail("Gestionnaire ActionScript inaccessible");
                    expect("(");
                    const auto key = name();
                    std::string annotation = "*";
                    if (eat(":") && !eat("*"))
                    {
                        const auto first_name = name();
                        annotation = qualified_class(first_name);
                        if (annotation.empty()) annotation = new_native_name(first_name);
                    }
                    expect(")");
                    catches_all = annotation == "*" || annotation == "Object";
                    const auto header = first ? "if (" : "else if (";
                    line(header + (catches_all ? std::string("true") : "clip.isType(caught, " + json_string(annotation) + ')') + ')');
                    expect("{");
                    line("{");
                    ++indent_;
                    line("let $" + std::string(key) + " = caught;");
                    const auto found = locals_.find(key);
                    const auto previous = found == locals_.end() ? std::nullopt : std::optional{found->second};
                    locals_.insert_or_assign(key, "");
                    while (!eat("}")) statement();
                    if (previous) locals_.insert_or_assign(key, *previous);
                    else locals_.erase(key);
                    --indent_;
                    line("}");
                    first = false;
                } while (eat("catch"));
                if (!catches_all) line("else { throw caught; }");
                --indent_;
                line("}");
                if (eat("finally")) { line("finally"); block(); }
            }
            else
            {
                expect("finally");
                line("finally");
                block();
            }
        }
        else if (eat("break") || eat("continue"))
        {
            if (!loops_ && (start == "continue" || !switches_)) fail("Saut ActionScript sans cible");
            expect(";");
            line(std::string(start) + ';');
        }
        else if (start == "super" && peek(1) == "(")
        {
            if (!scope_.constructor) fail("Appel de constructeur hors du constructeur");
            take();
            expect("(");
            line("clip.superInit(" + arguments(")") + ");");
            expect(";");
            called_super_ = true;
        }
        else
        {
            line(expression().text + ';');
            expect(";");
        }
    }

public:
    Compiler(std::string_view source, const WebScope& scope) : tokens_(scan(source)), scope_(scope) {}

    WebFunction run()
    {
        expect("function");
        name();
        expect("(");
        std::string parameters, coercions;
        if (!eat(")"))
        {
            do
            {
                const auto key = name();
                const auto annotation = eat(":") ? type() : "*";
                parameters += ", $" + std::string(key);
                if (eat("=")) parameters += " = " + expression().text;
                locals_.insert_or_assign(key, annotation);
                const auto input = "$" + std::string(key);
                const auto converted = convert(annotation, input);
                if (input != converted)
                    coercions += input + " = " + converted + ";\n";
            } while (eat(","));
            expect(")");
        }
        return_type_ = eat(":") ? type() : "*";
        expect("{");
        const auto write = [&]
        {
            while (!eat("}")) statement();
            if (!peek().empty()) fail("Source supplémentaire après la fonction");
        };
        write();
        if (body_.find("++loopCount") != std::string::npos)
            coercions += "let loopCount = 0;\nconst loopDeadline = performance.now() + 2500;\n";
        if (scope_.constructor && !called_super_) coercions += "clip.superInit();\n";
        return {std::move(parameters), coercions + body_};
    }
};
}

WebFunction compile_as3(std::string_view source, const WebScope& scope)
{
    if (source.find("Corps AVM2 non reconstruit.") != std::string_view::npos)
        throw std::runtime_error("Corps AVM2 non reconstruit par le décompilateur.");
    return Compiler(source, scope).run();
}
}
