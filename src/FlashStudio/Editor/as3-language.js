import { StreamLanguage, HighlightStyle, tags, pickedCompletion, Transaction } from "./vendor/codemirror.js";

const words = new Set(("as break case catch class const continue default delete do dynamic else extends final finally " +
    "for each function get if implements import in include instanceof interface internal is namespace native new override " +
    "package private protected public return set static super switch this throw try typeof use var while with").split(" "));
const builtins = ("Array Boolean Class Date DefinitionError Error EvalError Function int Math Namespace Number Object " +
    "QName RangeError ReferenceError RegExp SecurityError String SyntaxError TypeError uint URIError Vector VerifyError void XML XMLList").split(" ");
const literals = new Set(["true", "false", "null", "undefined", "NaN", "Infinity"]);
const name = "[A-Za-z_$][\\w$]*";
const path = `${name}(?:\\s*\\.\\s*${name})*`;
const documentInfo = new WeakMap();

const shortName = qualified => qualified.slice(qualified.lastIndexOf(".") + 1);
const packageName = qualified => qualified.slice(0, Math.max(0, qualified.lastIndexOf(".")));
const stripSpace = value => value.replace(/\s/g, "");

export function makeCatalog(movie, platform) {
    const symbols = new Map();
    const platformNames = new Set(platform);
    for (const qualified of [...builtins, ...platform, ...Object.keys(movie)]) {
        const label = shortName(qualified);
        const kind = platformNames.has(qualified) && /^[a-z]/.test(label) ? "function" : "class";
        symbols.set(qualified, { qualified, label, kind });
    }
    const byShort = new Map();
    for (const symbol of symbols.values()) {
        const list = byShort.get(symbol.label) ?? [];
        list.push(symbol);
        byShort.set(symbol.label, list);
    }
    return { symbols: [...symbols.values()], byShort, movie };
}

export function language(catalog) {
    return StreamLanguage.define({
        name: "ActionScript 3",
        startState: () => ({ comment: false, type: false }),
        languageData: { commentTokens: { line: "//", block: { open: "/*", close: "*/" } } },
        token(stream, state) {
            if (state.comment) {
                if (stream.skipTo("*/")) {
                    stream.match("*/");
                    state.comment = false;
                } else {
                    stream.skipToEnd();
                }
                return "blockComment";
            }
            if (stream.eatSpace()) return null;
            if (stream.match("//")) {
                stream.skipToEnd();
                return "lineComment";
            }
            if (stream.match("/*")) {
                state.comment = true;
                return "blockComment";
            }
            if (stream.match(/^(?:"(?:[^"\\]|\\.)*(?:"|$)|'(?:[^'\\]|\\.)*(?:'|$))/)) {
                state.type = false;
                return "string";
            }
            if (stream.match(/^(?:0[xX][\da-fA-F]+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)/)) {
                state.type = false;
                return "number";
            }
            if (stream.match(/^[A-Za-z_$][\w$]*/)) {
                const word = stream.current();
                const type = state.type || builtins.includes(word) || catalog.byShort.get(word)?.some(item => item.kind === "class");
                state.type = ["as", "is", "extends", "implements", "new", "class", "interface", "instanceof"].includes(word);
                if (literals.has(word)) return "atom";
                if (words.has(word)) return "keyword";
                return type ? "typeName" : "variableName";
            }
            if (stream.match(".<") || stream.match(":")) {
                state.type = true;
                return "punctuation";
            }
            if (!stream.match(".")) stream.next();
            state.type = false;
            return "punctuation";
        }
    });
}

export const colors = HighlightStyle.define([
    { tag: tags.keyword, color: "#ae9dff" },
    { tag: tags.typeName, color: "#80dbbe" },
    { tag: tags.string, color: "#9ad9a4" },
    { tag: [tags.number, tags.atom], color: "#ffbe74" },
    { tag: [tags.lineComment, tags.blockComment], color: "#9caebe" }
]);

function inspect(doc) {
    const previous = documentInfo.get(doc);
    if (previous) return previous;
    const source = doc.toString();
    // Mask comments and strings without moving offsets used by completion edits.
    const protectedRanges = [];
    const masked = source.replace(/\/\/[^\r\n]*|\/\*[\s\S]*?(?:\*\/|$)|"(?:[^"\\\r\n]|\\[^\r\n])*(?:"|$)|'(?:[^'\\\r\n]|\\[^\r\n])*(?:'|$)/gm,
        (token, start) => {
            const string = token.startsWith('"') || token.startsWith("'");
            const open = token.startsWith("//") || (token.startsWith("/*") && !token.endsWith("*/")) ||
                (string && (token.length === 1 || !/(?:^|[^\\])(?:\\\\)*["']$/.test(token)));
            protectedRanges.push({ start, end: start + token.length, open });
            return token.replace(/[^\r\n]/g, " ");
        });
    const pack = new RegExp(`\\bpackage\\s*(${path})?\\s*\\{`).exec(masked);
    const packageEnd = pack ? pack.index + pack[0].length : 0;
    const imports = [...masked.matchAll(new RegExp(`\\bimport\\s+(${path}(?:\\s*\\.\\s*\\*)?)\\s*;`, "g"))]
        .map(match => ({ name: stripSpace(match[1]), start: match.index, end: match.index + match[0].length }));
    const info = { masked, protectedRanges, package: stripSpace(pack?.[1] ?? ""), packageEnd, imports };
    documentInfo.set(doc, info);
    return info;
}

function localBindings(text) {
    const tokens = new RegExp(
        `\\bfunction\\s*(?:(?:get|set)\\s+)?(?:${name}\\s*)?\\(|` +
        `\\b(?:var|const)\\s+(${name})(?:\\s*:\\s*(${path}))?|[(){};]`, "g");
    const parameters = new RegExp(`(?:^|,)\\s*(?:\\.\\.\\.)?(${name})\\s*:\\s*(${path})?`, "g");
    const scopes = [];
    let depth = 0;
    let signature;
    for (const token of text.matchAll(tokens)) {
        const value = token[0];
        if (signature?.parens) {
            if (value === "(") signature.parens++;
            if (value === ")" && --signature.parens === 0) signature.end = token.index;
            continue;
        }
        if (value.startsWith("function")) {
            signature = { start: token.index + value.length, end: text.length, parens: 1 };
        } else if (value === "{") {
            depth++;
            if (signature) {
                const bindings = new Map();
                for (const match of text.slice(signature.start, signature.end).matchAll(parameters))
                    bindings.set(match[1], stripSpace(match[2] ?? ""));
                scopes.push({ depth, bindings });
                signature = undefined;
            }
        } else if (value === "}") {
            if (scopes.at(-1)?.depth === depth) scopes.pop();
            depth--;
            signature = undefined;
        } else if (value === ";") {
            signature = undefined;
        } else if (token[1] && scopes.length) {
            scopes.at(-1).bindings.set(token[1], stripSpace(token[2] ?? ""));
        }
    }
    return new Map(scopes.flatMap(scope => [...scope.bindings]));
}

function visibleClass(label, info, catalog) {
    if (label.includes(".")) return label;
    const choices = catalog.byShort.get(label) ?? [];
    const local = choices.find(candidate => packageName(candidate.qualified) === info.package);
    if (local) return local.qualified;
    const explicit = info.imports.find(item => shortName(item.name) === label);
    if (explicit) return explicit.name;
    const wildcard = choices.filter(candidate => info.imports.some(item => item.name === `${packageName(candidate.qualified)}.*`));
    return wildcard.length === 1 ? wildcard[0].qualified : choices.length === 1 ? choices[0].qualified : undefined;
}

function importCompletion(view, item, from, to, symbol, catalog) {
    const info = inspect(view.state.doc);
    const { qualified, label } = symbol;
    const owner = packageName(qualified);
    const imported = !owner || owner === info.package || info.imports.some(entry => entry.name === qualified);
    const conflicts = info.imports.some(entry => shortName(entry.name) === label && entry.name !== qualified) ||
        catalog.byShort.get(label)?.some(other => packageName(other.qualified) === info.package && other.qualified !== qualified);
    const wildcard = info.imports.filter(entry => entry.name.endsWith(".*"));
    const implicitlyImported = wildcard.some(entry => entry.name === `${owner}.*`) &&
        !catalog.byShort.get(label)?.some(other => other.qualified !== qualified &&
            wildcard.some(entry => entry.name === `${packageName(other.qualified)}.*`));
    let inserted = conflicts ? qualified : label;
    const changes = [];
    let caret = from + inserted.length;
    if (!imported && !conflicts && !implicitlyImported) {
        const after = info.imports.at(-1);
        const importLine = after && view.state.doc.lineAt(after.end);
        const offset = after && !info.masked.slice(after.end, importLine.to).trim() ? importLine.to :
            after?.end ?? info.packageEnd;
        if (offset < from) {
            const indent = after ? /^\s*/.exec(view.state.doc.lineAt(after.start).text)[0] : info.packageEnd ? "    " : "";
            const addition = `${offset ? "\n" : ""}${indent}import ${qualified};${after ? "" : "\n"}`;
            changes.push({ from: offset, insert: addition });
            caret += addition.length;
        } else {
            inserted = qualified;
            caret = from + inserted.length;
        }
    }
    changes.push({ from, to, insert: inserted });
    view.dispatch({ changes, selection: { anchor: caret },
        annotations: [Transaction.userEvent.of("input.complete"), pickedCompletion.of(item)] });
}

export function suggest(catalog, className) {
    return context => {
        const { state, pos, explicit } = context;
        const info = inspect(state.doc);
        if (info.protectedRanges.some(({ start, end, open }) => pos > start && (pos < end || (open && pos === end))))
            return null;
        const line = state.doc.lineAt(pos);
        const preceding = info.masked.slice(line.from, pos);
        const inImport = /^\s*import\s+([\w$.]*)$/.exec(preceding);
        if (inImport) {
            const prefix = inImport[1];
            return { from: pos - prefix.length, options: catalog.symbols.filter(item => item.qualified.includes(".") &&
                item.qualified.toLowerCase().startsWith(prefix.toLowerCase())).map(item => ({ label: item.qualified, type: item.kind })) };
        }
        const current = /[\w$]*$/.exec(preceding)[0];
        const from = pos - current.length;
        const after = state.doc.sliceString(pos, line.to).match(/^[\w$]*/)[0];
        const to = pos + after.length;
        const bindings = localBindings(info.masked.slice(0, from));
        const receiver = /\b((?:this\.)?[A-Za-z_$][\w$]*)\s*\.\s*$/.exec(info.masked.slice(line.from, from))?.[1];
        if (receiver) {
            let owner;
            const currentMembers = catalog.movie[className] ?? [];
            if (receiver === "this") owner = className;
            else if (receiver.startsWith("this.")) {
                const field = currentMembers.find(item => item.label === receiver.slice(5));
                owner = field && visibleClass(field.detail.match(/:\s*([\w$.]+)/)?.[1] ?? "", info, catalog);
            } else {
                const type = bindings.get(receiver);
                owner = visibleClass(type ?? receiver, info, catalog);
            }
            const members = catalog.movie[owner] ?? [];
            const staticAccess = receiver !== "this" && !receiver.startsWith("this.") && !bindings.has(receiver);
            const available = members.filter(item => (receiver === "this" || item.isPublic) &&
                (receiver === "this" || item.isStatic === staticAccess));
            return { from, to, options: available, validFor: /^[\w$]*$/ };
        }
        if (!explicit && current.length < 2) return null;
        const prefix = current.toLowerCase();
        const onPrefix = label => label.toLowerCase().startsWith(prefix);
        const options = [];
        for (const [label, detail] of bindings)
            if (onPrefix(label)) options.push({ label, detail, type: "variable", boost: 15 });
        for (const member of catalog.movie[className] ?? [])
            if (onPrefix(member.label) && !bindings.has(member.label)) options.push({ ...member, boost: 8 });
        for (const word of words)
            if (onPrefix(word)) options.push({ label: word, type: "keyword" });
        for (const symbol of catalog.symbols)
            if (onPrefix(symbol.label)) options.push({ label: symbol.label, detail: packageName(symbol.qualified), type: symbol.kind,
                boost: info.package === packageName(symbol.qualified) ? 4 : 0,
                apply: (view, completion, start, end) => importCompletion(view, completion, start, end, symbol, catalog) });
        return { from, to, options };
    };
}
