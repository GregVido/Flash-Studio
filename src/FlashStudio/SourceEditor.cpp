#include "UiStyle.hpp"
#include "../FlashCore/ClassEditing.hpp"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.Web.WebView2.Core.h>
#include <winrt/Windows.Data.Json.h>

#include <algorithm>
#include <stdexcept>

namespace studio
{
using namespace ui;
using namespace Microsoft::Web::WebView2::Core;
using namespace Windows::Data::Json;

namespace
{
constexpr std::wstring_view editor_host = L"as3-editor.flashstudio.local";
constexpr std::wstring_view editor_url = L"https://as3-editor.flashstudio.local/index.html";
const wchar_t* editor_hint() { return tr(L"Ctrl+Espace pour les suggestions · Ctrl+S pour enregistrer"); }

bool has_fallback_bodies(std::string_view source)
{
    return source.find("Corps AVM2 non reconstruit") != std::string_view::npos ||
        source.find("Initialiseur statique non reconstruit") != std::string_view::npos;
}

std::filesystem::path editor_assets()
{
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        throw std::runtime_error("Le dossier de Flash Studio est introuvable.");
    const auto assets = std::filesystem::path(path.data()).parent_path() / L"Editor";
    for (const auto file : {L"index.html", L"editor.js", L"as3-language.js", L"platform-symbols.js", L"vendor/codemirror.js"})
        if (!std::filesystem::is_regular_file(assets / file))
            throw std::runtime_error("Les ressources de l’éditeur AS3 sont introuvables.");
    return assets;
}

void put(const JsonObject& target, std::wstring_view key, std::wstring_view text)
{
    target.SetNamedValue(hstring(key), JsonValue::CreateStringValue(hstring(text)));
}

bool identifier(std::string_view name)
{
    const auto letter = [](char value) { return (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') || value == '$' || value == '_'; };
    return !name.empty() && letter(name.front()) && std::ranges::all_of(name, [&](char value)
    {
        return letter(value) || (value >= '0' && value <= '9');
    });
}
}

struct Workspace::SourceEdit
{
    std::shared_ptr<const flash::Movie> movie;
    std::filesystem::path original;
    ScriptEntry script;
    std::string source;
    std::size_t line;
    std::wstring id;
    View return_view;
    bool dirty = false;
    bool saving = false;
};

void Workspace::build_source_editor(const Grid& preview)
{
    source_editor_ = Grid();
    source_editor_.Margin(pad(12));
    source_editor_.Background(brush(base));
    source_editor_.Visibility(Visibility::Collapsed);
    row(source_editor_, 1, GridUnitType::Star);
    row(source_editor_);
    row(source_editor_);
    place(source_editor_, divider(), 1);

    Grid actions;
    actions.Padding(pad(10, 8, 10, 8));
    actions.Background(brush(panel));
    actions.ColumnSpacing(12);
    column(actions, 1, GridUnitType::Star);
    column(actions, 1, GridUnitType::Auto);
    source_feedback_ = label(editor_hint(), 12, secondary);
    source_feedback_.TextWrapping(TextWrapping::Wrap);
    source_feedback_.TextTrimming(TextTrimming::None);
    source_feedback_.VerticalAlignment(VerticalAlignment::Center);
    Automation::AutomationProperties::SetLiveSetting(source_feedback_, Automation::Peers::AutomationLiveSetting::Polite);
    ScrollViewer diagnostics;
    diagnostics.MaxHeight(100);
    diagnostics.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    diagnostics.Content(source_feedback_);
    place(actions, diagnostics, 0);

    source_save_ = Button();
    source_save_.Content(box_value(tr(L"Enregistrer")));
    source_save_.Style(Application::Current().Resources().Lookup(box_value(L"AccentButtonStyle")).as<Style>());
    source_save_.IsEnabled(false);
    source_save_.Click([this](auto&&, auto&&) { post_editor_action(L"save"); });
    source_cancel_ = Button();
    source_cancel_.Content(box_value(tr(L"Annuler")));
    source_cancel_.Click([this](auto&&, auto&&) { end_source_edit(); });
    const auto buttons = horizontal(8);
    buttons.VerticalAlignment(VerticalAlignment::Center);
    buttons.Children().Append(source_save_);
    buttons.Children().Append(source_cancel_);
    place(actions, buttons, 0, 1);
    place(source_editor_, actions, 2);
    preview.Children().Append(source_editor_);
}

void Workspace::set_source_editing(bool editing)
{
    busy_ = editing;
    for (const auto& control : {open_, save_, global_search_, source_edit_, document_class_})
        control.IsEnabled(!editing);
    for (const auto& tab : tabs_)
        tab.IsEnabled(!editing);
    for (const auto& mode : mode_tabs_)
        mode.IsEnabled(!editing);
    recent_.IsEnabled(!editing);
    export_.IsEnabled(!editing);
    documents_bar_.IsEnabled(!editing);
    script_tabs_bar_.IsEnabled(!editing);
    script_pin_.IsEnabled(!editing);
    script_search_.IsEnabled(!editing);
    scripts_.IsEnabled(!editing);
    source_create_.IsEnabled(!editing && movie_ && flash::supports_as3_class_creation(*movie_));
    members_.IsEnabled(!editing);
    for (const auto& child : abc_tools_.Children())
        if (const auto control = child.try_as<Control>())
            control.IsEnabled(!editing);
    toggle_outline_.IsEnabled(!editing);
    source_cancel_.IsEnabled(true);
    source_save_.IsEnabled(false);
    source_editor_.Visibility(editing ? Visibility::Visible : Visibility::Collapsed);
    if (editing)
    {
        ++preview_version_;
        code_view_.Visibility(Visibility::Collapsed);
        output_.Visibility(Visibility::Collapsed);
        preview_error_.Visibility(Visibility::Collapsed);
        progress_.Visibility(Visibility::Collapsed);
        progress_.IsActive(false);
    }
    update_abc_visibility();
    if (editing)
    {
        abc_owner_.IsEnabled(false);
        abc_method_list_.IsEnabled(false);
    }
}

void Workspace::open_editor_source()
{
    if (!source_session_ || !source_ready_)
        return;
    JsonObject message;
    put(message, L"action", L"open");
    put(message, L"id", source_session_->id);
    put(message, L"source", widen(source_session_->source));
    put(message, L"qualified", source_session_->script.qualified);
    put(message, L"locale", language() == Language::french ? L"fr" : L"en");
    message.SetNamedValue(L"newClass", JsonValue::CreateBooleanValue(!source_session_->script.type));
    message.SetNamedValue(L"line", JsonValue::CreateNumberValue(static_cast<double>(source_session_->line)));
    JsonObject classes;
    for (const auto& entry : script_entries_)
    {
        if (!entry.type || classes.HasKey(hstring(entry.qualified)))
            continue;
        const auto& type = entry.module->classes()[*entry.type];
        JsonArray members;
        for (const auto& member : type.members)
        {
            if (!identifier(member.name) || member.name == type.name)
                continue;
            JsonObject item;
            put(item, L"label", widen(member.name));
            put(item, L"detail", widen(member.declaration));
            put(item, L"type", member.declaration.find("function ") != std::string::npos &&
                member.declaration.find("function get ") == std::string::npos &&
                member.declaration.find("function set ") == std::string::npos ? L"method" : L"property");
            item.SetNamedValue(L"isPublic", JsonValue::CreateBooleanValue(member.declaration.starts_with("public ")));
            item.SetNamedValue(L"isStatic", JsonValue::CreateBooleanValue(member.declaration.find("static ") != std::string::npos));
            members.Append(item);
        }
        classes.SetNamedValue(hstring(entry.qualified), members);
    }
    message.SetNamedValue(L"classes", classes);
    source_web_.Focus(FocusState::Programmatic);
    source_web_.CoreWebView2().PostWebMessageAsJson(message.Stringify());
    set_editor_state(editor_hint());
}

void Workspace::post_editor_action(std::wstring_view action)
{
    if (!source_session_ || !source_ready_)
        return;
    JsonObject message;
    put(message, L"action", action);
    put(message, L"id", source_session_->id);
    source_web_.CoreWebView2().PostWebMessageAsJson(message.Stringify());
}

void Workspace::set_editor_state(std::wstring_view feedback, bool error)
{
    if (!source_session_)
        return;
    source_feedback_.Foreground(brush(error ? danger : secondary));
    const auto diagnostic = error ? localized_error(feedback) : std::wstring(feedback);
    source_feedback_.Text(diagnostic);
    source_save_.IsEnabled(source_ready_ && source_session_->dirty && !source_session_->saving);
    source_cancel_.IsEnabled(!source_session_->saving);
    if (source_ready_)
    {
        JsonObject message;
        put(message, L"action", L"state");
        put(message, L"id", source_session_->id);
        put(message, L"error", error ? std::wstring_view(diagnostic) : std::wstring_view{});
        message.SetNamedValue(L"locked", JsonValue::CreateBooleanValue(source_session_->saving));
        source_web_.CoreWebView2().PostWebMessageAsJson(message.Stringify());
    }
}

void Workspace::end_source_edit()
{
    if (!source_session_ || source_session_->saving)
        return;
    post_editor_action(L"close");
    next_code_line_ = source_session_->line;
    const auto view = source_session_->return_view;
    const bool creating = !source_session_->script.type;
    source_session_.reset();
    set_source_editing(false);
    set_view(view);
    (creating ? source_create_ : source_edit_).Focus(FocusState::Programmatic);
    status(tr(L"Source ActionScript affiché"));
}

fire_and_forget Workspace::edit_class()
{
    if (busy_ || source_preparing_ || !movie_ || !selected_script_)
        co_return;
    if (auto script = script_entries_[*selected_script_]; script.type)
        begin_class_source(std::move(script));
}

fire_and_forget Workspace::create_class()
{
    if (busy_ || source_preparing_ || mode_ != Mode::scripts || !movie_ || !flash::supports_as3_class_creation(*movie_))
        co_return;
    const auto lifetime = shared_from_this();
    const auto movie = movie_;
    try
    {
        TextBox input;
        input.Header(box_value(tr(L"Nom complet de la classe")));
        input.PlaceholderText(tr(L"exemple.outils.MaClasse"));
        input.MaxLength(240);
        if (selected_script_)
            if (const auto& package = script_entries_[*selected_script_].package; !package.empty())
                input.Text(package + L".");
        TextBlock feedback = label(L"", 12, danger);
        feedback.TextWrapping(TextWrapping::Wrap);
        StackPanel fields;
        fields.Spacing(8);
        fields.Children().Append(input);
        fields.Children().Append(feedback);
        ContentDialog prompt;
        prompt.XamlRoot(root_.XamlRoot());
        prompt.Title(box_value(tr(L"Nouvelle classe ActionScript")));
        prompt.Content(fields);
        prompt.PrimaryButtonText(tr(L"Créer"));
        prompt.CloseButtonText(tr(L"Annuler"));
        prompt.DefaultButton(ContentDialogButton::Primary);
        prompt.PrimaryButtonClick([this, input, feedback](auto&&, const ContentDialogButtonClickEventArgs& args)
        {
            const auto name = input.Text();
            if (!flash::valid_as3_class_name(to_string(name)))
            {
                feedback.Text(tr(L"Saisissez un nom de classe valide, avec ou sans package."));
                args.Cancel(true);
            }
            else if (std::ranges::any_of(script_entries_, [&](const ScriptEntry& entry)
            {
                return entry.type && entry.qualified == std::wstring_view(name);
            }))
            {
                feedback.Text(tr(L"Cette classe existe déjà dans le SWF."));
                args.Cancel(true);
            }
        });
        if (co_await prompt.ShowAsync() != ContentDialogResult::Primary || closed_ || movie_ != movie)
            co_return;
        const std::wstring qualified(input.Text());
        const auto split = qualified.rfind(L'.');
        const auto package = split == std::wstring::npos ? std::wstring{} : qualified.substr(0, split);
        const auto name = split == std::wstring::npos ? qualified : qualified.substr(split + 1);
        begin_class_source({0, {}, {}, package, name, qualified, {}});
    }
    catch (const hresult_error& error) { if (!closed_) status(error.message(), true); }
}

fire_and_forget Workspace::begin_class_source(ScriptEntry script)
{
    if (busy_ || source_preparing_ || !movie_)
        co_return;
    const auto lifetime = shared_from_this();
    const auto movie = movie_;
    const auto original = file_;
    const auto previous_view = view_;
    const auto line = script.type && view_ == View::source ? 1 + static_cast<std::size_t>(code_view_.VerticalOffset() / code_.LineHeight()) : 1;
    source_preparing_ = true;
    struct Preparing { bool& busy; ~Preparing() { busy = false; } } preparing{source_preparing_};
    const apartment_context ui_thread;
    try
    {
        status(tr(L"Préparation du source ActionScript…"));
        co_await resume_background();
        std::string source, error;
        try
        {
            if (script.type)
                source = script.module->decompile_class(*script.type).text;
            else
            {
                const auto package = script.package.empty() ? std::string{} : " " + to_string(hstring(script.package));
                const auto name = to_string(hstring(script.name));
                source = std::format("package{}\n{{\n    public class {}\n    {{\n        public function {}()\n        {{\n        }}\n    }}\n}}\n", package, name, name);
            }
        }
        catch (const std::exception& problem) { error = problem.what(); }
        co_await ui_thread;
        if (closed_ || busy_ || movie_ != movie || (script.type && (!selected_script_ ||
            script_entries_[*selected_script_].qualified != script.qualified ||
            script_entries_[*selected_script_].tag != script.tag)))
            co_return;
        if (!error.empty())
        {
            status(widen(error), true);
            co_return;
        }
        if (script.type && view_ != View::source)
            set_view(View::source);
        source_session_ = std::make_shared<SourceEdit>(SourceEdit{movie, original, script, std::move(source), line,
            std::to_wstring(++source_revision_), script.type ? View::source : previous_view, !script.type});
        set_source_editing(true);
        if (!script.type)
        {
            detail_title_.Text(script.qualified);
            detail_meta_.Text(tr(L"Nouvelle classe ActionScript"));
        }
        set_editor_state(tr(L"Chargement de l’éditeur…"));
        status(tr(L"Édition de ") + script.qualified);
        if (source_ready_)
        {
            open_editor_source();
            co_return;
        }

        const auto assets = editor_assets();
        if (source_web_)
        {
            source_web_.Close();
            const auto children = source_editor_.Children();
            std::uint32_t index{};
            if (children.IndexOf(source_web_, index))
                children.RemoveAt(index);
        }
        source_web_ = WebView2();
        source_web_.DefaultBackgroundColor(color(base));
        Automation::AutomationProperties::SetName(source_web_, tr(L"Éditeur ActionScript 3"));
        place(source_editor_, source_web_, 0);
        co_await source_web_.EnsureCoreWebView2Async();
        if (closed_ || !source_session_)
            co_return;
        const auto web = source_web_.CoreWebView2();
        web.SetVirtualHostNameToFolderMapping(hstring(editor_host), assets.wstring(), CoreWebView2HostResourceAccessKind::Deny);
        web.Settings().AreDevToolsEnabled(false);
        web.Settings().AreDefaultContextMenusEnabled(false);
        web.Settings().AreDefaultScriptDialogsEnabled(false);
        web.Settings().AreBrowserAcceleratorKeysEnabled(false);
        web.NavigationStarting([](auto&&, const CoreWebView2NavigationStartingEventArgs& event)
        {
            event.Cancel(event.Uri() != editor_url);
        });
        web.NewWindowRequested([](auto&&, const CoreWebView2NewWindowRequestedEventArgs& event) { event.Handled(true); });
        const auto weak = weak_from_this();
        web.NavigationCompleted([weak](auto&&, const CoreWebView2NavigationCompletedEventArgs& event)
        {
            if (const auto self = weak.lock(); self && !self->closed_ && !event.IsSuccess())
                self->set_editor_state(tr(L"Impossible de charger les ressources de l’éditeur."), true);
        });
        web.WebMessageReceived([weak](auto&&, const CoreWebView2WebMessageReceivedEventArgs& event)
        {
            const auto self = weak.lock();
            if (!self || self->closed_ || event.Source() != editor_url)
                return;
            try
            {
                const auto message = JsonObject::Parse(event.WebMessageAsJson());
                const auto action = message.GetNamedString(L"action", L"");
                if (action == L"ready")
                {
                    self->source_ready_ = true;
                    self->open_editor_source();
                }
                else if (self->source_session_ && message.GetNamedString(L"id", L"") == self->source_session_->id)
                {
                    if (action == L"changed")
                    {
                        self->source_session_->dirty = message.GetNamedBoolean(L"dirty", false);
                        self->source_save_.IsEnabled(self->source_session_->dirty && !self->source_session_->saving);
                    }
                    else if (action == L"save")
                        self->save_class(to_string(message.GetNamedString(L"source")));
                    else if (action == L"editedAfterError" && !self->source_session_->saving)
                        self->set_editor_state(editor_hint());
                    else if (action == L"cancel")
                        self->end_source_edit();
                }
            }
            catch (const hresult_error& error) { self->status(error.message(), true); }
        });
        web.Navigate(hstring(editor_url));
    }
    catch (const hresult_error& error)
    {
        if (!closed_ && source_session_)
            set_editor_state(error.message(), true);
    }
    catch (const std::exception& error)
    {
        if (!closed_)
        {
            if (source_session_) set_editor_state(widen(error.what()), true);
            else status(widen(error.what()), true);
        }
    }
}

fire_and_forget Workspace::save_class(std::string source)
{
    const auto session = source_session_;
    if (!session || session->saving || !session->dirty)
        co_return;
    const auto lifetime = shared_from_this();
    std::wstring problem;
    try
    {
        session->saving = true;
        set_editor_state(tr(L"Compilation et validation du bloc ABC…"));
        if (has_fallback_bodies(source))
            throw std::runtime_error("Replace the fallback blocks in the source before compiling.");
        if (!find_class_compiler(true))
        {
            session->saving = false;
            set_editor_state(editor_hint());
            co_return;
        }
        const auto compiler = class_compiler_;
        const auto qualified = to_string(hstring(session->script.qualified));
        const auto french = language() == Language::french;
        const apartment_context ui_thread;
        co_await resume_background();
        std::shared_ptr<flash::Movie> edited;
        try
        {
            edited = std::make_shared<flash::Movie>(session->script.type ?
                flash::compile_as3_class(*session->movie, session->script.tag, qualified, source, compiler, french) :
                flash::add_as3_class(*session->movie, qualified, source, compiler, french));
        }
        catch (const std::exception& error) { problem = widen(error.what()); }
        co_await ui_thread;
        if (closed_)
            co_return;
        if (edited)
        {
            const auto output = swf_copy_path(session->original, tr(L"-classe"));
            if (!output)
            {
                session->saving = false;
                set_editor_state(tr(L"Compilation réussie. Le brouillon est toujours ouvert."));
                co_return;
            }
            set_editor_state(tr(L"Écriture du SWF recompilé…"));
            co_await resume_background();
            std::vector<Row> rows;
            try
            {
                flash::Bytes compressed;
                if (session->movie->header().compression == 'C')
                {
                    compressed = edited->compressed();
                    edited = std::make_shared<flash::Movie>(flash::Movie::parse(compressed));
                }
                rows = index_rows(*edited);
                flash::write_file(*output, compressed.empty() ? edited->uncompressed() : flash::ByteView(compressed));
            }
            catch (const std::exception& error) { problem = widen(error.what()); }
            co_await ui_thread;
            if (closed_)
                co_return;
            if (problem.empty())
            {
                session->saving = false;
                end_source_edit();
                remember_recent(*output);
                add_document(*output, std::move(edited), std::move(rows), {});
                open_script_target({session->script.type ? std::optional{session->script.tag} : std::nullopt,
                    {}, session->script.qualified, {}, session->line, View::source});
                status(tr(L"Classe recompilée dans ") + output->filename().wstring());
                co_return;
            }
        }
    }
    catch (const hresult_error& error) { problem = error.message(); }
    catch (const std::exception& error) { problem = widen(error.what()); }
    if (!closed_ && source_session_ == session)
    {
        session->saving = false;
        set_editor_state(problem, true);
    }
}
}
