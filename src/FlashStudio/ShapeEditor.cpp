#include "UiStyle.hpp"
#include "../FlashCore/ShapeEditing.hpp"

#include <cmath>
#include <set>
#include <winrt/Microsoft.Web.WebView2.Core.h>

namespace studio
{
using namespace ui;
using namespace Windows::Data::Json;
using namespace Microsoft::Web::WebView2::Core;

namespace
{
constexpr std::wstring_view host = L"vector.flashstudio.local";
constexpr std::wstring_view url = L"https://vector.flashstudio.local/shape-editor.html";

void number(const JsonObject& object, std::wstring_view key, double value)
{
    object.SetNamedValue(hstring(key), JsonValue::CreateNumberValue(value));
}

void text(const JsonObject& object, std::wstring_view key, std::wstring_view value)
{
    object.SetNamedValue(hstring(key), JsonValue::CreateStringValue(hstring(value)));
}

JsonArray numbers(std::initializer_list<double> values)
{
    JsonArray result;
    for (const auto value : values)
        result.Append(JsonValue::CreateNumberValue(value));
    return result;
}

JsonArray point(flash::ShapePoint value) { return numbers({double(value.x), double(value.y)}); }
JsonArray color_value(const flash::ShapeColor& color) { return numbers({double(color[0]), double(color[1]), double(color[2]), double(color[3])}); }

JsonObject paint_value(const flash::ShapePaint& paint)
{
    JsonObject result;
    number(result, L"type", paint.type);
    result.SetNamedValue(L"color", color_value(paint.color));
    result.SetNamedValue(L"matrix", numbers({paint.matrix.scale_x, paint.matrix.skew_y, paint.matrix.skew_x, paint.matrix.scale_y,
        double(paint.matrix.x), double(paint.matrix.y)}));
    number(result, L"flags", paint.gradient_flags);
    number(result, L"focal", paint.focal);
    number(result, L"bitmap", paint.bitmap);
    JsonArray stops;
    for (const auto& stop : paint.stops)
    {
        JsonObject value;
        number(value, L"ratio", stop.ratio);
        value.SetNamedValue(L"color", color_value(stop.color));
        stops.Append(value);
    }
    result.SetNamedValue(L"stops", stops);
    return result;
}

hstring serialize_drawing(const flash::Movie& movie, const flash::ShapeDrawing& drawing, std::wstring_view title, bool french)
{
    JsonObject result;
    text(result, L"action", L"open");
    text(result, L"locale", french ? L"fr" : L"en");
    text(result, L"title", title);
    number(result, L"version", drawing.version);
    number(result, L"flags", drawing.flags);
    const auto& b = drawing.bounds;
    result.SetNamedValue(L"bounds", numbers({double(b[0]), double(b[1]), double(b[2]), double(b[3])}));
    JsonArray fills, strokes, edges;
    std::set<unsigned> bitmaps;
    const auto collect_bitmap = [&](const flash::ShapePaint& paint)
    {
        if (paint.type >= 0x40 && paint.type <= 0x43)
            bitmaps.insert(paint.bitmap);
    };
    for (const auto& fill : drawing.fills)
    {
        fills.Append(paint_value(fill));
        collect_bitmap(fill);
    }
    for (const auto& line : drawing.strokes)
    {
        JsonObject value;
        number(value, L"width", line.width);
        number(value, L"flags", line.flags);
        number(value, L"miter", line.miter);
        value.SetNamedValue(L"paint", paint_value(line.paint));
        strokes.Append(value);
        collect_bitmap(line.paint);
    }
    for (const auto& edge : drawing.edges)
    {
        JsonArray value;
        value.Append(point(edge.from));
        value.Append(point(edge.to));
        if (edge.control)
            value.Append(point(*edge.control));
        else
            value.Append(JsonValue::CreateNullValue());
        for (const auto item : {edge.fill0, edge.fill1, edge.stroke, edge.layer})
            value.Append(JsonValue::CreateNumberValue(item));
        edges.Append(value);
    }
    JsonObject textures;
    std::size_t embedded_size = 0;
    for (unsigned bitmap : bitmaps)
        if (const auto index = movie.character(bitmap))
        {
            try
            {
                const auto source = flash::embedded_bitmap(movie, *index);
                if (source.data_uri.size() > 8 * 1024 * 1024 - embedded_size)
                    continue;
                embedded_size += source.data_uri.size();
                JsonObject value;
                text(value, L"src", widen(source.data_uri));
                number(value, L"width", source.width);
                number(value, L"height", source.height);
                textures.SetNamedValue(to_hstring(bitmap), value);
            }
            catch (const std::runtime_error&) {}
        }
    result.SetNamedValue(L"fills", fills);
    result.SetNamedValue(L"strokes", strokes);
    result.SetNamedValue(L"edges", edges);
    result.SetNamedValue(L"textures", textures);
    return result.Stringify();
}

[[noreturn]] void invalid_drawing()
{
    throw std::runtime_error("Les données du dessin sont invalides.");
}

int integer(double value, int minimum, int maximum)
{
    if (!(value >= minimum && value <= maximum) || std::trunc(value) != value)
        invalid_drawing();
    return static_cast<int>(value);
}

flash::ShapePoint read_point(const JsonArray& value)
{
    if (value.Size() != 2)
        invalid_drawing();
    constexpr auto limit = flash::shape_coordinate_limit;
    return {integer(value.GetNumberAt(0), -limit, limit), integer(value.GetNumberAt(1), -limit, limit)};
}

flash::ShapeColor read_color(const JsonArray& value)
{
    if (value.Size() != 4)
        invalid_drawing();
    flash::ShapeColor color{};
    for (unsigned i = 0; i < color.size(); ++i)
        color[i] = static_cast<std::uint8_t>(integer(value.GetNumberAt(i), 0, 255));
    return color;
}

flash::ShapePaint decode_paint(const JsonObject& value)
{
    flash::ShapePaint paint;
    paint.type = static_cast<std::uint8_t>(integer(value.GetNamedNumber(L"type"), 0, 255));
    paint.color = read_color(value.GetNamedArray(L"color"));
    paint.gradient_flags = static_cast<std::uint8_t>(integer(value.GetNamedNumber(L"flags"), 0, 240));
    paint.focal = static_cast<std::int16_t>(integer(value.GetNamedNumber(L"focal"), INT16_MIN, INT16_MAX));
    paint.bitmap = static_cast<std::uint16_t>(integer(value.GetNamedNumber(L"bitmap"), 0, UINT16_MAX));
    const auto matrix = value.GetNamedArray(L"matrix");
    if (matrix.Size() != 6)
        invalid_drawing();
    paint.matrix = {matrix.GetNumberAt(0), matrix.GetNumberAt(3), matrix.GetNumberAt(2), matrix.GetNumberAt(1),
        integer(matrix.GetNumberAt(4), -flash::shape_coordinate_limit, flash::shape_coordinate_limit),
        integer(matrix.GetNumberAt(5), -flash::shape_coordinate_limit, flash::shape_coordinate_limit)};
    const auto stops = value.GetNamedArray(L"stops");
    if (stops.Size() > 15)
        invalid_drawing();
    for (unsigned i = 0; i < stops.Size(); ++i)
    {
        const auto item = stops.GetObjectAt(i);
        const auto ratio = static_cast<std::uint8_t>(integer(item.GetNamedNumber(L"ratio"), 0, 255));
        if (!paint.stops.empty() && paint.stops.back().ratio > ratio)
            invalid_drawing();
        paint.stops.push_back({ratio, read_color(item.GetNamedArray(L"color"))});
    }
    return paint;
}

flash::ShapeDrawing decode_draft(const flash::Movie& movie, std::size_t index, const hstring& message)
{
    if (message.size() > 12 * 1024 * 1024)
        invalid_drawing();
    const auto meta = flash::read_shape_drawing(movie.tags()[index].code, movie.data(index));
    flash::ShapeDrawing draft;
    draft.version = meta.version;
    draft.character = meta.character;
    draft.flags = meta.flags;
    const auto data = JsonObject::Parse(message);
    const auto fills = data.GetNamedArray(L"fills"), strokes = data.GetNamedArray(L"strokes"), edges = data.GetNamedArray(L"edges");
    if (!fills.Size() || !strokes.Size() || fills.Size() > 65536 || strokes.Size() > 65536 || edges.Size() > flash::shape_edge_limit)
        invalid_drawing();
    draft.fills.reserve(fills.Size());
    draft.strokes.reserve(strokes.Size());
    draft.edges.reserve(edges.Size());
    for (unsigned i = 1; i < fills.Size(); ++i)
        draft.fills.push_back(decode_paint(fills.GetObjectAt(i)));
    for (unsigned i = 1; i < strokes.Size(); ++i)
    {
        const auto line = strokes.GetObjectAt(i);
        draft.strokes.push_back({
            static_cast<std::uint16_t>(integer(line.GetNamedNumber(L"width"), 0, UINT16_MAX)),
            static_cast<std::uint16_t>(integer(line.GetNamedNumber(L"flags"), 0, UINT16_MAX)),
            static_cast<std::uint16_t>(integer(line.GetNamedNumber(L"miter"), 0, UINT16_MAX)),
            decode_paint(line.GetNamedObject(L"paint"))});
    }
    for (const auto& value : edges)
    {
        const auto edge = value.GetArray();
        if (edge.Size() != 7)
            invalid_drawing();
        const auto control = edge.GetAt(2);
        draft.edges.push_back({
            read_point(edge.GetArrayAt(0)), read_point(edge.GetArrayAt(1)),
            control.ValueType() == JsonValueType::Null ? std::nullopt : std::optional{read_point(control.GetArray())},
            static_cast<unsigned>(integer(edge.GetNumberAt(3), 0, static_cast<int>(draft.fills.size() - 1))),
            static_cast<unsigned>(integer(edge.GetNumberAt(4), 0, static_cast<int>(draft.fills.size() - 1))),
            static_cast<unsigned>(integer(edge.GetNumberAt(5), 0, static_cast<int>(draft.strokes.size() - 1))),
            static_cast<unsigned>(integer(edge.GetNumberAt(6), 0, 65535))});
    }
    return draft;
}
}

struct Workspace::ShapeEdit
{
    std::shared_ptr<const flash::Movie> movie;
    std::filesystem::path original;
    std::size_t tag{};
    Window window{nullptr};
    WebView2 web{nullptr};
    hstring initial;
    bool saving = false;
    bool closed = false;
    bool dirty = false;
    bool closing = false;

    void feedback(std::wstring_view message, bool error = false) const
    {
        if (closed)
            return;
        try
        {
            JsonObject state;
            text(state, L"action", L"state");
            text(state, L"message", message);
            state.SetNamedValue(L"error", JsonValue::CreateBooleanValue(error));
            state.SetNamedValue(L"saving", JsonValue::CreateBooleanValue(saving));
            web.CoreWebView2().PostWebMessageAsJson(state.Stringify());
        }
        catch (const hresult_error&) {}
    }
};

void Workspace::close_shape_editor()
{
    if (const auto session = shape_session_; session && session->window && !session->closed)
    {
        session->closing = true;
        session->window.Close();
    }
}

fire_and_forget Workspace::edit_shape()
{
    if (closed_ || busy_ || !movie_ || !selected_ || !flash::is_editable_shape(movie_->tags()[*selected_].code))
        co_return;
    if (shape_session_)
    {
        if (shape_session_->window)
            shape_session_->window.Activate();
        co_return;
    }
    const auto lifetime = shared_from_this();
    auto session = std::make_shared<ShapeEdit>();
    session->movie = movie_;
    session->original = file_;
    session->tag = *selected_;
    shape_session_ = session;
    const auto& resource = movie_->tags()[*selected_];
    const auto title = std::wstring(tr(L"Forme")) + L" #" + std::to_wstring(resource.character_id.value_or(0));
    const auto french = language() == Language::french;
    const apartment_context ui;
    std::wstring error;
    co_await resume_background();
    try
    {
        const auto drawing = flash::read_shape_drawing(resource.code, session->movie->data(session->tag));
        if (session->movie->tags()[session->tag].character_id != drawing.character)
            throw std::runtime_error("Identifiant de la forme SWF invalide.");
        session->initial = serialize_drawing(*session->movie, drawing, title, french);
    }
    catch (const hresult_error& problem) { error = problem.message(); }
    catch (const std::exception& problem) { error = widen(problem.what()); }
    co_await ui;
    if (closed_)
        co_return;
    if (!error.empty())
    {
        shape_session_.reset();
        status(error, true);
        co_return;
    }
    try
    {
        const auto assets = editor_assets();
        session->window = Window();
        session->window.Title(L"Flash Studio — " + title);
        session->window.AppWindow().Resize({1120, 740});
        Grid pane;
        pane.RequestedTheme(ElementTheme::Dark);
        pane.Background(brush(base));
        auto loading = label(tr(L"Chargement de l’éditeur de formes…"), 15);
        loading.HorizontalAlignment(HorizontalAlignment::Center);
        loading.VerticalAlignment(VerticalAlignment::Center);
        session->web = WebView2();
        session->web.DefaultBackgroundColor(color(base));
        pane.Children().Append(session->web);
        pane.Children().Append(loading);
        session->window.Content(pane);
        const auto weak = weak_from_this();
        const std::weak_ptr<ShapeEdit> weak_session(session);
        session->window.AppWindow().Closing([weak_session](auto&&, const Microsoft::UI::Windowing::AppWindowClosingEventArgs& event)
        {
            const auto edit = weak_session.lock();
            if (!edit || edit->closing || (!edit->dirty && !edit->saving))
                return;
            event.Cancel(true);
            if (edit->saving)
                return;
            try
            {
                JsonObject request;
                text(request, L"action", L"request-close");
                edit->web.CoreWebView2().PostWebMessageAsJson(request.Stringify());
            }
            catch (const hresult_error&) {}
        });
        session->window.Closed([weak, weak_session](auto&&, auto&&)
        {
            const auto edit = weak_session.lock();
            if (!edit)
                return;
            edit->closed = true;
            edit->web.Close();
            if (const auto owner = weak.lock(); owner && owner->shape_session_ == edit)
                owner->shape_session_.reset();
        });
        session->window.Activate();
        co_await session->web.EnsureCoreWebView2Async();
        if (closed_ || session->closed)
            co_return;
        const auto web = session->web.CoreWebView2();
        web.SetVirtualHostNameToFolderMapping(hstring(host), assets.wstring(), CoreWebView2HostResourceAccessKind::Deny);
        web.Settings().AreDevToolsEnabled(false);
        web.Settings().AreDefaultContextMenusEnabled(false);
        web.Settings().AreDefaultScriptDialogsEnabled(false);
        web.Settings().AreBrowserAcceleratorKeysEnabled(false);
        web.NavigationStarting([](auto&&, const CoreWebView2NavigationStartingEventArgs& event)
        {
            event.Cancel(event.Uri() != url);
        });
        web.NewWindowRequested([](auto&&, const CoreWebView2NewWindowRequestedEventArgs& event) { event.Handled(true); });
        web.NavigationCompleted([loading](auto&&, const CoreWebView2NavigationCompletedEventArgs& event)
        {
            if (!event.IsSuccess())
                loading.Text(tr(L"Impossible de charger l’éditeur de formes."));
        });
        web.WebMessageReceived([weak, weak_session, loading](auto&&, const CoreWebView2WebMessageReceivedEventArgs& event)
        {
            const auto owner = weak.lock();
            const auto edit = weak_session.lock();
            if (!owner || !edit || owner->closed_ || edit->closed || event.Source() != url)
                return;
            try
            {
                const auto message = event.WebMessageAsJson();
                if (message.size() > 12 * 1024 * 1024)
                {
                    edit->feedback(tr(L"Les données du dessin sont invalides."), true);
                    return;
                }
                const auto payload = JsonObject::Parse(message);
                const auto action = payload.GetNamedString(L"action", L"");
                if (action == L"ready")
                {
                    edit->web.CoreWebView2().PostWebMessageAsJson(edit->initial);
                    loading.Visibility(Visibility::Collapsed);
                }
                else if (action == L"close" && !edit->saving)
                    owner->close_shape_editor();
                else if (action == L"dirty")
                    edit->dirty = payload.GetNamedBoolean(L"value", false);
                else if (action == L"save")
                    owner->save_shape(edit, message);
            }
            catch (const hresult_error& error) { edit->feedback(error.message(), true); }
        });
        web.Navigate(hstring(url));
    }
    catch (const hresult_error& problem) { error = problem.message(); }
    catch (const std::exception& problem) { error = widen(problem.what()); }
    if (!closed_ && !error.empty())
    {
        close_shape_editor();
        shape_session_.reset();
        status(error, true);
    }
}

fire_and_forget Workspace::save_shape(std::shared_ptr<ShapeEdit> session, hstring message)
{
    if (closed_ || session->saving || session->closed)
        co_return;
    if (busy_)
    {
        session->feedback(tr(L"Une autre opération est en cours."), true);
        co_return;
    }
    const auto lifetime = shared_from_this();
    session->saving = true;
    busy_ = true;
    session->feedback(tr(L"Validation du dessin…"));
    const apartment_context ui;
    co_await resume_background();
    std::shared_ptr<const flash::Movie> edited;
    std::vector<Row> rows;
    std::wstring problem;
    try
    {
        auto draft = decode_draft(*session->movie, session->tag, message);
        edited = std::make_shared<flash::Movie>(flash::replace_shape_drawing(*session->movie, session->tag, draft));
        rows = index_rows(*edited);
    }
    catch (const hresult_error&) { problem = tr(L"Les données du dessin sont invalides."); }
    catch (const std::exception& error) { problem = widen(error.what()); }
    co_await ui;
    std::optional<std::filesystem::path> path;
    if (!closed_ && !session->closed && problem.empty())
    {
        try { path = swf_copy_path(session->original, tr(L"-forme"), session->window); }
        catch (const hresult_error& error) { problem = error.message(); }
        catch (const std::exception& error) { problem = widen(error.what()); }
    }
    if (path)
    {
        co_await resume_background();
        try
        {
            if (session->movie->header().compression == 'C')
            {
                const auto bytes = edited->compressed();
                edited = std::make_shared<flash::Movie>(flash::Movie::parse(bytes));
                flash::write_file(*path, bytes);
            }
            else
                flash::write_file(*path, edited->uncompressed());
        }
        catch (const std::exception& error) { problem = widen(error.what()); }
        co_await ui;
    }
    if (closed_)
        co_return;
    busy_ = false;
    session->saving = false;
    if (!path || !problem.empty())
    {
        session->feedback(problem.empty() ? tr(L"Enregistrement de la copie annulé.") : localized_error(problem), !problem.empty());
        co_return;
    }
    close_shape_editor();
    remember_recent(*path);
    add_document(*path, std::move(edited), std::move(rows), session->tag);
    status(tr(L"Forme modifiée et enregistrée"));
}
}
