#include "ActionScript.hpp"

namespace flash::script
{
std::string quote(std::string_view value, std::size_t limit)
{
    std::string text = "\"";
    const auto displayed = value.substr(0, limit);
    for (std::size_t i = 0; i < displayed.size(); ++i)
    {
        const auto byte = static_cast<unsigned char>(displayed[i]);
        if (displayed.substr(i, 3) == "\xE2\x80\xA8" || displayed.substr(i, 3) == "\xE2\x80\xA9")
        {
            text += displayed[i + 2] == '\xA8' ? "\\u2028" : "\\u2029";
            i += 2;
            continue;
        }
        switch (byte)
        {
        case '"': text += "\\\""; break;
        case '\\': text += "\\\\"; break;
        case '\n': text += "\\n"; break;
        case '\r': text += "\\r"; break;
        case '\t': text += "\\t"; break;
        default:
            if (byte < 32 || byte == 127)
                text += std::format("\\x{:02x}", byte);
            else
                text += static_cast<char>(byte);
        }
    }
    return text + (value.size() > limit ? "…\"" : "\"");
}

std::string identifier(std::string_view value, std::string_view fallback)
{
    constexpr std::string_view reserved[]{"as", "break", "case", "catch", "class", "const", "continue", "default",
        "delete", "do", "else", "extends", "false", "final", "finally", "for", "function", "if", "implements",
        "import", "in", "instanceof", "interface", "internal", "is", "namespace", "native", "new", "null", "override",
        "package", "private", "protected", "public", "return", "static", "super", "switch", "this", "throw",
        "true", "try", "typeof", "use", "var", "void", "while", "with"};
    const auto letter = [](char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$';
    };
    if (value.empty() || value.size() > 256 || !letter(value.front()))
        return std::string(fallback);
    if (std::ranges::find(reserved, value) != std::end(reserved))
        return std::string(fallback);
    for (const auto c : value)
        if (!letter(c) && !(c >= '0' && c <= '9'))
            return std::string(fallback);
    return std::string(value);
}

std::string_view Expressions::Value::ungrouped() const
{
    const auto value = std::string_view(text);
    return precedence ? value.substr(1, value.size() - 2) : value;
}

void Expressions::push(Value value)
{
    if (value.text.size() > limit_ || items_.size() >= 4096)
        throw std::runtime_error("Expression trop complexe.");
    items_.push_back(std::move(value));
}

Expressions::Value Expressions::take()
{
    if (items_.empty())
        throw std::runtime_error("Pile d’expressions incomplète.");
    auto value = std::move(items_.back());
    items_.pop_back();
    return value;
}

std::string Expressions::pop_as3_value()
{
    auto value = take();
    if (!value.precedence)
        return std::move(value.text);
    return std::string(value.ungrouped());
}

void Expressions::as3_binary(std::string_view op)
{
    // AS3 binary operators associate to the left. Keep equal-precedence right operands grouped.
    constexpr std::pair<std::string_view, unsigned> precedence[]{
        {"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5},
        {"==", 6}, {"!=", 6}, {"===", 6}, {"!==", 6},
        {"<", 7}, {"<=", 7}, {">", 7}, {">=", 7}, {"as", 7}, {"is", 7}, {"in", 7}, {"instanceof", 7},
        {"<<", 8}, {">>", 8}, {">>>", 8}, {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10}};
    const auto match = std::ranges::find(precedence, op, &std::pair<std::string_view, unsigned>::first);
    if (match == std::end(precedence))
    {
        binary(op);
        return;
    }
    const auto level = match->second;
    const auto right = take(), left = take();
    const auto first = left.precedence && *left.precedence >= level ? left.ungrouped() : std::string_view(left.text);
    const auto second = right.precedence && *right.precedence > level ? right.ungrouped() : std::string_view(right.text);
    push(Value{std::format("({} {} {})", first, op, second), level});
}

void Expressions::as3_conditional(std::string_view test, std::string_view yes, std::string_view no)
{
    push(Value{std::format("({} ? {} : {})", test, yes, no), 0});
}

std::string Expressions::arguments(unsigned count, bool reverse_pop_order, bool as3)
{
    if (count > items_.size() || count > 128)
        throw std::runtime_error("Nombre d’arguments invalide.");
    std::vector<std::string> args;
    for (unsigned i = 0; i < count; ++i)
        args.push_back(as3 ? pop_as3_value() : pop());
    if (reverse_pop_order)
        std::ranges::reverse(args);
    std::string result;
    for (const auto& arg : args)
    {
        if (!result.empty())
            result += ", ";
        result += arg;
        if (result.size() > 4096)
            throw std::runtime_error("Liste d’arguments trop complexe.");
    }
    return result;
}

std::string Expressions::as3_list(unsigned count, std::string_view open, std::string_view close)
{
    if (count > items_.size())
        throw std::runtime_error("Nombre d’éléments invalide.");
    std::vector<std::string> elements(count);
    std::size_t size = open.size() + close.size();
    for (auto item = elements.rbegin(); item != elements.rend(); ++item)
    {
        *item = pop_as3_value();
        size += item->size() + 2;
        if (size > limit_)
            throw std::runtime_error("Liste d’expressions trop grande.");
    }
    Output list(limit_);
    const bool multiline = size > 110;
    list.add(open);
    for (auto item = elements.begin(); item != elements.end(); ++item)
    {
        if (item != elements.begin())
            list.add(multiline ? "," : ", ");
        std::string_view remaining = *item;
        do
        {
            const auto newline = remaining.find('\n');
            if (multiline)
                list.add("\n    ");
            list.add(remaining.substr(0, newline));
            if (newline == std::string_view::npos)
                break;
            remaining.remove_prefix(newline + 1);
        } while (!remaining.empty());
    }
    if (multiline)
        list.add("\n");
    list.add(close);
    return std::move(list).take();
}
}

namespace flash
{
namespace
{
std::optional<Reader> button_actions(ByteView payload, bool modern)
{
    Reader button(payload);
    button.skip(2);
    if (modern)
    {
        button.skip(1);
        const auto offset_field = button.position();
        const auto offset = button.u16();
        if (!offset)
            return {};
        if (offset < 3 || offset_field + offset > payload.size())
            throw std::runtime_error("Décalage des actions du bouton invalide.");
        return Reader(payload.subspan(offset_field + offset));
    }
    while (button.u8())
    {
        button.skip(4);
        (void)read_matrix(button);
    }
    return button.remaining() && payload[button.position()] ? std::optional{button} : std::nullopt;
}

std::string render_button(ByteView payload, bool modern, bool source)
{
    auto actions = button_actions(payload, modern);
    if (!actions)
        return "// Aucun script AVM1 dans ce bouton.\n";
    if (!modern)
        return script::avm1(actions->take(actions->remaining()), source);

    script::Output output;
    while (actions->remaining())
    {
        const auto size = actions->u16();
        if (size && size < 5)
            throw std::runtime_error("Action conditionnelle du bouton tronquée.");
        Reader event(actions->take(size ? size - 2 : actions->remaining()));
        const auto flags = event.u8();
        const auto key_and_flags = event.u8();
        std::string label;
        const auto add = [&](std::string_view name)
        {
            if (!label.empty())
                label += ", ";
            label += name;
        };
        const std::string_view transitions[]{"rollOver", "rollOut", "press", "release", "dragOut", "dragOver", "releaseOutside", "menuPress"};
        for (unsigned bit = 0; bit < std::size(transitions); ++bit)
            if (flags & (1u << bit))
                add(transitions[bit]);
        if (key_and_flags & 1)
            add("menuReleaseOutside");
        if (const auto key = key_and_flags >> 1)
            add(std::format("keyPress {}", key));
        output.add(std::format("// Événement du bouton · {}\n", label));
        output.add(script::avm1(event.take(event.remaining()), source));
        output.add("\n");
        if (!size)
            return std::move(output).take();
    }
    throw std::runtime_error("Dernière action du bouton manquante.");
}

std::string render_script(const Movie& movie, std::size_t index, bool source)
{
    const auto code = movie.tags().at(index).code;
    Reader input(movie.data(index));
    switch (code)
    {
    case 7: case 34:
        return render_button(movie.data(index), code == 34, source);
    case 59:
        input.skip(2);
        [[fallthrough]];
    case 12:
        return script::avm1(input.take(input.remaining()), source);
    case 82:
        input.skip(4);
        input.cstring();
        [[fallthrough]];
    case 72:
        return script::avm2(input.take(input.remaining()), source);
    default:
        throw std::runtime_error("Ce tag ne contient pas de bloc ActionScript autonome.");
    }
}
}

bool has_avm1_actions(const Movie& movie, std::size_t index)
{
    const auto code = movie.tags().at(index).code;
    if (code == 12 || code == 59)
        return true;
    if (code != 7 && code != 34)
        return false;
    try { return button_actions(movie.data(index), code == 34).has_value(); }
    catch (const std::runtime_error&) { return false; }
}

std::string disassemble(const Movie& movie, std::size_t index) { return render_script(movie, index, false); }
std::string decompile(const Movie& movie, std::size_t index) { return render_script(movie, index, true); }
}
