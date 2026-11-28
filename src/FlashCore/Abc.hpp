#pragma once

#include "Swf.hpp"

#include <memory>

namespace flash
{
struct AbcMethod
{
    unsigned id{};
    std::string owner, name, signature;
    std::optional<std::size_t> code_size;
    std::size_t handlers{};
};

struct AbcMember
{
    std::string name, declaration;
    std::optional<unsigned> method;
};

struct AbcClass
{
    std::string package, name, qualified, declaration;
    std::string base;
    std::vector<std::string> interfaces;
    bool interface_type = false;
    bool sealed = false;
    std::vector<AbcMember> members;
};

struct AbcClassSource
{
    struct MemberRange
    {
        std::size_t first_line = 0;
        std::size_t past_last_line = 0;
    };

    std::string text;
    std::vector<MemberRange> members;
};

struct DeobfuscationReport
{
    std::size_t changed_methods{}, removed_instructions{}, constant_branches{}, skipped_methods{}, resolved_strings{},
        renamed_identifiers{};
};

struct As3Obfuscation
{
    std::size_t methods{}, identifiers{};
};

struct AbcDeobfuscation
{
    Bytes bytes;
    DeobfuscationReport report;
};

struct SwfDeobfuscation
{
    std::optional<Movie> movie;
    DeobfuscationReport report;
};

class AbcModule
{
public:
    explicit AbcModule(ByteView abc);

    unsigned major_version() const noexcept;
    unsigned minor_version() const noexcept;
    const std::vector<AbcMethod>& methods() const noexcept;
    const std::vector<AbcClass>& classes() const noexcept;
    const std::vector<std::string>& strings() const noexcept;
    std::string decompile(unsigned method) const;
    AbcClassSource decompile_class(std::size_t index) const;
    std::string disassemble(unsigned method) const;
    std::string pcode(unsigned method, bool simplify = false) const;
    std::string flow_graph(unsigned method) const;
    std::optional<std::string> source_problem(unsigned method) const;
    bool declares_classes(unsigned method) const;
    Bytes replace_method(unsigned method, std::string_view pcode) const;
    Bytes replace_string(unsigned index, std::string_view utf8) const;
    std::size_t obfuscated_methods() const;
    AbcDeobfuscation deobfuscate(std::optional<std::size_t> type = {}) const;

private:
    struct State;
    std::shared_ptr<const State> state_;
};

ByteView abc_bytecode(const Movie& movie, std::size_t index);
Movie replace_abc(const Movie& movie, std::size_t index, ByteView abc);
As3Obfuscation detect_as3_obfuscation(const Movie& movie);
SwfDeobfuscation deobfuscate_as3(const Movie& movie, std::optional<std::size_t> tag = {},
    std::optional<std::size_t> type = {});
SwfDeobfuscation simplify_as3_strings(const Movie& movie, std::optional<std::size_t> tag = {},
    std::optional<std::size_t> type = {});
SwfDeobfuscation rename_as3_identifiers(const Movie& movie);
}
