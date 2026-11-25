import {
    basicSetup, EditorView, EditorState, Compartment, Prec, keymap, StreamLanguage,
    syntaxHighlighting, autocompletion, completionStatus, lintGutter, setDiagnostics
} from "./vendor/codemirror.js";
import { language, colors, makeCatalog, suggest } from "./as3-language.js";
import platformSymbols from "./platform-symbols.js";
import { codeLink } from "./code-navigation.js";

const bridge = window.chrome.webview;
const parent = document.querySelector("#editor");
const locking = new Compartment();
let session;

const frenchPhrases = {
    Find: "Rechercher",
    Replace: "Remplacer",
    next: "suivant",
    previous: "précédent",
    all: "tout",
    "match case": "respecter la casse",
    "by word": "mot entier",
    regexp: "expression régulière",
    replace: "remplacer",
    "replace all": "tout remplacer",
    close: "fermer",
    "Go to line": "Aller à la ligne",
    "go": "aller"
};

const theme = EditorView.theme({
    "&": { height: "100%", color: "#f2f4f8", backgroundColor: "#121418" },
    ".cm-scroller": { fontFamily: "Consolas, monospace", fontSize: "13px", lineHeight: "20px" },
    ".cm-content": { padding: "11px 0 20px", caretColor: "#ffba72" },
    ".cm-line": { padding: "0 14px", minHeight: "20px" },
    ".cm-content[data-navigation]": { cursor: "pointer" },
    ".cm-gutters": { backgroundColor: "#121418", color: "#b0bac9", borderRight: "1px solid #323842" },
    ".cm-lineNumbers .cm-gutterElement": { padding: "0 9px 0 5px", minWidth: "30px" },
    ".cm-gutter-lint:not(:has(.cm-lint-marker))": { display: "none" },
    ".cm-scroller::-webkit-scrollbar": { width: "10px", height: "10px" },
    ".cm-scroller::-webkit-scrollbar-thumb": { backgroundColor: "#4e5967", border: "2px solid #121418", borderRadius: "6px" },
    ".cm-scroller::-webkit-scrollbar:horizontal": { height: "12px" },
    ".cm-scroller::-webkit-scrollbar-corner": { backgroundColor: "#121418" },
    ".cm-activeLine": { backgroundColor: "#7394c012" },
    ".cm-activeLineGutter": { backgroundColor: "#1d242f" },
    ".cm-cursor": { borderLeftColor: "#ffba72" },
    ".cm-selectionLayer .cm-selectionBackground": { backgroundColor: "#355278" },
    "&.cm-focused > .cm-scroller > .cm-selectionLayer .cm-selectionBackground": { backgroundColor: "#3b65a2" },
    ".cm-content ::selection": { backgroundColor: "#3b65a2" },
    ".cm-tooltip": { backgroundColor: "#20242c", color: "#f2f4f8", borderColor: "#414a58", borderRadius: "5px" },
    ".cm-tooltip-autocomplete ul": { fontFamily: "Consolas, monospace", fontSize: "13px" },
    ".cm-tooltip-autocomplete ul li[aria-selected]": { backgroundColor: "#354969", color: "#fff" },
    ".cm-completionDetail": { color: "#b0bac9", fontStyle: "normal", marginLeft: "10px" },
    ".cm-completionIcon-class": { color: "#80dbbe" },
    ".cm-completionIcon-function, .cm-completionIcon-method": { color: "#ae9dff" },
    ".cm-diagnostic-error": { borderLeftColor: "#ff9299" }
}, { dark: true });

function send(action, data = {}) {
    if (session) bridge.postMessage({ action, id: session.id, ...data });
}

function save() {
    if (session?.dirty && !session.locked) send("save", { source: session.view.state.doc.toString() });
    return true;
}

const assembly = StreamLanguage.define({
    startState: () => ({ command: true }),
    token(stream, state) {
        if (stream.sol()) state.command = true;
        if (stream.eatSpace()) return null;
        if (stream.match(/^;.*/)) return "lineComment";
        if (stream.match(/^L[0-9a-f]+:?/i)) return "typeName";
        if (stream.match(/^"(?:[^"\\]|\\.)*"/)) return "string";
        if (stream.match(/^-?(?:0x[0-9a-f]+|\d+)/i)) return "number";
        if (state.command && stream.match(/^\.?[A-Za-z_][\w_]*/)) {
            state.command = false;
            return "keyword";
        }
        stream.next();
        return null;
    }
});

const workspaceKeys = [
    ["Mod-o", "open"], ["Mod-Shift-o", "folder"], ["Mod-Shift-e", "workspace"],
    ["Mod-w", "close"], ["Ctrl-Tab", "next"], ["Ctrl-Shift-Tab", "previous"],
    ["Mod-1", "resources"], ["Mod-2", "scripts"], ["Mod-3", "tags"],
    ["Mod-Shift-f", "search"], ["Mod-s", "save"], ["Mod-Shift-s", "saveAs"], ["F1", "about"]
].map(([key, name]) => ({ key, run() { send("shortcut", { name }); return true; } }));

function jump(view, line) {
    const start = view.state.doc.line(Math.min(Math.max(line, 1), view.state.doc.lines)).from;
    view.dispatch({ selection: { anchor: start }, effects: EditorView.scrollIntoView(start, { y: "start" }) });
}

function openPreview(message) {
    session?.view.destroy();
    const french = message.locale === "fr";
    document.documentElement.lang = french ? "fr" : "en";
    document.title = french ? "Code ActionScript" : "ActionScript code";
    const catalog = makeCatalog(message.classes, platformSymbols);
    const navigate = (editor, position, control) => {
        const line = editor.state.doc.lineAt(position);
        const link = codeLink(line.text, position - line.from, control, message.instructions);
        if (!link) return false;
        const { action, ...data } = link;
        send(action, data);
        return true;
    };
    const view = new EditorView({
        parent,
        doc: message.source,
        extensions: [
            theme, basicSetup, message.instructions ? assembly : language(catalog), syntaxHighlighting(colors),
            EditorState.readOnly.of(true),
            EditorState.phrases.of(french ? frenchPhrases : {}),
            EditorView.contentAttributes.of({ tabindex: "0", "aria-label": document.title }),
            Prec.highest(keymap.of([
                ...workspaceKeys,
                { key: "Mod-Enter", run: editor => navigate(editor, editor.state.selection.main.from, true) }
            ])),
            EditorView.domEventHandlers({
                click(event, editor) {
                    if (event.button !== 0 || !editor.state.selection.main.empty) return false;
                    const position = editor.posAtCoords({ x: event.clientX, y: event.clientY });
                    return position !== null && navigate(editor, position, event.ctrlKey) && event.ctrlKey;
                },
                mousemove(event, editor) {
                    editor.contentDOM.toggleAttribute("data-navigation", event.ctrlKey);
                    return false;
                },
                mouseleave(_, editor) {
                    editor.contentDOM.removeAttribute("data-navigation");
                    return false;
                }
            })
        ]
    });
    session = { id: message.id, view, preview: true };
    let visible = message.line;
    let scheduled = false;
    view.scrollDOM.addEventListener("scroll", () => {
        if (scheduled) return;
        scheduled = true;
        requestAnimationFrame(() => {
            scheduled = false;
            if (session?.view !== view) return;
            const top = Math.max(0, view.scrollDOM.scrollTop - view.documentPadding.top);
            const line = view.state.doc.lineAt(view.lineBlockAtHeight(top).from).number;
            if (line === visible) return;
            visible = line;
            send("scroll", { line });
        });
    }, { passive: true });
    jump(view, message.line);
}

function replaceSession(message) {
    session?.view.destroy();
    const catalog = makeCatalog(message.classes, platformSymbols);
    const french = message.locale === "fr";
    document.documentElement.lang = french ? "fr" : "en";
    document.title = french ? "Éditeur ActionScript 3" : "ActionScript 3 editor";
    parent.setAttribute("aria-label", document.title);
    const state = EditorState.create({
        doc: message.source,
        extensions: [
            theme, basicSetup, locking.of([]), language(catalog), syntaxHighlighting(colors),
            autocompletion({ override: [suggest(catalog, message.qualified)], maxRenderedOptions: 10 }),
            lintGutter(),
            EditorState.phrases.of(french ? frenchPhrases : {}),
            EditorView.contentAttributes.of({ "aria-label": french ? "Source ActionScript 3" : "ActionScript 3 source", spellcheck: "false" }),
            Prec.highest(keymap.of([
                { key: "Mod-s", run: save },
                { key: "Escape", run: view => {
                    if (completionStatus(view.state)) return false;
                    send("cancel");
                    return true;
                } }
            ])),
            EditorView.updateListener.of(update => {
                if (!update.docChanged) return;
                if (session.failed) {
                    session.failed = false;
                    send("editedAfterError");
                }
                const dirty = session.newClass || !update.state.doc.eq(session.original);
                if (dirty !== session.dirty) {
                    session.dirty = dirty;
                    send("changed", { dirty });
                }
            })
        ]
    });
    const view = new EditorView({ state, parent });
    session = { id: message.id, view, original: state.doc, newClass: message.newClass, dirty: message.newClass, locked: false, failed: false };
    jump(view, message.line);
    view.focus();
}

function setState(message) {
    const { view } = session;
    session.locked = message.locked;
    session.failed = Boolean(message.error);
    view.dispatch({ effects: locking.reconfigure(message.locked ? [EditorState.readOnly.of(true), EditorView.editable.of(false)] : []) });
    const diagnostics = [];
    const location = /(?:\bline|\bligne|\bln)\s*[:=]?\s*(\d+)(?:\s*[,;:]?\s*(?:column|colonne|col)\s*[:=]?\s*(\d+))?/i.exec(message.error);
    if (location && +location[1] >= 1 && +location[1] <= view.state.doc.lines) {
        const line = view.state.doc.line(+location[1]);
        const from = location[2] ? Math.min(line.to, line.from + Math.max(0, +location[2] - 1)) : line.from;
        diagnostics.push({ from, to: location[2] ? Math.min(line.to, from + 1) : line.to, severity: "error", message: message.error });
        view.dispatch({ selection: { anchor: from }, scrollIntoView: true });
    }
    view.dispatch(setDiagnostics(view.state, diagnostics));
    if (!message.locked) view.focus();
}

bridge.addEventListener("message", ({ data }) => {
    if (data.action === "open") {
        replaceSession(data);
    } else if (data.action === "preview") {
        openPreview(data);
    } else if (session && session.id === data.id) {
        if (data.action === "save") save();
        if (data.action === "state") setState(data);
        if (data.action === "goto" && session.preview) jump(session.view, data.line);
        if (data.action === "close") {
            session.view.destroy();
            session = undefined;
        }
    }
});
bridge.postMessage({ action: "ready" });
