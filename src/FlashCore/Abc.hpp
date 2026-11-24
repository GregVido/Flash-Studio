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
    bool interface_type = false;
    std::vector<AbcMember> members;
};

struct AbcClassSource
{
    std::string text;
    std::vector<std::size_t> member_lines;
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

private:
    struct State;
    std::shared_ptr<const State> state_;
};

ByteView abc_bytecode(const Movie& movie, std::size_t index);
Movie replace_abc(const Movie& movie, std::size_t index, ByteView abc);
}
