#pragma once

#include <string_view>

namespace flash::web
{
inline constexpr std::string_view text_runtime = R"JS(const NS = "http://www.w3.org/2000/svg";
const elements = new Set(["a", "b", "br", "em", "font", "i", "li", "p", "s", "span", "strong", "textformat", "u"]);
const discard = new Set(["script", "style", "iframe", "object", "embed", "svg", "math", "template"]);
const pixels = value => Math.max(0, Number.isFinite(Number(value)) ? Number(value) : 0);
const color = value => {
    if (typeof value === "number") return "#" + (value & 0xffffff).toString(16).padStart(6, "0");
    const css = String(value ?? "").trim();
    const hex = css.match(/^(?:#|0x)([\da-f]{1,6})$/i)?.[1] ?? css.match(/^([\da-f]{6})$/i)?.[1];
    return hex ? "#" + hex.padStart(6, "0") : css;
};

const embeddedFonts = new Map(), namedFonts = new Map();
export function loadFonts(scene, page) {
    const faces = [...Object.values(scene.fonts ?? {}), ...Object.values(scene.fontClasses ?? {})];
    return Promise.all(faces.filter(face => face.url).map(face => {
        const url = new URL(face.url, page).href;
        let entry = embeddedFonts.get(url);
        if (!entry) {
            const family = "flash-embedded-" + embeddedFonts.size;
            const weight = face.fontStyle.toLowerCase().includes("bold") ? "bold" : "normal";
            const style = face.fontStyle.toLowerCase().includes("italic") ? "italic" : "normal";
            const font = new FontFace(family, "url(" + JSON.stringify(url) + ")", { weight, style });
            entry = { family, ready: font.load().then(() => {
                document.fonts.add(font);
                fontHeights.clear();
            }).catch(() => {}) };
            embeddedFonts.set(url, entry);
        }
        if (face.fontName) namedFonts.set(face.fontName.toLowerCase() + ":" + face.fontStyle, entry.family);
        face.family = entry.family;
        return entry.ready;
    }));
}

export class TextFormat {
    constructor(font, size, color, bold, italic, underline, url, target, align, leftMargin, rightMargin, indent, leading) {
        Object.assign(this, { font, size, color, bold, italic, underline, url, target, align, leftMargin, rightMargin, indent, leading });
    }
}

export class StyleSheet {
    #styles = new Map();
    #listeners = new Set();
    #scheduled = false;
    #changed() {
        if (this.#scheduled || !this.#listeners.size) return;
        this.#scheduled = true;
        queueMicrotask(() => {
            this.#scheduled = false;
            for (const ref of this.#listeners) {
                const changed = ref.deref();
                if (changed) changed();
                else this.#listeners.delete(ref);
            }
        });
    }
    observe(listener) {
        const reference = new WeakRef(listener);
        this.#listeners.add(reference);
        return () => this.#listeners.delete(reference);
    }
    setStyle(name, value) {
        if (value == null) this.#styles.delete(name.toLowerCase());
        else this.#styles.set(name.toLowerCase(), { ...value });
        this.#changed();
    }
    getStyle(name) { const style = this.#styles.get(name.toLowerCase()); return style && { ...style } || null; }
    clear() { this.#styles.clear(); this.#changed(); }
    get styleNames() { return [...this.#styles.keys()]; }
    parseCSS(text) {
        for (const [, selectors, declarations] of String(text).matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
            const style = {};
            for (const entry of declarations.split(";")) {
                const colon = entry.indexOf(":");
                if (colon < 0) continue;
                const key = entry.slice(0, colon).trim().replace(/-([a-z])/g, (_, c) => c.toUpperCase());
                style[key] = entry.slice(colon + 1).trim();
            }
            for (const selector of selectors.trim().split(/\s*,\s*/))
                if (/^[.]?[\w-]+$/.test(selector)) this.setStyle(selector, style);
        }
    }
}

function formatting(node, style, resolveFont) {
    if (!style) return;
    const size = parseFloat(style.fontSize ?? style.size);
    if (Number.isFinite(size) && size > 0) node.style.fontSize = size + "px";
    const fill = color(style.color);
    if (fill && CSS.supports("color", fill)) node.style.color = fill;
    const family = style.fontFamily ?? style.font;
    if (family) node.style.fontFamily = resolveFont(family, style);
    if (style.bold || style.fontWeight === "bold") node.style.fontWeight = "bold";
    if (style.italic || style.fontStyle === "italic") node.style.fontStyle = "italic";
    if (style.underline || style.textDecoration === "underline") node.style.textDecoration = "underline";
    const align = style.textAlign ?? style.align;
    if (["left", "right", "center", "justify"].includes(align)) node.style.textAlign = align;
}

const measureContext = document.createElement("canvas").getContext("2d");
const fontHeights = new Map();
function fontSpec(style) {
    return [style.fontStyle || "normal", style.fontWeight || "normal", style.fontSize, style.fontFamily].join(" ");
}

function fontHeight(style) {
    const spec = fontSpec(style);
    if (!fontHeights.has(spec)) {
        measureContext.font = spec;
        const metrics = measureContext.measureText("M");
        const height = metrics.fontBoundingBoxAscent + metrics.fontBoundingBoxDescent;
        if (fontHeights.size >= 128) fontHeights.delete(fontHeights.keys().next().value);
        fontHeights.set(spec, height || parseFloat(style.fontSize) * 1.12);
    }
    return fontHeights.get(spec);
}

function appendHtml(target, html, sheet, emit, multiline, resolveFont) {
    const template = document.createElement("template");
    // Flash accepts nested text links, which the HTML parser otherwise moves outside their styled spans.
    template.innerHTML = html.replace(/<(\/?)[aA](?=[\s>])/g, "<$1flash-link");
    const add = (parent, item) => {
        if (item.nodeType === Node.TEXT_NODE) { parent.append(document.createTextNode(item.data)); return; }
        if (item.nodeType !== Node.ELEMENT_NODE || discard.has(item.localName)) return;
        const kind = item.localName === "flash-link" ? "a" : item.localName;
        if (kind === "tab" || kind === "br" || kind === "sbr") {
            if (kind === "tab") parent.append(document.createTextNode("\t"));
            else if (multiline) parent.append(document.createElement("br"));
            // The HTML parser treats Flash's self-closing sbr and tab as container tags.
            for (const child of item.childNodes) add(parent, child);
            return;
        }
        if (!elements.has(kind)) {
            for (const child of item.childNodes) add(parent, child);
            return;
        }
        const node = document.createElement(kind === "font" ? "span" : kind === "textformat" ? "div" : kind);
        if (kind === "a") { node.style.color = "inherit"; node.style.textDecoration = "none"; }
        formatting(node, sheet?.getStyle(kind), resolveFont);
        for (const name of item.classList) formatting(node, sheet?.getStyle("." + name), resolveFont);
        if (kind === "p") {
            node.style.margin = "0";
            const align = item.getAttribute("align")?.toLowerCase();
            if (["left", "right", "center", "justify"].includes(align)) node.style.textAlign = align;
            // Empty Flash paragraphs occupy a line, which is also used to align authored hotspots.
            if (multiline && !item.textContent && !item.querySelector("br,sbr"))
                node.append(document.createElement("br"));
        }
        if (kind === "font") {
            const size = item.getAttribute("size");
            const face = item.getAttribute("face");
            formatting(node, { color: item.getAttribute("color"), font: face,
                bold: /\bbold\b/i.test(face ?? ""), italic: /\b(?:italic|oblique)\b/i.test(face ?? ""),
                size: size && !/^[+-]/.test(size) ? size : null }, resolveFont);
            // In saved SWF HTML a face can switch from the field's bold font to a regular run.
            if (face && !node.style.fontWeight && !item.closest("b,strong")) node.style.fontWeight = "normal";
            if (face && !node.style.fontStyle && !item.closest("i,em")) node.style.fontStyle = "normal";
            const spacing = parseFloat(item.getAttribute("letterspacing"));
            if (Number.isFinite(spacing)) node.style.letterSpacing = spacing + "px";
            const kerning = item.getAttribute("kerning");
            if (kerning === "0" || kerning === "1") node.style.fontKerning = kerning === "1" ? "normal" : "none";
            if (size && /^[+-]\d+(\.\d+)?$/.test(size))
                node.style.fontSize = "calc(1em " + size[0] + " " + pixels(size.slice(1)) + "px)";
        }
        if (kind === "textformat")
            for (const [attribute, property] of [["leftmargin", "paddingLeft"], ["rightmargin", "paddingRight"], ["indent", "textIndent"]]) {
                const number = Number(item.getAttribute(attribute));
                if (Number.isFinite(number)) node.style[property] = number + "px";
            }
        if (kind === "a") {
            const href = item.getAttribute("href") ?? "";
            if (href.startsWith("event:")) {
                node.href = "#";
                node.addEventListener("click", event => {
                    event.preventDefault(); event.stopPropagation(); emit("link", { text: href.slice(6) });
                });
            } else if (href) {
                try {
                    const url = new URL(href, location.href);
                    if (url.protocol === "http:" || url.protocol === "https:") {
                        node.href = url.href;
                        if (item.getAttribute("target") === "_blank") {
                            node.target = "_blank";
                            node.rel = "noopener noreferrer";
                        }
                    }
                } catch { /* Invalid Flash link. */ }
            }
        }
        parent.append(node);
        for (const child of item.childNodes) add(node, child);
    };
    for (const item of template.content.childNodes) add(target, item);
}

)JS" R"JS(export function createTextField(definition = {}, emit = () => {}, face) {
    const [x, y, width, height] = (definition.bounds ?? [0, 0, 2000, 2000]).map(value => value / 20);
    const node = document.createElementNS(NS, "foreignObject");
    // Let the browser lay out text at its real font size inside a scene measured in twips.
    node.setAttribute("transform", "scale(20)");
    for (const [key, value] of Object.entries({ x, y, width, height })) node.setAttribute(key, value);
    const body = document.createElement("div"), content = document.createElement("div");
    const size = (definition.height ?? 240) / 20;
    const [left, right] = (definition.margins ?? [0, 0]).map(value => value / 20);
    const gutter = 2;
    const initialFormat = {
        size, color: definition.fill ?? "#000000",
        align: ["left", "right", "center", "justify"][definition.alignment] ?? "left",
        font: face?.fontName,
        bold: face?.fontStyle.toLowerCase().includes("bold"), italic: face?.fontStyle.toLowerCase().includes("italic"),
    };
    const values = {
        type: definition.editable ? "input" : "dynamic", displayAsPassword: Boolean(definition.password),
        selectable: definition.selectable !== false, wordWrap: Boolean(definition.wrap), multiline: Boolean(definition.multiline),
        border: Boolean(definition.border), background: Boolean(definition.border), borderColor: 0, backgroundColor: 0xffffff,
        maxChars: definition.maximum ?? 0, defaultTextFormat: initialFormat, styleSheet: null, autoSize: definition.autoSize ?? "none",
        embedFonts: Boolean(definition.embedFonts || definition.fontClass),
    };
    let html = Boolean(definition.html), source = definition.text ?? "";
    let fieldWidth = width, fieldHeight = height, horizontalShift = 0, measured, unobserve;
    Object.assign(body.style, {
        width: "100%", height: "100%", boxSizing: "border-box", overflow: "hidden",
    });
    Object.assign(content.style, { lineHeight: "1.12", fontFamily: "Arial, sans-serif", outline: "none", opacity: definition.alpha ?? 1 });
    body.append(content);
    node.append(body);
    const resolveFont = (family, style) => {
        const name = String(family).trim(), key = name.toLowerCase();
        const bold = style.bold || style.fontWeight === "bold";
        const italic = style.italic || style.fontStyle === "italic";
        const familyName = bold || italic ? name.replace(/(?:\s+(?:bold|italic|oblique))+$/i, "") : name;
        const device = ({ _sans: "sans-serif", _serif: "serif", _typewriter: "monospace",
            "arial bold": "Arial", "arial italic": "Arial", "arial bold italic": "Arial" })[key] ?? familyName;
        if (!values.embedFonts) return device;
        const variant = bold && italic ? "boldItalic" : bold ? "bold" : italic ? "italic" : "regular";
        const embedded = key === face?.fontName?.toLowerCase() && face.family ? face.family :
            namedFonts.get(key + ":" + variant) ?? namedFonts.get(key + ":regular");
        return embedded ? embedded + ", " + device : device;
    };

    function measure() {
        if (measured) return measured;
        if (!source) return measured = [0, 0];
        if (!html && !values.wordWrap && !/[\n\r\t]/.test(source)) {
            measureContext.font = fontSpec(content.style);
            const text = values.displayAsPassword ? "\u2022".repeat(source.length) : source;
            return measured = [measureContext.measureText(text).width + (parseFloat(content.style.textIndent) || 0),
                parseFloat(content.style.lineHeight)];
        }
        const sample = content.cloneNode(true);
        sample.removeAttribute("contenteditable");
        Object.assign(sample.style, {
            position: "absolute", visibility: "hidden", pointerEvents: "none", left: "-100000px", top: "0",
            minWidth: "0", width: "max-content",
            maxWidth: values.wordWrap ? Math.max(1, fieldWidth - paddingWidth()) + "px" : "none",
        });
        document.body.append(sample);
        const box = sample.getBoundingClientRect();
        measured = [box.width, box.height];
        sample.remove();
        return measured;
    }

    const paddingLeft = () => gutter + (values.defaultTextFormat?.leftMargin == null ? left : pixels(values.defaultTextFormat.leftMargin));
    const paddingRight = () => gutter + (values.defaultTextFormat?.rightMargin == null ? right : pixels(values.defaultTextFormat.rightMargin));
    const paddingWidth = () => paddingLeft() + paddingRight();

    function updateBounds() {
        const mode = values.autoSize;
        const automatic = mode === "left" || mode === "center" || mode === "right";
        const [textWidth, textHeight] = automatic ? measure() : [0, 0];
        const nextWidth = automatic && !values.wordWrap ? Math.max(0.05, Math.ceil((textWidth + paddingWidth()) * 20) / 20) : fieldWidth;
        const nextHeight = automatic ? Math.ceil((textHeight + 2 * gutter) * 20) / 20 : fieldHeight;
        const anchor = mode === "right" ? 1 : mode === "center" ? 0.5 : 0;
        horizontalShift = (fieldWidth - nextWidth) * anchor;
        for (const [key, value] of [["x", x + horizontalShift], ["width", nextWidth], ["height", nextHeight]])
            if (node.getAttribute(key) !== String(value)) node.setAttribute(key, value);
    }

    function renderContent() {
        measured = undefined;
        content.replaceChildren();
        if (values.displayAsPassword && values.type === "input") {
            const input = document.createElement("input");
            input.type = "password";
            input.value = source;
            input.maxLength = values.maxChars || 524288;
            Object.assign(input.style, { font: "inherit", color: "inherit", width: "100%", border: "0", background: "transparent" });
            input.addEventListener("input", () => { source = input.value; emit("change"); });
            content.removeAttribute("contenteditable");
            content.append(input);
        } else {
            content.contentEditable = values.type === "input" ? "plaintext-only" : "false";
            if (html) appendHtml(content, source, values.styleSheet, emit, values.multiline || values.wordWrap, resolveFont);
            else content.textContent = values.displayAsPassword ? "\u2022".repeat(source.length) : source;
        }
        updateBounds();
    }

    function render() {
        node.toggleAttribute("data-flash-passive-text", !values.selectable && values.type !== "input");
        content.style.whiteSpace = values.multiline || values.wordWrap ? "pre-wrap" : "pre";
        content.style.overflowWrap = values.wordWrap ? "anywhere" : "normal";
        body.style.userSelect = values.selectable || values.type === "input" ? "text" : "none";
        body.style.boxShadow = values.border ? "inset 0 0 0 1px " + color(values.borderColor) : "none";
        body.style.backgroundColor = values.background ? color(values.backgroundColor) : "transparent";
        body.style.padding = `${gutter}px ${paddingRight()}px ${gutter}px ${paddingLeft()}px`;
        content.style.fontSize = size + "px";
        Object.assign(content.style, { color: color(initialFormat.color), fontFamily: "Arial, sans-serif", fontWeight: "",
            fontStyle: "", textDecoration: "", textAlign: initialFormat.align,
            textIndent: (values.defaultTextFormat?.indent ?? (definition.indent ?? 0) / 20) + "px" });
        formatting(content, values.defaultTextFormat, resolveFont);
        const leading = values.defaultTextFormat?.leading;
        const lineHeight = values.multiline || values.wordWrap ? fontHeight(content.style) : parseFloat(content.style.fontSize) * 1.12;
        content.style.lineHeight = Math.max(0.05, lineHeight +
            (leading == null ? (definition.leading ?? 0) / 20 : Number(leading) || 0)) + "px";
        renderContent();
    }
    content.addEventListener("input", event => {
        if (event.target !== content) return;
        const text = content.innerText;
        source = values.maxChars ? text.slice(0, values.maxChars) : text;
        html = false;
        if (source !== text) renderContent();
        else { measured = undefined; updateBounds(); }
        emit("change");
    });
    content.addEventListener("keydown", event => {
        if (event.key === "Enter" && !values.multiline && event.target === content) event.preventDefault();
    });

    const field = { node, focus() {
        if (values.type !== "input") return false;
        const editor = content.firstElementChild?.matches("input") ? content.firstElementChild : content;
        editor.focus({ preventScroll: true });
        return document.activeElement === editor;
    }, setTextFormat(format) {
        const changes = Object.entries(format).filter(([key, value]) => value != null && values.defaultTextFormat?.[key] !== value);
        if (changes.length) { values.defaultTextFormat = { ...values.defaultTextFormat, ...Object.fromEntries(changes) }; render(); }
    } };
    const refreshStyles = () => { if (html) render(); };
    for (const key of Object.keys(values))
        Object.defineProperty(field, key, { enumerable: true, get: () => values[key], set: value => {
            if (values[key] === value) return;
            values[key] = value;
            if (key === "styleSheet") { unobserve?.(); unobserve = value?.observe?.(refreshStyles); }
            render();
        } });
    Object.defineProperties(field, {
        offsetX: { value: () => horizontalShift },
        text: { enumerable: true, get: () => html ? content.innerText : source, set: value => {
            value = String(value ?? "");
            if (!html && source === value) return;
            html = false;
            source = value;
            renderContent();
        } },
        htmlText: { enumerable: true, get: () => html ? source : content.innerHTML, set: value => {
            value = String(value ?? "");
            if (html && source === value) return;
            html = true;
            source = value;
            renderContent();
        } },
        textColor: { enumerable: true, get: () => {
            const value = values.defaultTextFormat?.color;
            return typeof value === "number" ? value & 0xffffff : Number.parseInt(color(value).slice(1), 16) || 0;
        }, set: value => {
            field.setTextFormat({ color: value });
        } },
        width: { enumerable: true, get: () => Number(node.getAttribute("width")),
            set: value => { fieldWidth = pixels(value); measured = undefined; updateBounds(); } },
        height: { enumerable: true, get: () => Number(node.getAttribute("height")),
            set: value => { fieldHeight = pixels(value); updateBounds(); } },
        textWidth: { enumerable: true, get: () => measure()[0] },
        textHeight: { enumerable: true, get: () => measure()[1] },
        length: { enumerable: true, get: () => field.text.length },
        scrollV: { enumerable: true, get: () => Math.floor(body.scrollTop / size) + 1, set: n => { body.scrollTop = Math.max(0, n - 1) * size; } },
        maxScrollV: { enumerable: true, get: () => Math.ceil(Math.max(0, body.scrollHeight - body.clientHeight) / size) + 1 },
    });
    render();
    return field;
}
)JS" R"JS(
const xmlObjects = new WeakMap();
export const isXml = (value, kind) => xmlObjects.has(value) && (kind === "XMLList" || xmlObjects.get(value).kind === "XML");

export function scriptXml(value, kind = "XML") {
    if (isXml(value, kind)) return value;
    const parser = new DOMParser();
    const source = String(value ?? "");
    const wrapped = kind === "XMLList";
    if (/<!DOCTYPE/i.test(source)) throw new SyntaxError("Type de document XML non pris en charge.");
    const document = parser.parseFromString(wrapped ? "<entries>" + source + "</entries>" :
        (source.trim() || "<empty/>"), "application/xml");
    if (document.getElementsByTagName("parsererror").length) throw new SyntaxError("XML ActionScript invalide.");
    return xmlView(wrapped ? [...document.documentElement.childNodes] : [document.documentElement], kind);
}

function xmlView(nodes, kind = "XMLList") {
    const children = (parent, name) => [...(parent.children ?? [])].filter(child => name === "*" || child.localName === name);
    const matching = name => nodes.flatMap(node => children(node, name));
    const attrs = name => nodes.flatMap(node => [...(node.attributes ?? [])].filter(attribute => name === "*" || attribute.localName === name));
    const serialized = () => nodes.map(node => node.nodeType === Node.ATTRIBUTE_NODE ? node.value :
        new XMLSerializer().serializeToString(node)).join("\n");
    const text = () => nodes.length === 1 && nodes[0].children?.length ? serialized() :
        nodes.filter(node => node.nodeType !== Node.COMMENT_NODE).map(node => node.textContent ?? "").join("");
    const api = {
        length: () => nodes.length,
        toString: text, toXMLString: serialized, valueOf: text,
        children: () => xmlView(nodes.flatMap(node => [...(node.childNodes ?? [])])),
        child: name => xmlView(matching(String(name))),
        attribute: name => xmlView(attrs(String(name))),
        attributes: () => xmlView(attrs("*")),
        descendants: (name = "*") => xmlView(nodes.flatMap(node => [...(node.getElementsByTagName?.("*") ?? [])]
            .filter(child => name === "*" || child.localName === name))),
        name: () => nodes[0]?.nodeName ?? null,
        localName: () => nodes[0]?.localName ?? null,
        text: () => xmlView(nodes.flatMap(node => [...(node.childNodes ?? [])].filter(child => child.nodeType === Node.TEXT_NODE))),
    };
    const view = new Proxy(Object.create(null), {
        get(_, key) {
            if (key === Symbol.toPrimitive) return text;
            if (typeof key !== "string") return undefined;
            if (Object.hasOwn(api, key)) return api[key];
            if (/^(0|[1-9]\d*)$/.test(key)) return nodes[key] ? xmlView([nodes[key]], "XML") : undefined;
            if (key.startsWith("@")) return xmlView(attrs(key.slice(1)));
            return xmlView(matching(key));
        },
        set(_, key, value) {
            if (typeof key !== "string" || key === "@*" || !key.startsWith("@")) return false;
            for (const node of nodes) node.setAttribute?.(key.slice(1), String(value));
            return true;
        },
    });
    xmlObjects.set(view, { kind });
    return view;
}
)JS";
}
