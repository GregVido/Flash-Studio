#include "Avm1Code.hpp"
#include "WebExport.hpp"
#include "ActionScript.hpp"

#include <cmath>
#include <map>
#include <set>

namespace flash::web
{
namespace
{
std::string number(double value)
{
    if (std::isnan(value)) return "NaN";
    if (!std::isfinite(value)) return value < 0 ? "-Infinity" : "Infinity";
    return std::format("{:.17g}", value);
}

std::string read_value(Reader& args)
{
    switch (args.u8())
    {
    case 0: return json_string(args.cstring());
    case 1: return number(std::bit_cast<float>(args.u32()));
    case 2: return "null";
    case 3: return "undefined";
    case 4: return std::format("e.registers[{}]", args.u8());
    case 5: return args.u8() ? "true" : "false";
    case 6:
    {
        const auto high = args.u32();
        return number(std::bit_cast<double>((std::uint64_t{high} << 32) | args.u32()));
    }
    case 7: return std::to_string(std::bit_cast<std::int32_t>(args.u32()));
    case 8: return std::format("e.constants[{}]", args.u8());
    case 9: return std::format("e.constants[{}]", args.u16());
    default: throw std::runtime_error("Type de valeur AVM1 inconnu.");
    }
}

std::string compile(ByteView bytes, unsigned depth);

std::string instruction(const script::Avm1Action& action, unsigned depth)
{
    Reader args(action.args);
    const auto binary = [](std::string_view op, std::string_view cast = "")
    {
        return std::format("{{ const b = s.pop(), a = s.pop(); s.push({}(a) {} {}(b)); }}\n", cast, op, cast);
    };
    switch (action.code)
    {
    case 0x04: return "e.timeline(\"nextFrame\");\n";
    case 0x05: return "e.timeline(\"prevFrame\");\n";
    case 0x06: return "e.timeline(\"play\");\n";
    case 0x07: return "e.timeline(\"stop\");\n";
    case 0x08: return "e.toggleQuality();\n";
    case 0x09: return "e.timeline(\"stopSounds\");\n";
    case 0x0A: return binary("+", "Number");
    case 0x0B: return binary("-", "Number");
    case 0x0C: return binary("*", "Number");
    case 0x0D: return binary("/", "Number");
    case 0x0E: return binary("===", "Number");
    case 0x0F: return binary("<", "Number");
    case 0x10: return binary("&&", "Boolean");
    case 0x11: return binary("||", "Boolean");
    case 0x12: return "s.push(!s.pop());\n";
    case 0x13: return binary("===", "String");
    case 0x14: case 0x31: return "s.push(String(s.pop()).length);\n";
    case 0x15: case 0x35: return "{ const n = s.pop(), i = s.pop(); s.push(String(s.pop()).substr(Math.max(0, i - 1), n)); }\n";
    case 0x17: return "s.pop();\n";
    case 0x18: return "s.push(s.pop() | 0);\n";
    case 0x1C: return "s.push(e.variable(s.pop()));\n";
    case 0x1D: return "{ const v = s.pop(); e.assign(s.pop(), v); }\n";
    case 0x20: return "e.setTarget(s.pop());\n";
    case 0x21: return binary("+", "String");
    case 0x22: return "{ const i = s.pop(); s.push(e.property(s.pop(), i)); }\n";
    case 0x23: return "{ const v = s.pop(), i = s.pop(); e.setProperty(s.pop(), i, v); }\n";
    case 0x24: return "{ const d = s.pop(), n = s.pop(); e.clone(s.pop(), n, d); }\n";
    case 0x25: return "e.remove(s.pop());\n";
    case 0x26: return "console.log(s.pop());\n";
    case 0x29: return binary("<", "String");
    case 0x2A: return "throw s.pop();\n";
    case 0x30: return "s.push(Math.floor(Math.random() * Math.max(0, s.pop() | 0)));\n";
    case 0x32: case 0x36: return "s.push(String(s.pop()).charCodeAt(0) || 0);\n";
    case 0x33: case 0x37: return "s.push(String.fromCharCode(s.pop()));\n";
    case 0x34: return "s.push(e.timeline(\"elapsed\"));\n";
    case 0x3A: return "{ const k = s.pop(); s.push(e.removeMember(s.pop(), k)); }\n";
    case 0x3B: return "s.push(e.removeVariable(s.pop()));\n";
    case 0x3C: return "{ const v = s.pop(); e.define(s.pop(), v); }\n";
    case 0x3D: case 0x40: return std::format("{{ const n = s.pop(); s.push(e.callFunction(n, s, {})); }}\n",
        action.code == 0x40 ? "true" : "false");
    case 0x3E: return "return s.pop();\n";
    case 0x3F: return binary("%", "Number");
    case 0x41: return "e.define(s.pop(), undefined);\n";
    case 0x42: return "s.push(e.arguments(s));\n";
    case 0x43: return "s.push(e.object(s));\n";
    case 0x44: return "s.push(e.typeOf(s.pop()));\n";
    case 0x45: return "s.push(e.path(s.pop()));\n";
    case 0x47: return binary("+");
    case 0x48: return binary("<");
    case 0x49: return binary("==");
    case 0x4A: return "s.push(Number(s.pop()));\n";
    case 0x4B: return "s.push(String(s.pop()));\n";
    case 0x4C: return "s.push(s.at(-1));\n";
    case 0x4D: return "{ const b = s.pop(), a = s.pop(); s.push(b, a); }\n";
    case 0x4E: return "{ const k = s.pop(); s.push(e.member(s.pop(), k)); }\n";
    case 0x4F: return "{ const v = s.pop(), k = s.pop(); e.setMember(s.pop(), k, v); }\n";
    case 0x50: return "s.push(Number(s.pop()) + 1);\n";
    case 0x51: return "s.push(Number(s.pop()) - 1);\n";
    case 0x52: case 0x53: return std::format("{{ const k = s.pop(), o = s.pop(); s.push(e.callMethod(o, k, s, {})); }}\n",
        action.code == 0x53 ? "true" : "false");
    case 0x54: return "{ const b = s.pop(), a = s.pop(); s.push(typeof b === \"function\" && a instanceof b); }\n";
    case 0x60: return binary("&");
    case 0x61: return binary("|");
    case 0x62: return binary("^");
    case 0x63: return binary("<<");
    case 0x64: return binary(">>");
    case 0x65: return binary(">>>");
    case 0x66: return binary("===");
    case 0x67: return binary(">");
    case 0x68: return binary(">", "String");
    case 0x69: return "{ const base = s.pop(); e.extend(s.pop(), base); }\n";
    case 0x81: return std::format("e.timeline(\"seek\", {});\n", unsigned{args.u16()} + 1);
    case 0x83:
    {
        const auto url = args.cstring();
        return std::format("e.navigate({}, {});\n", json_string(url), json_string(args.cstring()));
    }
    case 0x87: return std::format("e.registers[{}] = s.at(-1);\n", args.u8());
    case 0x88:
    {
        std::string result = "e.constants = [";
        const auto count = args.u16();
        for (unsigned i = 0; i < count; ++i)
        {
            if (i) result += ", ";
            result += json_string(args.cstring());
        }
        return result + "];\n";
    }
    case 0x8B: return "e.setTarget(" + json_string(args.cstring()) + ");\n";
    case 0x8C: return "e.timeline(\"seek\", " + json_string(args.cstring()) + ");\n";
    case 0x8E: case 0x9B:
    {
        const auto& fn = *action.function;
        std::string parameters = "[";
        for (const auto& [name, reg] : fn.parameters)
        {
            if (parameters.size() > 1) parameters += ", ";
            parameters += std::format("[{}, {}]", json_string(name), reg);
        }
        parameters += ']';
        auto value = std::format("e.function({}, {}, {}, e => {{\n{}}})",
            parameters, fn.flags, action.code == 0x8E ? "true" : "false", compile(fn.body, depth + 1));
        if (fn.name.empty()) return "s.push(" + value + ");\n";
        return "e.define(" + json_string(fn.name) + ", " + value + ");\n";
    }
    case 0x96:
    {
        std::string result;
        while (args.remaining())
            result += "s.push(" + read_value(args) + ");\n";
        return result;
    }
    case 0x9A:
    {
        const auto flags = args.u8();
        if (flags & 0xC3)
            throw std::runtime_error("Chargement externe AS1/AS2 à porter.");
        return "{ const target = s.pop(); e.navigate(s.pop(), target); }\n";
    }
    case 0x9E: return "e.callFrame(s.pop());\n";
    case 0x9F:
    {
        const auto flags = args.u8();
        const auto bias = flags & 2 ? args.u16() : 0;
        return std::format("e.go(s.pop(), {}, {});\n", flags & 1 ? "true" : "false", bias);
    }
    default:
        throw std::runtime_error(std::format("Instruction ActionScript 1/2 0x{:02X} à porter.", action.code));
    }
}

std::string compile(ByteView bytes, unsigned depth)
{
    if (depth > 32)
        throw std::runtime_error("Trop de fonctions AVM1 imbriquées.");
    const auto actions = script::avm1_actions(bytes);
    if (actions.empty() || !actions.front().code)
        return {};
    const auto end = actions.back().code ? actions.back().next : actions.back().offset;
    std::map<std::size_t, std::size_t> indices{{end, actions.size()}};
    for (std::size_t i = 0; i < actions.size() && actions[i].code; ++i)
        indices.emplace(actions[i].offset, i);
    std::set<std::size_t> blocks{0, end};
    std::map<std::size_t, std::size_t> branches;
    for (std::size_t i = 0; i < actions.size() && actions[i].code; ++i)
    {
        const auto& item = actions[i];
        std::int64_t target;
        if (item.code == 0x99 || item.code == 0x9D)
            target = script::avm1_branch(item);
        else if (item.code == 0x8A || item.code == 0x8D)
        {
            Reader args(item.args);
            if (item.code == 0x8A) args.skip(2);
            const auto next = i + 1 + args.u8();
            target = next < actions.size() ? actions[next].offset : end;
        }
        else continue;
        if (target < 0 || !indices.contains(static_cast<std::size_t>(target)))
            throw std::runtime_error(std::format("Saut AVM1 vers une adresse invalide à 0x{:04X}.", item.offset));
        const auto address = static_cast<std::size_t>(target);
        blocks.insert(address);
        blocks.insert(item.next);
        branches.emplace(item.offset, address);
    }
    script::Output output(8 * 1024 * 1024);
    output.add("const s = [];\n");
    if (branches.empty())
    {
        for (const auto& item : actions)
            if (item.code) output.add(instruction(item, depth));
        return std::move(output).take();
    }
    // A JavaScript case is emitted per basic block. The browser never reads or decodes AVM1 bytecode.
    output.add("let block = 0;\nwhile (e.step()) {\nswitch (block) {\n");
    for (auto start = blocks.begin(), next = std::next(start); next != blocks.end(); ++start, ++next)
    {
        output.add(std::format("case {}:\n", *start));
        bool terminal = false;
        for (auto i = indices.at(*start); i < actions.size() && actions[i].offset < *next; ++i)
        {
            const auto& item = actions[i];
            if (const auto found = branches.find(item.offset); found != branches.end())
            {
                if (item.code == 0x99)
                    output.add(std::format("block = {};\nbreak;\n", found->second));
                else
                {
                    auto test = item.code == 0x9D ? std::string("s.pop()") :
                        item.code == 0x8A ? std::format("e.frameAvailable({})", unsigned{Reader(item.args).u16()} + 1) :
                        std::string("e.frameAvailable(s.pop())");
                    const auto taken = item.code == 0x9D ? found->second : item.next;
                    const auto not_taken = item.code == 0x9D ? item.next : found->second;
                    output.add(std::format("block = {} ? {} : {};\nbreak;\n", test, taken, not_taken));
                }
                terminal = true;
            }
            else
            {
                output.add(instruction(item, depth));
                terminal = item.code == 0x3E || item.code == 0x2A;
            }
        }
        if (!terminal)
        {
            if (*next == end) output.add("return;\n");
            else output.add(std::format("block = {};\nbreak;\n", *next));
        }
    }
    output.add("default: return;\n}\n}\n");
    return std::move(output).take();
}
}

std::string frame_action(ByteView input)
{
    auto body = compile(input, 0);
    return body.empty() ? std::string{} : "{\nconst e = clip.legacyContext();\n" + body + "}\n";
}
}
