import {
    basicSetup, EditorView, EditorState, Compartment, Prec, keymap,
    syntaxHighlighting, autocompletion, completionStatus, lintGutter, setDiagnostics
} from "./vendor/codemirror.js";
import { language, colors, makeCatalog, suggest } from "./as3-language.js";
import platformSymbols from "./platform-symbols.js";

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
    ".cm-gutters": { backgroundColor: "#121418", color: "#b0bac9", borderRight: "1px solid #323842" },
    ".cm-lineNumbers .cm-gutterElement": { padding: "0 9px 0 5px", minWidth: "30px" },
    ".cm-gutter-lint:not(:has(.cm-lint-marker))": { display: "none" },
    ".cm-scroller::-webkit-scrollbar": { width: "10px", height: "10px" },
    ".cm-scroller::-webkit-scrollbar-thumb": { backgroundColor: "#4e5967", border: "2px solid #121418", borderRadius: "6px" },
    ".cm-scroller::-webkit-scrollbar-corner": { backgroundColor: "#121418" },
    ".cm-activeLine, .cm-activeLineGutter": { backgroundColor: "#1d242f" },
    ".cm-cursor": { borderLeftColor: "#ffba72" },
    "&.cm-focused > .cm-scroller > .cm-selectionLayer .cm-selectionBackground, .cm-selectionBackground":
        { backgroundColor: "#34435c" },
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
    const start = view.state.doc.line(Math.min(Math.max(message.line, 1), view.state.doc.lines)).from;
    view.dispatch({ selection: { anchor: start }, effects: EditorView.scrollIntoView(start, { y: "start" }) });
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
    } else if (session && session.id === data.id) {
        if (data.action === "save") save();
        if (data.action === "state") setState(data);
        if (data.action === "close") {
            session.view.destroy();
            session = undefined;
        }
    }
});
bridge.postMessage({ action: "ready" });
