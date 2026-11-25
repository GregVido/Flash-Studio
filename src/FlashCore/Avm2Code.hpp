#pragma once
#include "Binary.hpp"

#include <array>

namespace flash::script
{
struct Opcode { unsigned value; std::string_view name; unsigned count; bool unknown_operands = false; };
inline constexpr auto opcodes = []
{
    constexpr Opcode defined[]{
        {0x01, "bkpt", 0}, {0x02, "nop", 0}, {0x03, "throw", 0},
        {0x04, "getsuper", 1}, {0x05, "setsuper", 1}, {0x06, "dxns", 1},
        {0x07, "dxnslate", 0}, {0x08, "kill", 1}, {0x09, "label", 0},
        {0x0A, "lf32x4", 0}, {0x0B, "sf32x4", 0}, {0x0C, "ifnlt", 1},
        {0x0D, "ifnle", 1}, {0x0E, "ifngt", 1}, {0x0F, "ifnge", 1},
        {0x10, "jump", 1}, {0x11, "iftrue", 1}, {0x12, "iffalse", 1},
        {0x13, "ifeq", 1}, {0x14, "ifne", 1}, {0x15, "iflt", 1},
        {0x16, "ifle", 1}, {0x17, "ifgt", 1}, {0x18, "ifge", 1},
        {0x19, "ifstricteq", 1}, {0x1A, "ifstrictne", 1}, {0x1B, "lookupswitch", 2},
        {0x1C, "pushwith", 0}, {0x1D, "popscope", 0}, {0x1E, "nextname", 0},
        {0x1F, "hasnext", 0}, {0x20, "pushnull", 0}, {0x21, "pushundefined", 0},
        {0x22, "pushfloat", 1}, {0x23, "nextvalue", 0}, {0x24, "pushbyte", 1},
        {0x25, "pushshort", 1}, {0x26, "pushtrue", 0}, {0x27, "pushfalse", 0},
        {0x28, "pushnan", 0}, {0x29, "pop", 0}, {0x2A, "dup", 0},
        {0x2B, "swap", 0}, {0x2C, "pushstring", 1}, {0x2D, "pushint", 1},
        {0x2E, "pushuint", 1}, {0x2F, "pushdouble", 1}, {0x30, "pushscope", 0},
        {0x31, "pushnamespace", 1}, {0x32, "hasnext2", 2}, {0x35, "li8", 0},
        {0x36, "li16", 0}, {0x37, "li32", 0}, {0x38, "lf32", 0},
        {0x39, "lf64", 0}, {0x3A, "si8", 0}, {0x3B, "si16", 0},
        {0x3C, "si32", 0}, {0x3D, "sf32", 0}, {0x3E, "sf64", 0},
        {0x40, "newfunction", 1}, {0x41, "call", 1}, {0x42, "construct", 1},
        {0x43, "callmethod", 2}, {0x44, "callstatic", 2}, {0x45, "callsuper", 2},
        {0x46, "callproperty", 2}, {0x47, "returnvoid", 0}, {0x48, "returnvalue", 0},
        {0x49, "constructsuper", 1}, {0x4A, "constructprop", 2}, {0x4C, "callproplex", 2},
        {0x4B, "callsuperid", 0, true}, {0x4D, "callinterface", 1},
        {0x4E, "callsupervoid", 2}, {0x4F, "callpropvoid", 2}, {0x50, "sxi1", 0},
        {0x51, "sxi8", 0}, {0x52, "sxi16", 0}, {0x53, "applytype", 1},
        {0x54, "pushfloat4", 1}, {0x55, "newobject", 1}, {0x56, "newarray", 1},
        {0x57, "newactivation", 0}, {0x58, "newclass", 1}, {0x59, "getdescendants", 1},
        {0x5A, "newcatch", 1}, {0x5B, "deldescendants", 1}, {0x5D, "findpropstrict", 1}, {0x5E, "findproperty", 1},
        {0x5F, "finddef", 1}, {0x60, "getlex", 1}, {0x61, "setproperty", 1},
        {0x62, "getlocal", 1}, {0x63, "setlocal", 1}, {0x64, "getglobalscope", 0},
        {0x65, "getscopeobject", 1}, {0x66, "getproperty", 1}, {0x67, "getouterscope", 1},
        {0x68, "initproperty", 1}, {0x69, "setpropertylate", 0, true}, {0x6A, "deleteproperty", 1},
        {0x6B, "deletepropertylate", 0}, {0x6C, "getslot", 1},
        {0x6D, "setslot", 1}, {0x6E, "getglobalslot", 1}, {0x6F, "setglobalslot", 1},
        {0x70, "convert_s", 0}, {0x71, "esc_xelem", 0}, {0x72, "esc_xattr", 0},
        {0x73, "convert_i", 0}, {0x74, "convert_u", 0}, {0x75, "convert_d", 0},
        {0x76, "convert_b", 0}, {0x77, "convert_o", 0}, {0x78, "checkfilter", 0},
        {0x79, "convert_f", 0}, {0x7A, "unplus", 0}, {0x7B, "convert_f4", 0},
        {0x80, "coerce", 1}, {0x81, "coerce_b", 0}, {0x82, "coerce_a", 0},
        {0x83, "coerce_i", 0}, {0x84, "coerce_d", 0}, {0x85, "coerce_s", 0},
        {0x86, "astype", 1}, {0x87, "astypelate", 0}, {0x88, "coerce_u", 0},
        {0x89, "coerce_o", 0}, {0x90, "negate", 0}, {0x91, "increment", 0},
        {0x92, "inclocal", 1}, {0x93, "decrement", 0}, {0x94, "declocal", 1},
        {0x95, "typeof", 0}, {0x96, "not", 0}, {0x97, "bitnot", 0},
        {0xA0, "add", 0}, {0xA1, "subtract", 0}, {0xA2, "multiply", 0},
        {0xA3, "divide", 0}, {0xA4, "modulo", 0}, {0xA5, "lshift", 0},
        {0xA6, "rshift", 0}, {0xA7, "urshift", 0}, {0xA8, "bitand", 0},
        {0xA9, "bitor", 0}, {0xAA, "bitxor", 0}, {0xAB, "equals", 0},
        {0xAC, "strictequals", 0}, {0xAD, "lessthan", 0}, {0xAE, "lessequals", 0},
        {0xAF, "greaterthan", 0}, {0xB0, "greaterequals", 0}, {0xB1, "instanceof", 0},
        {0xB2, "istype", 1}, {0xB3, "istypelate", 0}, {0xB4, "in", 0},
        {0xC0, "increment_i", 0}, {0xC1, "decrement_i", 0}, {0xC2, "inclocal_i", 1},
        {0xC3, "declocal_i", 1}, {0xC4, "negate_i", 0}, {0xC5, "add_i", 0},
        {0xC6, "subtract_i", 0}, {0xC7, "multiply_i", 0}, {0xD0, "getlocal0", 0},
        {0xD1, "getlocal1", 0}, {0xD2, "getlocal2", 0}, {0xD3, "getlocal3", 0},
        {0xD4, "setlocal0", 0}, {0xD5, "setlocal1", 0}, {0xD6, "setlocal2", 0},
        {0xD7, "setlocal3", 0}, {0xED, "invalid", 0, true}, {0xEE, "abs_jump", 0, true},
        {0xEF, "debug", 4}, {0xF0, "debugline", 1},
        {0xF1, "debugfile", 1}, {0xF2, "bkptline", 1}, {0xF3, "timestamp", 0},
        {0xF5, "verifypass", 0, true}, {0xF6, "alloc", 0, true}, {0xF7, "mark", 0, true},
        {0xF8, "wb", 0, true}, {0xF9, "prologue", 0, true}, {0xFA, "sendenter", 0, true},
        {0xFB, "doubletoatom", 0, true}, {0xFC, "sweep", 0, true}, {0xFD, "codegenop", 0, true},
        {0xFE, "verifyop", 0, true}, {0xFF, "decode", 0, true},
    };
    std::array<Opcode, 256> result{};
    for (const auto& opcode : defined)
        result[opcode.value] = opcode;
    return result;
}();

struct Instruction
{
    std::size_t offset{};
    unsigned op{};
    std::vector<std::int64_t> args;
};

struct ExceptionEntry
{
    unsigned from, to, target, type, variable;
};

struct AssembledMethod
{
    unsigned stack{}, locals{}, initial_scope{}, max_scope{};
    Bytes code;
    std::vector<ExceptionEntry> exceptions;
};

struct FlowChanges
{
    std::size_t removed{}, redirected{}, constant_branches{}, constants{};
};

Instruction decode(Reader& code);
FlowChanges simplify_flow(std::vector<Instruction>& instructions, std::size_t end,
    std::span<const ExceptionEntry> exceptions = {});
bool has_boolean_prelude(ByteView code, unsigned first_local, unsigned locals);
bool has_discarded_closure_prelude(ByteView code);
void simplify_source_flow(std::vector<Instruction>& code, std::size_t end,
    unsigned first_local, unsigned locals);
FlowChanges deobfuscate_flow(std::vector<Instruction>& instructions, std::size_t end,
    std::span<const ExceptionEntry> exceptions, unsigned first_local, unsigned locals);
AssembledMethod assemble_method(std::string_view text);
void put_u30(Bytes& bytes, unsigned value);
}
