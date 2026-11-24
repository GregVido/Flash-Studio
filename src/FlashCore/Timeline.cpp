#include "Binary.hpp"

#include <array>
#include <cmath>
#include <format>
#include <map>
#include <set>

namespace flash
{
namespace
{
struct Position
{
    double a = 1, b = 0, c = 0, d = 1, x = 0, y = 0;
};

Position operator*(const Position& l, const Position& r)
{
    return {l.a * r.a + l.c * r.b, l.b * r.a + l.d * r.b,
        l.a * r.c + l.c * r.d, l.b * r.c + l.d * r.d,
        l.a * r.x + l.c * r.y + l.x, l.b * r.x + l.d * r.y + l.y};
}

Position position(Matrix m) { return {m.scale_x, m.skew_y, m.skew_x, m.scale_y, static_cast<double>(m.x), static_cast<double>(m.y)}; }

std::string transform(const Position& p)
{
    return std::format("matrix({:g} {:g} {:g} {:g} {:g} {:g})", p.a, p.b, p.c, p.d, p.x, p.y);
}

struct Bounds
{
    double x1{}, y1{}, x2{}, y2{};
    bool valid = false;

    bool operator==(const Bounds&) const = default;

    void add(double x, double y)
    {
        if (!std::isfinite(x) || !std::isfinite(y))
            throw std::runtime_error("Transformation de scène hors limites.");
        if (!valid)
            *this = {x, y, x, y, true};
        x1 = std::min(x1, x);
        y1 = std::min(y1, y);
        x2 = std::max(x2, x);
        y2 = std::max(y2, y);
    }

    void add(const Bounds& shape, const Position& p)
    {
        for (const auto x : {shape.x1, shape.x2})
            for (const auto y : {shape.y1, shape.y2})
                add(p.a * x + p.c * y + p.x, p.b * x + p.d * y + p.y);
    }
};

struct Instance
{
    unsigned id{}, placed{}, clip_end{};
    Position transform;
    double opacity = 1.0;
    bool unsupported = false, visible = true;
};

struct Display
{
    std::map<unsigned, Instance> layers;
    std::optional<std::array<unsigned, 3>> background;
};

double color_alpha(Reader& input, bool alpha)
{
    BitReader bits(input);
    const auto has_addition = bits.unsigned_bits(1), has_multiplier = bits.unsigned_bits(1);
    const auto width = bits.unsigned_bits(4);
    double multiplier = 1.0, addition = 0.0;
    if (has_multiplier)
    {
        for (int channel = 0; channel < 3; ++channel)
            bits.signed_bits(width);
        if (alpha)
            multiplier = bits.signed_bits(width) / 256.0;
    }
    if (has_addition)
    {
        for (int channel = 0; channel < 3; ++channel)
            bits.signed_bits(width);
        if (alpha)
            addition = bits.signed_bits(width) / 255.0;
    }
    return std::clamp(multiplier + addition, 0.0, 1.0);
}

void skip_filters(Reader& input)
{
    const auto count = input.u8();
    for (unsigned i = 0; i < count; ++i)
        switch (input.u8())
        {
        case 0: input.skip(23); break;
        case 1: input.skip(9); break;
        case 2: input.skip(15); break;
        case 3: input.skip(27); break;
        case 4: case 7:
        {
            const auto colors = input.u8();
            input.skip(5 * colors + 19);
            break;
        }
        case 5:
        {
            const auto columns = input.u8(), rows = input.u8();
            input.skip(4 * columns * rows + 13);
            break;
        }
        case 6: input.skip(80); break;
        default: throw std::runtime_error("Filtre SWF inconnu.");
        }
}

void place_object(Display& display, unsigned code, ByteView payload, unsigned frame)
{
    Reader input(payload);
    if (code == 4)
    {
        Instance item;
        item.id = input.u16();
        const auto depth = input.u16();
        item.placed = frame;
        item.transform = position(read_matrix(input));
        if (input.remaining())
            color_alpha(input, false);
        display.layers[depth] = item;
        return;
    }

    const auto flags = input.u8();
    const auto extra = code == 70 || code == 94 ? input.u8() : 0;
    const auto depth = input.u16();
    const auto previous = display.layers.find(depth);
    const auto move = (flags & 1) != 0;
    Instance item = move && previous != display.layers.end() ? previous->second : Instance{};
    if ((extra & 8) || ((extra & 16) && (flags & 2)))
        input.cstring();
    if (flags & 2)
    {
        item.id = input.u16();
        item.placed = frame;
    }
    if (flags & 4)
        item.transform = position(read_matrix(input));
    if (flags & 8)
        item.opacity = color_alpha(input, true);
    if (flags & 16)
        input.skip(2);
    if (flags & 32)
        input.cstring();
    if (flags & 64)
        item.clip_end = input.u16();
    if (!item.id)
        return;
    if (extra & 32)
    {
        if (extra & 1)
            skip_filters(input);
        if (extra & 2)
            input.skip(1);
        if (extra & 4)
            input.skip(1);
        item.visible = input.u8() != 0;
    }
    if (extra & 67)
        item.unsupported = true;
    display.layers[depth] = item;
}

Display display_at(const Movie& movie, std::optional<std::size_t> sprite, unsigned frame)
{
    Display display;
    unsigned current = 1;
    for (std::size_t i = sprite ? *sprite + 1 : 0; i < movie.tags().size(); i = movie.tags()[i].next_sibling)
    {
        const auto code = movie.tags()[i].code;
        switch (code)
        {
        case 0: return display;
        case 1: if (current++ == frame) return display; break;
        case 4: case 26: case 70: case 94:
            place_object(display, code, movie.data(i), current);
            break;
        case 5: case 28:
        {
            Reader input(movie.data(i));
            if (code == 5)
                input.skip(2);
            display.layers.erase(input.u16());
            break;
        }
        case 9:
        {
            Reader input(movie.data(i));
            display.background = {input.u8(), input.u8(), input.u8()};
            break;
        }
        default: break;
        }
    }
    return display;
}

struct Shape
{
    Bounds bounds;
    std::string definition;
    std::string paths;
};

class Scene
{
public:
    explicit Scene(const Movie& movie) : movie_(movie) {}
    FramePreview render(unsigned frame, std::optional<std::size_t> sprite, Bounds* viewport = nullptr);
    Bounds measure(unsigned frame, std::size_t sprite)
    {
        draw(frame, sprite);
        return bounds_;
    }
private:
    Display draw(unsigned frame, std::optional<std::size_t> sprite);
    const Shape* shape(std::size_t index);
    void layers(const Display& display, unsigned frame, const Position& parent, unsigned depth);
    void add(std::string& output, std::string_view piece);

    const Movie& movie_;
    std::map<std::size_t, std::optional<Shape>> shapes_;
    std::map<std::pair<std::size_t, unsigned>, Display> sprites_;
    std::set<std::size_t> defined_;
    std::size_t shape_bytes_ = 0, cached_layers_ = 0;
    Bounds bounds_;
    std::string definitions_, objects_;
    unsigned omissions_ = 0, visited_ = 0, mask_ = 0;
};

void Scene::add(std::string& output, std::string_view piece)
{
    constexpr std::size_t max_svg_size = 16 * 1024 * 1024;
    if (piece.size() > max_svg_size - definitions_.size() - objects_.size())
        throw std::runtime_error("L’image SVG de cette scène dépasse 16 Mio.");
    output += piece;
}

const Shape* Scene::shape(std::size_t index)
{
    auto [found, created] = shapes_.try_emplace(index);
    if (created)
    {
        try
        {
            const auto code = movie_.tags()[index].code;
            if (movie_.tags()[index].category == Category::images)
            {
                const auto bitmap = embedded_bitmap(movie_, index);
                const auto width = bitmap.width * 20.0, height = bitmap.height * 20.0;
                const auto id = std::format("image-tag-{}", index);
                const auto definition = std::format("<image id=\"{}\" width=\"{:g}\" height=\"{:g}\" "
                    "xlink:href=\"{}\"/>\n", id, width, height, bitmap.data_uri);
                found->second = Shape{{0, 0, width, height, true}, definition,
                    std::format("<use xlink:href=\"#{}\"/>\n", id)};
            }
            else
            {
                const auto asset = code == 11 || code == 33 ? std::optional{text_svg(movie_, index)} : extract(movie_, index);
                if (!asset || asset->extension != L".svg")
                    return nullptr;
                Reader input(movie_.data(index));
                input.skip(2);
                BitReader bits(input);
                const auto width = bits.unsigned_bits(5);
                const double x1 = bits.signed_bits(width), x2 = bits.signed_bits(width);
                const double y1 = bits.signed_bits(width), y2 = bits.signed_bits(width);
                std::string_view svg(reinterpret_cast<const char*>(asset->bytes.data()), asset->bytes.size());
                svg = svg.substr(svg.find('>') + 1);
                svg = svg.substr(0, svg.rfind("</svg>"));
                std::string definition;
                if (const auto end = svg.find("</defs>"); end != std::string_view::npos)
                {
                    const auto start = svg.find("<defs>") + 6;
                    definition = svg.substr(start, end - start);
                    svg.remove_prefix(end + 7);
                }
                found->second = Shape{{x1, y1, x2, y2, true}, std::move(definition), std::string(svg)};
            }
            if (const auto& item = found->second)
                shape_bytes_ += item->definition.size() + item->paths.size();
        }
        catch (const std::runtime_error&)
        {
            // An unsupported definition does not prevent the other layers from rendering.
            found->second.reset();
        }
    }
    if (const auto& item = found->second; item && !item->definition.empty() && defined_.insert(index).second)
        add(definitions_, item->definition);
    return found->second ? &*found->second : nullptr;
}

void Scene::layers(const Display& display, unsigned frame, const Position& parent, unsigned depth)
{
    if (depth > 24)
    {
        ++omissions_;
        return;
    }
    struct Clip { unsigned end, id; };
    std::vector<Clip> clips;
    for (const auto& [at, item] : display.layers)
    {
        if (!item.visible)
            continue;
        if (++visited_ > 12'000)
            throw std::runtime_error("Trop d’instances dans cette image.");
        std::erase_if(clips, [at](const Clip& clip) { return clip.end < at; });
        const auto index = movie_.character(item.id);
        if (!index || item.opacity == 0)
            continue;
        const auto code = movie_.tags()[*index].code;
        const auto vector = code == 2 || code == 22 || code == 32 || code == 83 || code == 11 || code == 33 ||
            movie_.tags()[*index].category == Category::images;
        const auto outline = vector ? shape(*index) : nullptr;
        if (item.clip_end > at)
        {
            if (outline)
            {
                const auto id = ++mask_;
                add(definitions_, std::format("<clipPath id=\"mask-{}\"><g transform=\"{}\">", id, transform(item.transform)));
                add(definitions_, outline->paths);
                add(definitions_, "</g></clipPath>\n");
                clips.push_back({item.clip_end, id});
            }
            else
                ++omissions_;
            continue;
        }
        if (!outline && code != 39)
        {
            ++omissions_;
            continue;
        }
        if (item.unsupported)
            ++omissions_;
        for (const auto& clip : clips)
            add(objects_, std::format("<g clip-path=\"url(#mask-{})\">", clip.id));
        add(objects_, std::format("<g transform=\"{}\" opacity=\"{:g}\">\n", transform(item.transform), item.opacity));
        const auto world = parent * item.transform;
        if (outline)
        {
            bounds_.add(outline->bounds, world);
            add(objects_, outline->paths);
        }
        else
        {
            const auto count = std::max(1u, flash::frame_count(movie_, *index));
            const auto local = (frame - item.placed) % count + 1;
            auto [child, created] = sprites_.try_emplace({*index, local});
            if (created)
            {
                child->second = display_at(movie_, *index, local);
                cached_layers_ += child->second.layers.size();
            }
            layers(child->second, local, world, depth + 1);
        }
        add(objects_, "</g>\n");
        for (std::size_t i = 0; i < clips.size(); ++i)
            add(objects_, "</g>");
    }
}

Display Scene::draw(unsigned frame, std::optional<std::size_t> sprite)
{
    if (!frame || frame > std::max(1u, flash::frame_count(movie_, sprite)))
        throw std::runtime_error("Numéro d’image hors limites.");
    if (shape_bytes_ > 24 * 1024 * 1024 || shapes_.size() > 512)
    {
        shapes_.clear();
        shape_bytes_ = 0;
    }
    if (cached_layers_ > 32'000 || sprites_.size() > 256)
    {
        sprites_.clear();
        cached_layers_ = 0;
    }
    defined_.clear();
    definitions_.clear();
    objects_.clear();
    bounds_ = {};
    omissions_ = visited_ = mask_ = 0;
    auto display = display_at(movie_, sprite, frame);
    layers(display, frame, {}, 0);
    if (!sprite)
    {
        const auto& h = movie_.header();
        bounds_ = {static_cast<double>(h.left), static_cast<double>(h.top),
            static_cast<double>(h.right), static_cast<double>(h.bottom), true};
    }
    return display;
}

FramePreview Scene::render(unsigned frame, std::optional<std::size_t> sprite, Bounds* viewport)
{
    const auto display = draw(frame, sprite);
    bool viewport_changed = false;
    if (sprite && viewport)
    {
        const auto previous = *viewport;
        if (bounds_.valid)
        {
            viewport->add(bounds_.x1, bounds_.y1);
            viewport->add(bounds_.x2, bounds_.y2);
        }
        viewport_changed = *viewport != previous;
        bounds_ = *viewport;
    }
    const auto width = std::max(20.0, bounds_.x2 - bounds_.x1), height = std::max(20.0, bounds_.y2 - bounds_.y1);
    const auto scale = std::min({1.0, 4096.0 * 20 / width, 4096.0 * 20 / height});
    auto svg = std::format("<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "width=\"{:g}\" height=\"{:g}\" "
        "viewBox=\"{:g} {:g} {:g} {:g}\">\n", width * scale / 20, height * scale / 20,
        bounds_.x1, bounds_.y1, width, height);
    if (!definitions_.empty())
        svg += "<defs>\n" + definitions_ + "</defs>\n";
    if (!sprite && display.background)
    {
        const auto color = *display.background;
        svg += std::format("<rect x=\"{:g}\" y=\"{:g}\" width=\"{:g}\" height=\"{:g}\" fill=\"#{:02X}{:02X}{:02X}\"/>\n",
            bounds_.x1, bounds_.y1, width, height, color[0], color[1], color[2]);
    }
    svg += objects_;
    svg += "</svg>\n";
    return {{L".svg", Bytes(svg.begin(), svg.end())}, omissions_, viewport_changed};
}
}

struct TimelinePreview::State
{
    State(std::shared_ptr<const Movie> movie, std::optional<std::size_t> sprite)
        : movie(std::move(movie)), scene(*this->movie), sprite(sprite) {}

    std::shared_ptr<const Movie> movie;
    Scene scene;
    std::optional<std::size_t> sprite;
    Bounds viewport;
    bool primed = false;
};

TimelinePreview::TimelinePreview(std::shared_ptr<const Movie> movie, std::optional<std::size_t> sprite)
    : state_(std::make_unique<State>(std::move(movie), sprite)) {}

TimelinePreview::~TimelinePreview() = default;

FramePreview TimelinePreview::render(unsigned frame)
{
    if (!state_->primed && state_->sprite)
    {
        state_->primed = true;
        const auto count = frame_count(*state_->movie, state_->sprite);
        if (count > 1 && count <= 256)
        {
            const std::set samples{1u, (count + 1) / 2, count};
            for (const auto sample : samples)
                if (sample != frame)
                    try { state_->scene.render(sample, state_->sprite, &state_->viewport); }
                    catch (const std::runtime_error&) {}
        }
    }
    return state_->scene.render(frame, state_->sprite, &state_->viewport);
}

unsigned frame_count(const Movie& movie, std::optional<std::size_t> sprite)
{
    if (!sprite)
        return movie.header().frame_count;
    if (movie.tags().at(*sprite).code != 39)
        throw std::runtime_error("Le tag sélectionné n’est pas un sprite.");
    return Reader(movie.data(*sprite).subspan(2)).u16();
}

double playback_fps(const Movie& movie) noexcept
{
    const auto fps = movie.header().frames_per_second;
    return fps > 0 ? fps : 24.0;
}

FramePreview render_frame(const Movie& movie, unsigned frame, std::optional<std::size_t> sprite)
{
    return Scene(movie).render(frame, sprite);
}

void render_sprite_frames(const Movie& movie, std::size_t sprite, const std::function<void(unsigned, FramePreview)>& accept)
{
    const auto count = (std::max)(1u, frame_count(movie, sprite));
    Scene scene(movie);
    Bounds viewport;
    for (unsigned i = 1; i <= count; ++i)
    {
        const auto bounds = scene.measure(i, sprite);
        if (bounds.valid)
        {
            viewport.add(bounds.x1, bounds.y1);
            viewport.add(bounds.x2, bounds.y2);
        }
    }
    for (unsigned i = 1; i <= count; ++i)
        accept(i, scene.render(i, sprite, &viewport));
}
}
