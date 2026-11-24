#include "ActionScript.hpp"

#include <array>
#include <charconv>

namespace flash::script
{
namespace
{
constexpr auto names = []
{
    std::array<std::string_view, 256> result{};
    result[0x04] = "NextFrame";
    result[0x05] = "PreviousFrame";
    result[0x06] = "Play";
    result[0x07] = "Stop";
    result[0x08] = "ToggleQuality";
    result[0x09] = "StopSounds";
    result[0x0A] = "Add";
    result[0x0B] = "Subtract";
    result[0x0C] = "Multiply";
    result[0x0D] = "Divide";
    result[0x0E] = "Equals";
    result[0x0F] = "Less";
    result[0x10] = "And";
    result[0x11] = "Or";
    result[0x12] = "Not";
    result[0x13] = "StringEquals";
    result[0x14] = "StringLength";
    result[0x15] = "StringExtract";
    result[0x17] = "Pop";
    result[0x18] = "ToInteger";
    result[0x1C] = "GetVariable";
    result[0x1D] = "SetVariable";
    result[0x20] = "SetTarget2";
    result[0x21] = "StringAdd";
    result[0x22] = "GetProperty";
    result[0x23] = "SetProperty";
    result[0x24] = "CloneSprite";
    result[0x25] = "RemoveSprite";
    result[0x26] = "Trace";
    result[0x27] = "StartDrag";
    result[0x28] = "EndDrag";
    result[0x29] = "StringLess";
    result[0x2A] = "Throw";
    result[0x2B] = "CastOp";
    result[0x2C] = "ImplementsOp";
    result[0x30] = "RandomNumber";
    result[0x31] = "MBStringLength";
    result[0x32] = "CharToAscii";
    result[0x33] = "AsciiToChar";
    result[0x34] = "GetTime";
    result[0x35] = "MBStringExtract";
    result[0x36] = "MBCharToAscii";
    result[0x37] = "MBAsciiToChar";
    result[0x3A] = "Delete";
    result[0x3B] = "Delete2";
    result[0x3C] = "DefineLocal";
    result[0x3D] = "CallFunction";
    result[0x3E] = "Return";
    result[0x3F] = "Modulo";
    result[0x40] = "NewObject";
    result[0x41] = "DefineLocal2";
    result[0x42] = "InitArray";
    result[0x43] = "InitObject";
    result[0x44] = "TypeOf";
    result[0x45] = "TargetPath";
    result[0x46] = "Enumerate";
    result[0x47] = "Add2";
    result[0x48] = "Less2";
    result[0x49] = "Equals2";
    result[0x4A] = "ToNumber";
    result[0x4B] = "ToString";
    result[0x4C] = "PushDuplicate";
    result[0x4D] = "StackSwap";
    result[0x4E] = "GetMember";
    result[0x4F] = "SetMember";
    result[0x50] = "Increment";
    result[0x51] = "Decrement";
    result[0x52] = "CallMethod";
    result[0x53] = "NewMethod";
    result[0x54] = "InstanceOf";
    result[0x55] = "Enumerate2";
    result[0x60] = "BitAnd";
    result[0x61] = "BitOr";
    result[0x62] = "BitXor";
    result[0x63] = "BitLShift";
    result[0x64] = "BitRShift";
    result[0x65] = "BitURShift";
    result[0x66] = "StrictEquals";
    result[0x67] = "Greater";
    result[0x68] = "StringGreater";
    result[0x69] = "Extends";
    result[0x81] = "GotoFrame";
    result[0x83] = "GetURL";
    result[0x87] = "StoreRegister";
    result[0x88] = "ConstantPool";
    result[0x8A] = "WaitForFrame";
    result[0x8B] = "SetTarget";
    result[0x8C] = "GotoLabel";
    result[0x8D] = "WaitForFrame2";
    result[0x8E] = "DefineFunction2";
    result[0x8F] = "Try";
    result[0x94] = "With";
    result[0x96] = "Push";
    result[0x99] = "Jump";
    result[0x9A] = "GetURL2";
    result[0x9B] = "DefineFunction";
    result[0x9D] = "If";
    result[0x9E] = "Call";
    result[0x9F] = "GotoFrame2";
    return result;
}();

std::string variable(std::string_view expression)
{
    if (expression.starts_with('"') && expression.ends_with('"') && expression.size() >= 2)
    {
        auto name = identifier(expression.substr(1, expression.size() - 2), "");
        if (!name.empty())
            return name;
    }
    return std::format("_root[{}]", expression);
}

std::string push_value(Reader& input, const std::vector<std::string>& constants)
{
    const auto kind = input.u8();
    switch (kind)
    {
    case 0: return quote(input.cstring());
    case 1: return std::format("{}", std::bit_cast<float>(input.u32()));
    case 2: return "null";
    case 3: return "undefined";
    case 4: return std::format("register{}", input.u8());
    case 5: return input.u8() ? "true" : "false";
    case 6:
    {
        const auto high = input.u32();
        return std::format("{}", std::bit_cast<double>((static_cast<std::uint64_t>(high) << 32) | input.u32()));
    }
    case 7: return std::format("{}", std::bit_cast<std::int32_t>(input.u32()));
    case 8: case 9:
    {
        const auto index = kind == 8 ? input.u8() : input.u16();
        if (static_cast<std::size_t>(index) >= constants.size())
            return std::format("constant_{}", index);
        return quote(constants[index]);
    }
    default: throw std::runtime_error("Type de valeur AVM1 inconnu.");
    }
}

unsigned literal_count(const std::string& text)
{
    unsigned count{};
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, count);
    if (parsed.ec != std::errc{} || parsed.ptr != end || count > 128)
        throw std::runtime_error("Nombre d’arguments ou d’éléments calculé à l’exécution.");
    return count;
}

std::string movie_property(std::string_view index)
{
    constexpr std::string_view properties[]{"_x", "_y", "_xscale", "_yscale", "_currentframe", "_totalframes", "_alpha", "_visible",
        "_width", "_height", "_rotation", "_target", "_framesloaded", "_name", "_droptarget", "_url", "_highquality",
        "_focusrect", "_soundbuftime", "_quality", "_xmouse", "_ymouse"};
    unsigned id{};
    const auto end = index.data() + index.size();
    const auto parsed = std::from_chars(index.data(), end, id);
    if (parsed.ec != std::errc{} || parsed.ptr != end || id >= std::size(properties))
        throw std::runtime_error("Propriété de MovieClip calculée à l’exécution.");
    return std::string(properties[id]);
}

void render(ByteView bytes, bool source, Output& output, unsigned depth,
    std::vector<std::string> constants = {})
{
    if (depth > 32)
        throw std::runtime_error("Trop de blocs AVM1 imbriqués.");
    Reader stream(bytes);
    Expressions stack;
    const std::string indent(depth * 4, ' ');
    auto statement = [&](std::string_view text) { output.add(indent + std::string(text) + '\n'); };
    while (stream.remaining())
    {
        const auto offset = stream.position();
        const auto op = stream.u8();
        if (!op)
        {
            if (!source)
                statement(std::format("{:04X}  End", offset));
            return;
        }
        Reader args(op >= 128 ? stream.take(stream.u16()) : ByteView{});
        std::string operands;
        ByteView nested;
        unsigned flags = 0, scene_bias = 0;
        if (op == 0x88)
        {
            constants.clear();
            const auto count = args.u16();
            for (unsigned i = 0; i < count; ++i)
                constants.push_back(args.cstring());
            operands = std::format("{} chaînes", count);
            if (!source)
                for (unsigned i = 0; i < count && i < 256; ++i)
                    operands += std::format("\n{}      [{}] {}", indent, i, quote(constants[i]));
        }
        else if (op == 0x96)
        {
            while (args.remaining())
            {
                auto value = push_value(args, constants);
                if (source)
                    stack.push(std::move(value));
                else
                {
                    if (operands.size() > 8192)
                    {
                        operands += ", …";
                        break;
                    }
                    operands += (operands.empty() ? "" : ", ") + value;
                }
            }
        }
        else if (op == 0x8E || op == 0x9B)
        {
            const auto name = args.cstring();
            const auto params = args.u16();
            if (op == 0x8E)
                args.skip(3);
            std::string signature = identifier(name, "fonction_anonyme") + '(';
            for (unsigned i = 0; i < params; ++i)
            {
                if (op == 0x8E)
                    args.skip(1);
                if (i)
                    signature += ", ";
                signature += identifier(args.cstring(), std::format("arg{}", i + 1));
                if (signature.size() > 8192)
                    throw std::runtime_error("Signature AVM1 trop longue.");
            }
            operands = signature + ')';
            const auto size = args.u16();
            nested = args.remaining() >= size ? args.take(size) : stream.take(size);
        }
        else if (op == 0x94)
        {
            const auto size = args.u16();
            nested = args.remaining() >= size ? args.take(size) : stream.take(size);
        }
        else if (op == 0x99 || op == 0x9D)
            operands = std::format("→ {:04X}", static_cast<std::int64_t>(stream.position()) +
                std::bit_cast<std::int16_t>(args.u16()));
        else if (op == 0x81)
            operands = std::format("{}", args.u16());
        else if (op == 0x87)
            operands = std::format("register{}", args.u8());
        else if (op == 0x8B || op == 0x8C)
            operands = quote(args.cstring());
        else if (op == 0x83)
        {
            operands = quote(args.cstring());
            operands += ", " + quote(args.cstring());
        }
        else if (op == 0x9A || op == 0x9F)
        {
            flags = args.u8();
            if (op == 0x9A)
                operands = std::format("méthode {}, cible {}, variables {}", flags >> 6, (flags & 2) != 0, (flags & 1) != 0);
            else
            {
                if (flags & 2)
                    scene_bias = args.u16();
                operands = std::format("{} · décalage de scène {}", flags & 1 ? "lecture" : "arrêt", scene_bias);
            }
        }
        else if (op == 0x8A)
        {
            const auto frame = args.u16();
            operands = std::format("image {}, ignorer {} action(s)", frame, args.u8());
        }
        else if (op == 0x8D)
            operands = std::format("ignorer {} action(s)", args.u8());
        else if (args.remaining())
        {
            const auto raw = args.take((std::min)(args.remaining(), std::size_t{24}));
            for (const auto byte : raw)
                operands += std::format("{:02X} ", byte);
            if (args.remaining())
                operands += "…";
        }
        if (!source)
        {
            const auto name = names[op];
            statement(std::format("{:04X}  {}{}{}", offset, name.empty() ? std::format("0x{:02X}", op) : std::string(name),
                operands.empty() ? "" : "  ", operands));
            if (!nested.empty())
                render(nested, false, output, depth + 1, constants);
            continue;
        }

        switch (op)
        {
        case 0x04: statement("nextFrame();"); break;
        case 0x05: statement("prevFrame();"); break;
        case 0x06: statement("play();"); break;
        case 0x07: statement("stop();"); break;
        case 0x08: statement("toggleHighQuality();"); break;
        case 0x09: statement("stopAllSounds();"); break;
        case 0x0A: case 0x21: case 0x47: stack.binary("+"); break;
        case 0x0B: stack.binary("-"); break;
        case 0x0C: stack.binary("*"); break;
        case 0x0D: stack.binary("/"); break;
        case 0x0E: case 0x13: case 0x49: stack.binary("=="); break;
        case 0x0F: case 0x29: case 0x48: stack.binary("<"); break;
        case 0x10: stack.binary("&&"); break;
        case 0x11: stack.binary("||"); break;
        case 0x12: stack.push("!(" + stack.pop() + ')'); break;
        case 0x14: case 0x31:
            stack.push(std::string(op == 0x14 ? "length(" : "mblength(") + stack.pop() + ')');
            break;
        case 0x15: case 0x35:
        {
            const auto count = stack.pop(), start = stack.pop(), value = stack.pop();
            stack.push(std::format("{}({}, {}, {})", op == 0x15 ? "substring" : "mbsubstring", value, start, count));
            break;
        }
        case 0x17: statement(stack.pop() + ';'); break;
        case 0x18: stack.push("int(" + stack.pop() + ')'); break;
        case 0x1C: stack.push(variable(stack.pop())); break;
        case 0x1D: case 0x3C:
        {
            const auto value = stack.pop();
            const auto name = variable(stack.pop());
            statement(std::format("{}{} = {};", op == 0x3C ? "var " : "", name, value));
            break;
        }
        case 0x22: case 0x23:
        {
            const auto value = op == 0x23 ? stack.pop() : std::string{};
            const auto property = movie_property(stack.pop());
            const auto target = stack.pop();
            const auto object = target == "\"\"" ? "this" : "eval(" + target + ')';
            if (op == 0x23)
                statement(object + '.' + property + " = " + value + ';');
            else
                stack.push(object + '.' + property);
            break;
        }
        case 0x24:
        {
            const auto level = stack.pop(), name = stack.pop(), target = stack.pop();
            statement(std::format("duplicateMovieClip({}, {}, {});", target, name, level));
            break;
        }
        case 0x25: statement("removeMovieClip(" + stack.pop() + ");"); break;
        case 0x26: statement("trace(" + stack.pop() + ");"); break;
        case 0x28: statement("stopDrag();"); break;
        case 0x2A: statement("throw " + stack.pop() + ';'); break;
        case 0x2B:
        {
            const auto value = stack.pop(), type = stack.pop();
            stack.push(type + '(' + value + ')');
            break;
        }
        case 0x30: stack.push("random(" + stack.pop() + ')'); break;
        case 0x32: case 0x33: case 0x36: case 0x37:
            stack.push(std::string(op == 0x32 ? "ord(" : op == 0x33 ? "chr(" : op == 0x36 ? "mbord(" : "mbchr(") + stack.pop() + ')');
            break;
        case 0x34: stack.push("getTimer()"); break;
        case 0x3A:
        {
            const auto key = stack.pop(), object = stack.pop();
            stack.push("delete " + object + '[' + key + ']');
            break;
        }
        case 0x3B: stack.push("delete " + variable(stack.pop())); break;
        case 0x3D: case 0x40:
        {
            const auto function = variable(stack.pop());
            const auto argument_count = literal_count(stack.pop());
            stack.push(std::format("{}{}({})", op == 0x40 ? "new " : "", function,
                stack.arguments(argument_count, false)));
            break;
        }
        case 0x3E: statement("return " + stack.pop() + ';'); break;
        case 0x3F: stack.binary("%"); break;
        case 0x41: statement("var " + variable(stack.pop()) + ';'); break;
        case 0x42:
        {
            const auto count = literal_count(stack.pop());
            stack.push('[' + stack.arguments(count, false) + ']');
            break;
        }
        case 0x43:
        {
            const auto count = literal_count(stack.pop());
            std::vector<std::string> properties;
            for (unsigned i = 0; i < count; ++i)
            {
                const auto value = stack.pop(), key = stack.pop();
                if (!key.starts_with('"'))
                    throw std::runtime_error("Clé d’objet calculée à l’exécution.");
                properties.push_back(key + ": " + value);
            }
            std::string object = "{";
            for (auto item = properties.rbegin(); item != properties.rend(); ++item)
                object += (item == properties.rbegin() ? "" : ", ") + *item;
            stack.push(object + '}');
            break;
        }
        case 0x44: stack.push("typeof (" + stack.pop() + ')'); break;
        case 0x45: stack.push("targetPath(" + stack.pop() + ')'); break;
        case 0x4A: stack.push("Number(" + stack.pop() + ')'); break;
        case 0x4B: stack.push("String(" + stack.pop() + ')'); break;
        case 0x4C: stack.duplicate(); break;
        case 0x4D:
        {
            auto right = stack.pop(), left = stack.pop();
            stack.push(std::move(right));
            stack.push(std::move(left));
            break;
        }
        case 0x50: stack.push("(Number(" + stack.pop() + ") + 1)"); break;
        case 0x51: stack.push("(Number(" + stack.pop() + ") - 1)"); break;
        case 0x52: case 0x53:
        {
            const auto member = stack.pop(), object = stack.pop();
            const auto count = literal_count(stack.pop());
            auto callable = member == "\"\"" || member == "undefined" ? object : object + '[' + member + ']';
            stack.push(std::format("{}{}({})", op == 0x53 ? "new " : "", callable, stack.arguments(count, false)));
            break;
        }
        case 0x54: stack.binary("instanceof"); break;
        case 0x60: stack.binary("&"); break;
        case 0x61: stack.binary("|"); break;
        case 0x62: stack.binary("^"); break;
        case 0x63: stack.binary("<<"); break;
        case 0x64: stack.binary(">>"); break;
        case 0x65: stack.binary(">>>"); break;
        case 0x4E: case 0x4F:
        {
            const auto value = op == 0x4F ? stack.pop() : std::string{};
            const auto member = stack.pop();
            const auto object = stack.pop();
            auto property = std::format("{}[{}]", object, member);
            if (op == 0x4E)
                stack.push(std::move(property));
            else
                statement(property + " = " + value + ';');
            break;
        }
        case 0x66: stack.binary("==="); break;
        case 0x67: case 0x68: stack.binary(">"); break;
        case 0x81: statement(std::format("gotoAndStop({});", std::stoul(operands) + 1)); break;
        case 0x83: statement("getURL(" + operands + ");"); break;
        case 0x87:
            stack.duplicate();
            statement(operands + " = " + stack.pop() + ';');
            break;
        case 0x88: case 0x96: break;
        case 0x8C: statement("gotoAndStop(" + operands + ");"); break;
        case 0x8E: case 0x9B:
            statement("function " + operands + " {");
            try { render(nested, true, output, depth + 1, constants); }
            catch (const std::runtime_error& error)
            {
                statement(std::format("    // Décompilation interrompue : {}", error.what()));
            }
            statement("}");
            break;
        case 0x94:
            statement("with (" + stack.pop() + ") {");
            render(nested, true, output, depth + 1, constants);
            statement("}");
            break;
        case 0x9A:
        {
            const auto target = stack.pop(), url = stack.pop();
            const char* name = flags & 1 ? (flags & 2 ? "loadVariables" : "loadVariablesNum") : flags & 2 ? "loadMovie" : "getURL";
            const auto method = flags >> 6;
            statement(std::format("{}({}, {}{});", name, url, target, method == 1 ? ", \"GET\"" : method == 2 ? ", \"POST\"" : ""));
            break;
        }
        case 0x9E: statement("call(" + stack.pop() + ");"); break;
        case 0x9F:
        {
            auto frame = stack.pop();
            if (scene_bias)
                frame = std::format("(Number({}) + {})", frame, scene_bias);
            statement(std::format("{}({});", flags & 1 ? "gotoAndPlay" : "gotoAndStop", frame));
            break;
        }
        default:
            throw std::runtime_error(std::format("Instruction {} à 0x{:04X}. Voir le désassemblage.",
                names[op].empty() ? std::format("0x{:02X}", op) : std::string(names[op]), offset));
        }
    }
}
}

std::string avm1(ByteView bytes, bool source)
{
    Output output;
    output.add(source ? "// ActionScript 1/2 · reconstruction partielle\n\n" : "AVM1 · instructions\n\n");
    try { render(bytes, source, output, 0); }
    catch (const std::runtime_error& error) { output.add(std::format("\n// Lecture interrompue : {}\n", error.what())); }
    return std::move(output).take();
}
}
