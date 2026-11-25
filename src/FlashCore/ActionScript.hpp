#pragma once
#include "Binary.hpp"

#include <format>

namespace flash::script
{
std::string avm1(ByteView bytes, bool source);
std::string avm2(ByteView bytes, bool source);
std::string quote(std::string_view value, std::size_t limit = 1024);
std::string identifier(std::string_view value, std::string_view fallback);

class Output
{
public:
    explicit Output(std::size_t limit = max_text_size) : limit_(limit) {}
    void add(std::string_view text)
    {
        if (text.size() > limit_ - value_.size())
            throw std::runtime_error("Le texte produit dépasse la limite d’export.");
        value_ += text;
    }
    std::string take() && { return std::move(value_); }
private:
    std::size_t limit_;
    std::string value_;
};

class Expressions
{
public:
    explicit Expressions(std::size_t limit = 4096) : limit_(limit) {}
    bool empty() const noexcept { return items_.empty(); }
    std::size_t size() const noexcept { return items_.size(); }
    void push(std::string value)
    {
        push(Value{std::move(value), {}});
    }
    std::string pop()
    {
        return take().text;
    }
    std::string pop_as3_value();
    void duplicate()
    {
        if (items_.empty())
            throw std::runtime_error("Pile d’expressions incomplète.");
        push(Value(items_.back()));
    }
    void binary(std::string_view op)
    {
        const auto right = pop(), left = pop();
        push(std::format("({} {} {})", left, op, right));
    }
    void as3_binary(std::string_view op);
    void as3_conditional(std::string_view test, std::string_view yes, std::string_view no);
    std::string arguments(unsigned count, bool reverse_pop_order, bool as3 = false);
    std::string as3_list(unsigned count, std::string_view open, std::string_view close);
private:
    struct Value
    {
        std::string text;
        std::optional<unsigned> precedence;
        std::string_view ungrouped() const;
    };
    void push(Value value);
    Value take();
    std::size_t limit_;
    std::vector<Value> items_;
};
}
