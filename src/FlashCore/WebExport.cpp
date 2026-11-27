#include "Binary.hpp"
#include "WebExport.hpp"
#include "WebClassRuntime.hpp"
#include "WebText.hpp"
#include "WebMedia.hpp"
#include "WebServer.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <fstream>
#include <format>
#include <mutex>
#include <regex>
#include <thread>
#include <utility>

#pragma comment(lib, "bcrypt.lib")

namespace flash::web
{
std::string json_string(std::string_view value)
{
    std::string text = "\"";
    for (const unsigned char c : value)
    {
        if (c == '"' || c == '\\')
        {
            text.push_back('\\');
            text.push_back(static_cast<char>(c));
        }
        else if (c < 32)
            text += std::format("\\u{:04x}", c);
        else
            text.push_back(static_cast<char>(c));
    }
    return text + '"';
}

void omission(ExportReport& report, std::string message)
{
    ++report.failed;
    if (report.errors.size() < 250)
        report.errors.push_back(std::move(message));
}

namespace
{
std::string html_text(std::string_view value)
{
    std::string escaped;
    for (const auto c : value)
        switch (c)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        default: escaped.push_back(c); break;
        }
    return escaped;
}

constexpr std::string_view page = R"HTML(<!doctype html>
<html lang="fr">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{}</title>
  <link rel="stylesheet" href="{}">
</head>
<body>
  <main>
    <svg id="stage" tabindex="-1" aria-label="Scène convertie"><defs id="symbols"></defs><g id="art"></g></svg>
    <div class="controls">
      <button id="play" type="button" disabled>Lire</button>
      <span id="status" role="status">Chargement de la scène…</span>
    </div>
    <details id="linked" hidden><summary>Animations liées</summary><ul></ul></details>
  </main>
  <script type="module" src="{}"></script>
</body>
</html>
)HTML";

constexpr std::string_view css = R"CSS(:root { color-scheme: dark; font: 14px/1.5 system-ui, sans-serif; background: #121418; }
* { box-sizing: border-box; }
body { margin: 0; }
main { min-height: 100dvh; display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 10px; padding: 16px; }
#stage { display: block; width: min(100%, var(--scene-width)); height: auto; max-height: calc(100dvh - 84px); background: #fff; }
.controls { display: flex; align-items: center; gap: 12px; color: #d6ddea; }
button { font: inherit; padding: 5px 18px; border: 1px solid #56637b; border-radius: 5px; background: #242c3a; color: #fff; }
button:focus-visible { outline: 2px solid #9cc9ff; outline-offset: 2px; }
#stage [role=button] { cursor: pointer; outline-offset: 2px; }
#stage[data-flash-hide-cursor], #stage[data-flash-hide-cursor] * { cursor: none !important; }
#stage [data-flash-mouse="on"] { pointer-events: visiblePainted; }
#stage [data-flash-mouse="on"][data-flash-pointer-listener] { pointer-events: bounding-box; }
#stage [data-flash-mouse="self"] { pointer-events: bounding-box; }
#stage [data-flash-mouse="off"] { pointer-events: none; }
#stage foreignObject[data-flash-passive-text] { pointer-events: none; }
#stage [data-flash-pointer-listener]:not([data-flash-mouse="off"]) > foreignObject[data-flash-passive-text],
#stage [data-flash-mouse="on"] > foreignObject[data-flash-passive-text] a[href] { pointer-events: auto; }
#stage [data-flash-static] { pointer-events: none; }
#stage [data-flash-blend-hoisted] { mix-blend-mode: normal !important; }
#stage [data-flash-composite] { opacity: 0 !important; }
#stage [data-flash-composite], #stage [data-flash-composite] * { pointer-events: none !important; }
#stage [data-flash-hit] :is(image, use, path, rect, ellipse, circle) { pointer-events: visiblePainted; }
#stage[data-flash-quality="low"] { shape-rendering: crispEdges; }
#stage[data-flash-quality="low"] :is(image, canvas) { image-rendering: pixelated !important; }
#stage :is([data-flash-no-children], [data-flash-decoration]) * { pointer-events: none !important; }
#linked { color: #d6ddea; max-width: min(90vw, 720px); }
.controls a, #linked a, .catalogue a { color: #9cc9ff; overflow-wrap: anywhere; }
.catalogue { max-width: 760px; margin: auto; align-items: stretch; justify-content: flex-start; }
.catalogue input { font: inherit; padding: 8px 10px; }
html[data-embedded] main { min-height: 0; padding: 0; }
html[data-embedded] #stage { width: 100%; max-height: none; }
html[data-embedded] :is(.controls, #linked) { display: none; }
)CSS";

constexpr std::string_view stage_script = R"JS(
import { ConvertedClasses, convertedClassName, convertedMethodReceiver } from "./as3.js";
import { createTextField, loadFonts, StyleSheet, TextFormat, scriptXml, isXml } from "./text.js";
import { mediaAdapters, ByteArray, URLRequest, URLRequestHeader, URLVariables, SoundTransform } from "./media.js";

const documentUrl = new URL(location.href);
const [{ actions }, { definitions }] = await Promise.all([
    import(new URL("actions.js", documentUrl).href), import(new URL("classes.js", documentUrl).href),
]);
const stage = document.querySelector("#stage");
const symbols = document.querySelector("#symbols");
const art = document.querySelector("#art");
const button = document.querySelector("#play");
const status = document.querySelector("#status");
if (new URLSearchParams(location.search).has("embedded")) document.documentElement.dataset.embedded = "";
const NS = "http://www.w3.org/2000/svg";
const element = (name, attributes = {}) => {
    const created = document.createElementNS(NS, name);
    for (const [key, value] of Object.entries(attributes)) created.setAttribute(key, value);
    return created;
};

let data, root, mainBundle, catalog, running = false, entered = false, previous = 0, accumulated = 0, masks = 0;
let sockets = Object.create(null), httpRoutes = Object.create(null);
let movieRoutes = Object.create(null);
let lazyAssetPaths = [];
const movieRequests = new Map();
const cachedMovieSizes = new WeakMap();
const movieByteBudget = 24 * 1024 * 1024;
let cachedMovieBytes = 0;
const movieBytes = new WeakMap();
let bundleSequence = 0;
const rasterImages = new Map();
const linkedPixels = new WeakMap();
const pixelMapImages = new Map();
const defined = new Map();
const assetsById = new Map();
const clonedAssetUses = Symbol("clonedAssetUses");
let assetSequence = 0, rasterBytes = 0;
const decodedRasterBudget = 96 * 1024 * 1024;
const assetUsers = new FinalizationRegistry(entry => {
    if (--entry.users !== 0) return;
    if (defined.get(entry.key) === entry) defined.delete(entry.key);
    assetsById.delete(entry.node.id);
    entry.node.remove();
});
const filters = new Map();
const sounds = new Map();
const activeStreams = new Set();
const pendingBitmapDraws = new Set();
const unnamedLayers = new Map();
const bitmapTints = new WeakMap();
const authoredTints = new WeakMap();
const bitmapSurfaces = new WeakMap();
const bitmapPatterns = new WeakMap();
const playbackEpoch = performance.now();
const background = element("rect");
art.before(background);

function asset(id, bundle) {
    const definition = bundle.scene.assets[id];
    if (!definition) return element("g");
    const href = new URL(definition.url, bundle.page).href;
    const key = href + "\0" + definition.bounds.join(",");
    let entry = defined.get(key);
    if (!entry) {
        const [x, y, width, height] = definition.bounds;
        const node = element("image", { href, x, y, width, height });
        node.id = "asset-" + assetSequence++;
        entry = { key, node, users: 0 };
        defined.set(key, entry);
        assetsById.set(node.id, entry);
        symbols.append(node);
    }
    const use = element("use", { href: "#" + entry.node.id });
    ++entry.users;
    assetUsers.register(use, entry);
    return use;
}

function timelineAsset(row, scene) {
    const variant = row[11] && row[1] + "@" + row[11];
    return variant && scene.assets[variant] ? variant : row[1];
}

function cloneDisplay(node) {
    const copy = node.cloneNode(true);
    const registered = [];
    const register = use => {
        const id = use.getAttribute("href");
        const entry = id?.startsWith("#") && assetsById.get(id.slice(1));
        if (entry) { ++entry.users; assetUsers.register(use, entry); registered.push(use); }
    };
    if (copy.localName === "use") register(copy);
    for (const use of copy.querySelectorAll("use")) register(use);
    copy[clonedAssetUses] = registered;
    return copy;
}

function rasterImage(url) {
    const cached = rasterImages.get(url);
    if (cached) {
        rasterImages.delete(url);
        rasterImages.set(url, cached);
        return cached.image;
    }
    const image = new Image(), entry = { image, bytes: 0 };
    const trim = () => {
        while (rasterBytes > decodedRasterBudget || rasterImages.size > 2048) {
            const first = rasterImages.keys().next().value;
            rasterBytes -= rasterImages.get(first).bytes;
            rasterImages.delete(first);
        }
    };
    image.addEventListener("load", () => {
        if (rasterImages.get(url) !== entry) return;
        entry.bytes = image.naturalWidth * image.naturalHeight * 4;
        rasterBytes += entry.bytes;
        trim();
    }, { once: true });
    image.addEventListener("error", () => {
        if (rasterImages.get(url) === entry) rasterImages.delete(url);
    }, { once: true });
    rasterImages.set(url, entry);
    image.src = url;
    trim();
    return image;
}

function loadPixelMapImages(scene, page) {
    const roots = new Set();
    for (const clip of Object.values(scene.clips))
        for (const [rows] of clip.frames)
            for (const row of rows)
                if (row[4] === "surfaceMap" || row[4] === "environmentMap") roots.add(row[1]);
    const visited = new Set(), pending = [];
    const visit = id => {
        if (visited.has(id)) return;
        visited.add(id);
        const clip = scene.clips[id];
        if (clip) for (const [rows] of clip.frames) for (const row of rows) visit(row[1]);
        const asset = scene.assets[id]?.url;
        if (!asset?.endsWith(".svg")) return;
        const address = new URL(asset, page).href;
        let raster = pixelMapImages.get(address);
        if (!raster) {
            const image = new Image();
            const ready = fetch(address).then(async response => {
                if (!response.ok) return;
                const svg = (await response.text()).replace(/<svg(?=[\s>])/, '<svg shape-rendering="crispEdges"');
                const objectUrl = URL.createObjectURL(new Blob([svg], { type: "image/svg+xml" }));
                try { image.src = objectUrl; await image.decode(); }
                finally { URL.revokeObjectURL(objectUrl); }
            }).catch(() => {});
            raster = { image, ready };
            pixelMapImages.set(address, raster);
            if (pixelMapImages.size > 128) pixelMapImages.delete(pixelMapImages.keys().next().value);
        }
        pending.push(raster.ready);
    };
    for (const id of roots) visit(id);
    return Promise.all(pending);
}

)JS" R"JS(function svgFilter(transform, effects, units = 20) {
    const key = JSON.stringify([transform, effects, units]);
    if (filters.has(key)) return filters.get(key);
    const id = "effect-" + filters.size;
    const filter = element("filter", { id, "color-interpolation-filters": "sRGB",
        ...(effects.length ? { x: "-100%", y: "-100%", width: "300%", height: "300%" } :
            { x: "0%", y: "0%", width: "100%", height: "100%" }) });
    let input = "SourceGraphic";
    if (transform) {
        const transfer = element("feComponentTransfer", { in: input, result: "colored" });
        for (const [index, channel] of [..."RGBA"].entries())
            transfer.append(element("feFunc" + channel, { type: "linear",
                slope: transform[index] / 256, intercept: transform[index + 4] / 255 }));
        filter.append(transfer);
        input = "colored";
        if (transform[7] > 0) input = preserveCoverage(filter, input);
    }
    for (const [index, effect] of effects.entries()) {
        const result = "effect" + index;
        const [kind, x, y, a, b, color, alpha, flags, passes] = effect;
        if (kind === 3) {
            bevelPrimitives(filter, input, result, effect, units);
        } else if (kind === 6) {
            filter.append(element("feColorMatrix", { in: input, type: "matrix", values: effect.slice(1).join(" "), result }));
        } else if (kind === 5) {
            const [, columns, rows, divisor, bias, options, kernel] = effect;
            filter.append(element("feConvolveMatrix", { in: input, order: columns + " " + rows, divisor, bias,
                kernelMatrix: kernel.join(" "), edgeMode: options & 2 ? "duplicate" : "none",
                preserveAlpha: options & 1 ? "true" : "false", result }));
        } else {
            // Match the variance of the Flash box-blur passes in the SVG coordinate system.
            const deviation = units * Math.sqrt(Math.max(1, kind === 1 ? a : passes) / 12);
            const stdDeviation = [x * deviation, y * deviation].join(" ");
            if (kind === 1) {
                filter.append(element("feGaussianBlur", { in: input, stdDeviation, edgeMode: "none", result }));
            } else if (alpha <= 1 && !(flags & (128 | 64)) && (flags & 32)) {
                filter.append(element("feDropShadow", { in: input, stdDeviation, dx: a * units, dy: b * units,
                    "flood-color": color, "flood-opacity": Math.min(alpha, 1), result }));
            } else {
                const inner = Boolean(flags & 128);
                let mask = input;
                if (inner) {
                    filter.append(element("feComponentTransfer", { in: input, result: result + "-inverse" }));
                    filter.lastChild.append(element("feFuncA", { type: "linear", slope: -1, intercept: 1 }));
                    mask = result + "-inverse";
                }
                const blurred = result + "-blur";
                filter.append(element("feGaussianBlur", { in: mask, stdDeviation, edgeMode: "none", result: blurred }));
                let coverage = blurred;
                if (alpha > 1) {
                    coverage = result + "-strong";
                    const strength = element("feComponentTransfer", { in: blurred, result: coverage });
                    strength.append(element("feFuncA", { type: "linear", slope: alpha }));
                    filter.append(strength);
                }
                filter.append(element("feOffset", { in: coverage, dx: a * units, dy: b * units, result: result + "-offset" }));
                filter.append(element("feFlood", { "flood-color": color, "flood-opacity": Math.min(alpha, 1), result: result + "-fill" }));
                filter.append(element("feComposite", { in: result + "-fill", in2: result + "-offset", operator: "in",
                    result: result + "-tint" }));
                if (inner || (flags & 64)) {
                    filter.append(element("feComposite", { in: result + "-tint", in2: input,
                        operator: inner ? "in" : "out", result: result + "-clip" }));
                }
                const shadow = inner || (flags & 64) ? result + "-clip" : result + "-tint";
                const merge = element("feMerge", { result });
                const showSource = !(flags & 64) && Boolean(flags & 32);
                if (showSource && inner) merge.append(element("feMergeNode", { in: input }));
                merge.append(element("feMergeNode", { in: shadow }));
                if (showSource && !inner) merge.append(element("feMergeNode", { in: input }));
                filter.append(merge);
            }
        }
        input = result;
    }
    symbols.append(filter);
    const url = "url(#" + id + ")";
    filters.set(key, url);
    return url;
}

function preserveCoverage(filter, input) {
    const coverage = element("feComponentTransfer", { in: "SourceAlpha", result: "coverage" });
    coverage.append(element("feFuncA", { type: "linear", slope: 255 }));
    filter.append(coverage, element("feComposite", { in: input, in2: "coverage", operator: "in", result: "covered" }));
    return "covered";
}

function bevelPrimitives(filter, input, result, effect, units) {
    const [, x, y, dx, dy, highlight, shadow, highAlpha, lowAlpha, quality, type, knockout] = effect;
    const blur = result + "-blur", toward = result + "-toward", away = result + "-away";
    const deviation = units * Math.sqrt(quality / 12);
    filter.append(element("feGaussianBlur", { in: input, stdDeviation: [x * deviation, y * deviation].join(" "), result: blur }),
        element("feOffset", { in: blur, dx: dx * units, dy: dy * units, result: toward }),
        element("feOffset", { in: blur, dx: -dx * units, dy: -dy * units, result: away }));
    const merge = element("feMerge", { result });
    if (!knockout) merge.append(element("feMergeNode", { in: input }));
    for (const [name, from, subtract, color, alpha] of [
        [result + "-highlight", toward, away, highlight, highAlpha], [result + "-shadow", away, toward, shadow, lowAlpha],
    ]) {
        filter.append(element("feComposite", { in: from, in2: subtract, operator: "arithmetic", k2: alpha, k3: -alpha, result: name + "-edge" }),
            element("feFlood", { "flood-color": color, result: name + "-color" }),
            element("feComposite", { in: name + "-color", in2: name + "-edge", operator: "in", result: name }));
        if (type !== "full") filter.append(element("feComposite", { in: name, in2: input,
            operator: type === "outer" ? "out" : "in", result: name }));
        merge.append(element("feMergeNode", { in: name }));
    }
    filter.append(merge);
}

function playSound([id, from, to, loops, unique, stop], bundle) {
    const url = bundle.scene.audio[id];
    if (!url) return;
    const key = bundle.id + ":" + id;
    let active = sounds.get(key);
    if (stop) {
        if (active) for (const channel of active) channel.stop();
        sounds.delete(key);
        return;
    }
    if (unique && active?.size) return;
    if (!active) { active = new Set(); sounds.set(key, active); }
    const sound = browserMedia.embeddedSound(new URL(url, bundle.page).href, bundle.scene.audioTiming?.[id]);
    const channel = sound.play(from / 44.1, Math.max(0, loops - 1), undefined, to / 44.1);
    active.add(channel);
    const clear = () => {
        active.delete(channel);
        if (!active.size && sounds.get(key) === active) sounds.delete(key);
    };
    channel.addEventListener("soundComplete", clear);
    sound.addEventListener("ioError", clear);
}

function makeButton(definition, node, parent, ancestry) {
    let state = 0, pointer = null, lastHit, hitCopy, enabled = true, useHandCursor = true;
    const drawing = element("g"), hitArea = element("g", { opacity: 0 });
    drawing.style.pointerEvents = "none";
    drawing.setAttribute("data-flash-decoration", "");
    hitArea.setAttribute("data-flash-hit", "");
    node.append(drawing, hitArea);
    const view = {};
    const states = [];
    const getState = index => {
        if (index in states) return states[index];
        const rows = definition.states[index];
        if (!rows.length) return states[index] = null;
        if (rows.length === 1) return states[index] = parent.makeLayer(rows[0], ancestry).view;
        const group = scriptSprite("Sprite");
        const target = displayNodes.get(group);
        for (const row of rows) target.append(parent.makeLayer(row, ancestry).node);
        return states[index] = group;
    };
    const originalHit = getState(3) ?? getState(0);
    const select = value => {
        state = value;
        const image = displayNodes.get(getState(state) ?? getState(0));
        if (drawing.firstChild !== (image ?? null)) drawing.replaceChildren(...(image ? [image] : []));
        const hit = displayNodes.get(getState(3) ?? originalHit);
        if (hit !== lastHit) {
            hitCopy = hit ? cloneDisplay(hit) : null;
            hitArea.replaceChildren(...(hitCopy ? [hitCopy] : []));
            lastHit = hit;
        }
    };
    for (const [index, key] of ["upState", "overState", "downState", "hitTestState"].entries())
        Object.defineProperty(view, key, { get: () => getState(index), set: value => { states[index] = value; select(state); } });
    const appearance = () => {
        hitArea.style.pointerEvents = enabled ? "" : "none";
        hitArea.toggleAttribute("data-flash-decoration", !enabled);
        node.style.cursor = enabled && useHandCursor ? "pointer" : "default";
    };
    Object.defineProperties(view, {
        enabled: { get: () => enabled, set: value => { enabled = Boolean(value); appearance(); } },
        useHandCursor: { get: () => useHandCursor, set: value => { useHandCursor = Boolean(value); appearance(); } },
    });
    select(0);
    const fire = index => parent.run(definition.handlers[index]);
    node.setAttribute("role", "button");
    node.setAttribute("tabindex", "0");
    node.addEventListener("pointerenter", () => { if (pointer === null) { select(1); fire(2); } });
    node.addEventListener("pointerleave", () => {
        if (pointer !== null) { pointer = null; fire(4); }
        select(0);
        fire(3);
    });
    node.addEventListener("pointerdown", event => {
        if (event.button !== 0 || !enabled) return;
        pointer = event.pointerId;
        select(2);
        fire(1);
    });
    node.addEventListener("pointercancel", () => { pointer = null; select(0); });
    node.addEventListener("pointerup", event => {
        if (pointer !== event.pointerId) return;
        pointer = null;
        if (!enabled) return;
        select(1);
        fire(0);
        node.dispatchEvent(new MouseEvent("click", event));
    });
    node.addEventListener("click", event => {
        // Flash activates this button on release even when the browser retargets its native click.
        if (event.isTrusted && event.detail > 0) { event.preventDefault(); event.stopImmediatePropagation(); }
    }, true);
    node.addEventListener("keydown", event => {
        if (!event.repeat && enabled && (event.key === "Enter" || event.key === " ")) {
            event.preventDefault();
            fire(0);
            node.dispatchEvent(new MouseEvent("click", { bubbles: true, cancelable: true }));
        }
    });
    return view;
}

)JS" R"JS(const timelineMethods = new Set(["play", "stop", "gotoAndPlay", "gotoAndStop", "nextFrame", "prevFrame", "addFrameScript"]);
const coordinates = new Set(["x", "y", "alpha", "scaleX", "scaleY", "rotation"]);
const visualProperties = new Set([...coordinates, "visible", "text", "buttonMode", "name", "blendMode"]);
const blendModes = { add: "plus-lighter", layer: "isolate", hardlight: "hard-light" };
function displayBlend(node, mode) {
    const composite = mode === "alpha" || mode === "erase";
    const previous = node.getAttribute("data-flash-composite");
    if (composite && previous !== mode) node.setAttribute("data-flash-composite", mode);
    else if (!composite && previous) node.removeAttribute("data-flash-composite");
    const isolation = mode === "isolate" ? "isolate" : "";
    if (node.style.isolation !== isolation) node.style.isolation = isolation;
    const blend = !composite && mode && mode !== "normal" && mode !== "isolate" ? mode : "";
    if (node.style.mixBlendMode !== blend) node.style.mixBlendMode = blend;
    if (node.parentElement && (composite || previous)) updateBlendMask(node.parentElement);
}
const displayViews = new WeakMap();
const displayTypes = new WeakMap();
const displayNodes = new WeakMap();
const displayParents = new WeakMap();
const displayOwners = new WeakMap();
const scriptedNodes = new Set();
// Flash broadcasts ENTER_FRAME to display objects even when they are off stage.
const frameListeners = new Set();
const frameReferences = new WeakMap();
const authoredClips = new WeakMap();
const scriptMasks = new WeakMap();
const activeScriptMasks = new Set();
const blendMasks = new WeakMap();
const maskElements = new FinalizationRegistry(mask => mask.remove());
const maskSources = new WeakMap();
const reportedErrors = new Set();
const eventKinds = new WeakMap();
const eventObjects = new WeakMap();
const objectEvents = new WeakMap();
const activeEvents = new WeakMap();
const generatedEventKinds = new Map(Object.entries({
    link: "TextEvent", textInput: "TextEvent", timer: "TimerEvent", timerComplete: "TimerEvent",
    progress: "ProgressEvent", socketData: "ProgressEvent", data: "DataEvent", status: "StatusEvent",
    httpStatus: "HTTPStatusEvent", httpResponseStatus: "HTTPStatusEvent",
    ioError: "IOErrorEvent", securityError: "SecurityErrorEvent", asyncError: "AsyncErrorEvent",
}));
const eventParents = new Map(Object.entries({
    IOErrorEvent: "ErrorEvent", SecurityErrorEvent: "ErrorEvent", AsyncErrorEvent: "ErrorEvent",
    ErrorEvent: "TextEvent", DataEvent: "TextEvent",
}));
const domEvent = type => ({
    mouseDown: "mousedown", mouseUp: "mouseup", mouseMove: "mousemove", doubleClick: "dblclick",
    mouseOver: "mouseover", mouseOut: "mouseout", rollOver: "pointerenter", rollOut: "pointerleave", mouseWheel: "wheel",
    keyDown: "keydown", keyUp: "keyup", focusIn: "focusin", focusOut: "focusout",
}[type] ?? type);

function displayObjectFor(node) {
    for (let current = node; current; current = current.parentElement) {
        const view = displayViews.get(current);
        if (view) return view;
    }
    return null;
}

let pointerX = 0, pointerY = 0;
let pointerEditor;
const trackPointer = event => { pointerX = event.clientX; pointerY = event.clientY; };
for (const type of ["pointermove", "pointerdown", "pointerup"])
    stage.addEventListener(type, trackPointer, { capture: true, passive: true });

const clickTargets = new WeakSet();
let pressedClip, releasedClip;
const clickOwner = event => event.composedPath().find(node => clickTargets.has(node));
stage.addEventListener("pointerdown", event => {
    if (event.isPrimary && event.button === 0) pressedClip = clickOwner(event);
}, true);
stage.addEventListener("pointercancel", event => { if (event.isPrimary) pressedClip = null; }, true);
stage.addEventListener("pointerup", event => {
    if (!event.isPrimary || event.button !== 0) return;
    const target = pressedClip;
    pressedClip = null;
    if (!target || target !== clickOwner(event)) return;
    const release = { target };
    releasedClip = release;
    // A MovieClip can swap its drawing during a press and lose the DOM click.
    setTimeout(() => {
        if (releasedClip !== release) return;
        releasedClip = null;
        if (target.isConnected) target.dispatchEvent(new MouseEvent("click", event));
    }, 0);
}, true);
stage.addEventListener("click", event => {
    if (releasedClip?.target === clickOwner(event)) releasedClip = null;
}, true);

function positionIn(node, clientX = pointerX, clientY = pointerY) {
    let screen = node.isConnected ? node.getScreenCTM() : stage.getScreenCTM();
    if (!node.isConnected && screen) {
        const detached = [];
        for (let parent = node; parent instanceof SVGGraphicsElement; parent = parent.parentElement)
            detached.push(parent);
        for (let cursor = detached.length - 1; cursor >= 0; --cursor) {
            const list = detached[cursor].transform.baseVal;
            for (let index = 0; index < list.numberOfItems; ++index) screen = screen.multiply(list.getItem(index).matrix);
        }
    }
    const matrix = screen?.inverse();
    return matrix ? new Point((matrix.a * clientX + matrix.c * clientY + matrix.e) / 20,
        (matrix.b * clientX + matrix.d * clientY + matrix.f) / 20) : new Point();
}

function transformPoint(point, from, to) {
    const source = from.getScreenCTM(), destination = to.getScreenCTM()?.inverse();
    if (!source || !destination || !Number.isFinite(destination.a)) return new Point(point.x, point.y);
    const matrix = destination.multiply(source);
    return new Point(matrix.a * point.x + matrix.c * point.y + matrix.e / 20,
        matrix.b * point.x + matrix.d * point.y + matrix.f / 20);
}

// Changes to the display list already reach the browser's next paint.
function updateAfterEvent() {}

function pointerAccess(node, interactive) {
    let enabled = true, children = true;
    const update = () => {
        if (interactive || !enabled) node.dataset.flashMouse = !enabled ? "off" : children ? "on" : "self";
        else delete node.dataset.flashMouse;
        node.toggleAttribute("data-flash-no-children", !children);
    };
    if (interactive) update();
    return {
        get mouseEnabled() { return enabled; },
        set mouseEnabled(value) { if (enabled !== Boolean(value)) { enabled = Boolean(value); update(); } },
        get mouseChildren() { return children; },
        set mouseChildren(value) { if (children !== Boolean(value)) { children = Boolean(value); update(); } },
    };
}

const pointerEvents = new Set(["click", "dblclick", "mousedown", "mouseup", "mousemove", "mouseover", "mouseout",
    "pointerenter", "pointerleave", "wheel"]);

function scriptProblem(error) {
    const message = error instanceof Error ? error.message : String(error);
    if (!reportedErrors.has(message)) {
        reportedErrors.add(message);
        console.error(error);
        status.textContent = message;
    }
}

function scriptEvent(type, bubbles = false, cancelable = false, detail, kind = generatedEventKinds.get(type) ?? "Event") {
    const event = new CustomEvent(domEvent(type), { bubbles, cancelable, detail });
    eventKinds.set(event, kind);
    return event;
}

function eventKind(event) {
    return eventKinds.get(event) ??
        (event instanceof MouseEvent ? "MouseEvent" : event instanceof KeyboardEvent ? "KeyboardEvent" :
            event instanceof FocusEvent ? "FocusEvent" : event instanceof Event ? "Event" : null);
}

function isEvent(value, type) {
    for (let kind = eventKind(value); kind; kind = kind === "Event" ? null : eventParents.get(kind) ?? "Event")
        if (kind === type) return true;
    return false;
}

function inheritedEvent() {
    let view, event;
    const active = () => activeEvents.get(view) ?? event;
    return {
        bindView(instance) { view = instance; },
        initialize(type, bubbles, cancelable) {
            event = scriptEvent(type, bubbles, cancelable, undefined, "Event");
            eventKinds.set(view, "Event");
            eventObjects.set(event, view);
            objectEvents.set(view, event);
        },
        get type() { return event?.type; },
        get bubbles() { return event?.bubbles ?? false; },
        get cancelable() { return event?.cancelable ?? false; },
        get target() { return displayObjectFor(active()?.target); },
        get currentTarget() { return displayViews.get(active()?.currentTarget) ?? null; },
        preventDefault() { active()?.preventDefault(); },
        isDefaultPrevented() { return active()?.defaultPrevented ?? false; },
        stopPropagation() { active()?.stopPropagation(); },
        stopImmediatePropagation() { active()?.stopImmediatePropagation(); },
    };
}

)JS" R"JS(function changeMask(target, view) {
    const prior = scriptMasks.get(target);
    if (prior) {
        prior.definition.remove();
        target.removeAttribute("mask");
        activeScriptMasks.delete(prior.handle);
        prior.observer.disconnect();
        const count = maskSources.get(prior.node) - 1;
        if (count) maskSources.set(prior.node, count);
        else { maskSources.delete(prior.node); prior.node.style.removeProperty("visibility"); }
        scriptMasks.delete(target);
    }
    const node = displayNodes.get(view);
    if (!node || node === target || node.contains(target)) return;
    const id = "script-mask-" + (++masks);
    const definition = element("mask", { id, "mask-type": "alpha", maskUnits: "userSpaceOnUse",
        x: "-1000000", y: "-1000000", width: "2000000", height: "2000000" });
    symbols.append(definition);
    const entry = { view, node, definition, target: new WeakRef(target), dirty: true, matrix: null };
    entry.handle = { ref: new WeakRef(entry), definition, source: new WeakRef(node) };
    entry.observer = new MutationObserver(records => {
        if (records.some(record => record.target !== node || record.attributeName !== "transform")) entry.dirty = true;
    });
    entry.observer.observe(node, { attributes: true, childList: true, subtree: true, characterData: true });
    scriptMasks.set(target, entry);
    activeScriptMasks.add(entry.handle);
    maskSources.set(node, 1 + (maskSources.get(node) ?? 0));
    node.style.visibility = "hidden";
    target.setAttribute("mask", "url(#" + id + ")");
}

function updateScriptMasks() {
    const transforms = new WeakMap();
    const toStage = node => {
        if (!node || node === stage) return new DOMMatrix();
        if (!transforms.has(node)) transforms.set(node, toStage(node.parentElement).multiply(localDisplayMatrix(node)));
        return transforms.get(node);
    };
    for (const handle of activeScriptMasks) {
        const entry = handle.ref.deref();
        const target = entry?.target.deref();
        if (!target) {
            entry?.observer.disconnect();
            handle.definition.remove();
            activeScriptMasks.delete(handle);
            const source = handle.source.deref();
            if (!source) continue;
            const count = maskSources.get(source) - 1;
            if (count) maskSources.set(source, count);
            else { maskSources.delete(source); source.style.removeProperty("visibility"); }
            continue;
        }
        if (!target.isConnected) continue;
        const destination = toStage(target), source = toStage(entry.node);
        const relative = destination.inverse().multiply(source);
        const values = [relative.a, relative.b, relative.c, relative.d, relative.e, relative.f];
        if (!values.every(Number.isFinite)) continue;
        if (entry.dirty) {
            entry.graphic = cloneMaskGraphic(entry.node);
            entry.graphic.style.removeProperty("visibility");
            entry.definition.replaceChildren(entry.graphic);
        }
        if (entry.dirty || !entry.matrix || values.some((value, index) => value !== entry.matrix[index])) {
            entry.graphic.setAttribute("transform", "matrix(" + values.join(" ") + ")");
            entry.matrix = values;
        }
        entry.dirty = false;
    }
}

function cloneMaskGraphic(node) {
    const copy = cloneDisplay(node);
    const originals = node.querySelectorAll("foreignObject");
    const surfaces = copy.querySelectorAll("foreignObject");
    for (let index = 0; index < originals.length; ++index) {
        const bitmap = bitmapSurfaces.get(originals[index])?.();
        if (!bitmap?.width || !bitmap.height) continue;
        const image = element("image", { href: bitmap.imageUrl() });
        for (const attr of ["x", "y", "width", "height", "transform"]) if (originals[index].hasAttribute(attr))
            image.setAttribute(attr, originals[index].getAttribute(attr));
        surfaces[index].replaceWith(image);
    }
    copy.removeAttribute("data-flash-composite");
    return copy;
}

function updateBlendMask(container) {
    const sources = [];
    for (let node = container.lastElementChild; node?.hasAttribute("data-flash-composite"); node = node.previousElementSibling)
        sources.unshift({ node, mode: node.getAttribute("data-flash-composite") });
    const previous = blendMasks.get(container);
    if (previous?.sources.length === sources.length &&
        sources.every(({ node, mode }, index) => previous.sources[index].node === node && previous.sources[index].mode === mode)) return;
    if (previous) {
        for (const observer of previous.observers) observer.disconnect();
        maskElements.unregister(previous.definitions);
        previous.definitions.remove();
        container.removeAttribute("mask");
        blendMasks.delete(container);
    }
    if (!sources.length || container.hasAttribute("mask")) return;
    // An alpha or erase layer contributes coverage, independently of its RGB channels.
    const extent = { x: -1000000, y: -1000000, width: 2000000, height: 2000000 };
    const definitions = element("g"), observers = [];
    const create = mode => {
        const mask = element("mask", { id: "blend-mask-" + (++masks),
            "mask-type": mode === "erase" ? "luminance" : "alpha", maskUnits: "userSpaceOnUse", ...extent });
        definitions.append(mask);
        return mask;
    };
    const coverage = element("rect", { ...extent, fill: "white" });
    let combined = coverage, single;
    for (const { node, mode } of sources) {
        const mask = single = create(mode);
        const draw = () => {
            const graphic = cloneMaskGraphic(node);
            if (mode === "erase") {
                const erase = element("g", { style: "filter:brightness(0)" });
                erase.append(graphic);
                mask.replaceChildren(element("rect", { ...extent, fill: "white" }), erase);
            } else mask.replaceChildren(graphic);
        };
        draw();
        const observer = new MutationObserver(draw);
        observer.observe(node, { attributes: true, childList: true, subtree: true });
        observers.push(observer);
        if (sources.length > 1) {
            const layer = element("g", { mask: "url(#" + mask.id + ")" });
            layer.append(combined);
            combined = layer;
        }
    }
    if (combined !== coverage) {
        single = create("alpha");
        single.append(combined);
    }
    blendMasks.set(container, { sources, definitions, observers });
    maskElements.register(container, definitions, definitions);
    symbols.append(definitions);
    container.setAttribute("mask", "url(#" + single.id + ")");
}

function property(layer, name) {
    if (Object.hasOwn(layer.overrides, name)) return layer.overrides[name];
    const [a, b, c, d, x, y] = layer.row[2];
    switch (name) {
    case "x": return x / 20;
    case "y": return y / 20;
    case "scaleX": return Math.hypot(a, b);
    case "scaleY": return Math.hypot(c, d);
    case "rotation": return Math.atan2(b, a) * 180 / Math.PI;
    case "alpha": return layer.row[3];
    case "visible": return layer.row[6];
    case "blendMode": return layer.row[10] === "plus-lighter" ? "add" : layer.row[10] === "isolate" ? "layer" :
        layer.row[10] === "hard-light" ? "hardlight" : layer.row[10] || "normal";
    case "text": return layer.field?.text ?? "";
    case "buttonMode": return false;
    }
}

let nextFocus;
function eventView(node) {
    const listeners = new Map();
    return {
        addEventListener(type, callback, capture = false) {
            const eventType = domEvent(type);
            const key = JSON.stringify([eventType, Boolean(capture)]);
            if (typeof callback !== "function") throw new TypeError("ActionScript: écouteur manquant");
            let byCallback = listeners.get(key);
            if (!byCallback) { byCallback = new Map(); listeners.set(key, byCallback); }
            if (byCallback.has(callback)) return;
            const transient = node === stage && (eventType === "mousemove" || eventType === "mouseup") ?
                displayNodes.get(convertedMethodReceiver(callback)) : null;
            const attached = transient?.isConnected ? transient : null;
            const handle = event => {
                if (attached && !attached.isConnected) {
                    node.removeEventListener(eventType, handle, Boolean(capture));
                    byCallback.delete(callback);
                    return;
                }
                if (eventType === "enterFrame" && event.target !== node) return;
                try {
                    const custom = eventObjects.get(event);
                    if (custom) {
                        const previous = activeEvents.get(custom);
                        activeEvents.set(custom, event);
                        try { callback(custom); }
                        finally { if (previous) activeEvents.set(custom, previous); else activeEvents.delete(custom); }
                        return;
                    }
                    const kind = eventKind(event);
                    const view = {
                        type, bubbles: event.bubbles, cancelable: event.cancelable,
                        currentTarget: displayViews.get(node), target: displayObjectFor(event.target) ?? displayViews.get(node),
                        text: event.text ?? (typeof event.detail === "string" ? event.detail : event.detail?.text ?? ""),
                        data: event.data ?? event.detail?.data,
                        level: event.level ?? event.detail?.level, code: event.code ?? event.detail?.code,
                        bytesLoaded: event.bytesLoaded ?? event.detail?.bytesLoaded ?? 0,
                        bytesTotal: event.bytesTotal ?? event.detail?.bytesTotal ?? 0,
                        status: event.status ?? event.detail?.status,
                        keyCode: event.keyCode ?? event.detail?.keyCode ?? 0, charCode: event.charCode ?? event.detail?.charCode ?? 0,
                        ctrlKey: Boolean(event.ctrlKey ?? event.detail?.ctrlKey), shiftKey: Boolean(event.shiftKey ?? event.detail?.shiftKey),
                        altKey: Boolean(event.altKey ?? event.detail?.altKey),
                        preventDefault: () => event.preventDefault(), stopPropagation: () => event.stopPropagation(),
                        stopImmediatePropagation: () => event.stopImmediatePropagation(),
                        isDefaultPrevented: () => event.defaultPrevented,
                    };
                    if (kind === "MouseEvent" || kind === "KeyboardEvent" || kind === "TimerEvent")
                        view.updateAfterEvent = updateAfterEvent;
                    if (eventType === "focusin" || eventType === "focusout")
                        view.relatedObject = event.relatedTarget === stage ? null : displayObjectFor(event.relatedTarget);
                    if (kind === "MouseEvent") {
                        view.relatedObject = displayObjectFor(event.relatedTarget);
                        view.buttonDown = Boolean(event.buttons & 1);
                        view.delta = event instanceof WheelEvent ? Math.sign(-event.deltaY) * 3 : 0;
                        let local, global;
                        const eventNode = displayNodes.get(view.target) ?? (node.getScreenCTM ? node : stage);
                        Object.defineProperties(view, {
                            localX: { get: () => (local ??= positionIn(eventNode, event.clientX, event.clientY)).x },
                            localY: { get: () => (local ??= positionIn(eventNode, event.clientX, event.clientY)).y },
                            stageX: { get: () => (global ??= positionIn(stage, event.clientX, event.clientY)).x },
                            stageY: { get: () => (global ??= positionIn(stage, event.clientX, event.clientY)).y },
                        });
                    }
                    eventKinds.set(view, kind);
                    if (eventType !== "focusout") callback(view);
                    else {
                        const previous = nextFocus;
                        // The DOM briefly reports body as active during a native focus transfer.
                        nextFocus = { view: !event.relatedTarget || event.relatedTarget === stage ?
                            displayObjectFor(pointerEditor?.node) ?? view.relatedObject : view.relatedObject };
                        try { callback(view); }
                        finally { nextFocus = previous; }
                    }
                } catch (error) { scriptProblem(error); }
            };
            byCallback.set(callback, handle);
            node.addEventListener(eventType, handle, Boolean(capture));
            if (eventType === "click" && node instanceof SVGElement) clickTargets.add(node);
            if (pointerEvents.has(eventType) && node instanceof SVGElement && !node.hasAttribute("role"))
                node.setAttribute("data-flash-pointer-listener", "");
            if (eventType === "enterFrame" && node instanceof SVGElement && !frameReferences.has(node)) {
                const reference = new WeakRef(node);
                frameReferences.set(node, reference);
                frameListeners.add(reference);
            }
        },
        removeEventListener(type, callback, capture = false) {
            const eventType = domEvent(type);
            const byCallback = listeners.get(JSON.stringify([eventType, Boolean(capture)]));
            const handle = byCallback?.get(callback);
            if (handle) {
                node.removeEventListener(eventType, handle, Boolean(capture));
                byCallback.delete(callback);
                if (eventType === "click" && !listeners.get('["click",false]')?.size && !listeners.get('["click",true]')?.size)
                    clickTargets.delete(node);
                if (pointerEvents.has(eventType) && node instanceof SVGElement &&
                    ![...pointerEvents].some(type => listeners.get(JSON.stringify([type, false]))?.size || listeners.get(JSON.stringify([type, true]))?.size))
                    node.removeAttribute("data-flash-pointer-listener");
                if (eventType === "enterFrame" && !listeners.get(JSON.stringify([eventType, false]))?.size &&
                    !listeners.get(JSON.stringify([eventType, true]))?.size) {
                    frameListeners.delete(frameReferences.get(node));
                    frameReferences.delete(node);
                }
            }
        },
        dispatchEvent(event) {
            const original = objectEvents.get(event);
            if (original) {
                if (original.eventPhase === Event.NONE) event = original;
                else {
                    const forwarded = scriptEvent(event.type, event.bubbles, event.cancelable, undefined, eventKinds.get(event));
                    eventObjects.set(forwarded, event);
                    event = forwarded;
                }
            } else if (!(event instanceof Event) && eventKinds.has(event))
                event = scriptEvent(event.type, event.bubbles, event.cancelable, event, eventKinds.get(event));
            return node.dispatchEvent(event);
        },
        hasEventListener(type) {
            const name = domEvent(type);
            return Boolean(listeners.get(JSON.stringify([name, false]))?.size || listeners.get(JSON.stringify([name, true]))?.size);
        },
    };
}

)JS" R"JS(function reconcileChildren(container, nodes) {
    const retained = new Set(nodes);
    for (let child = container.firstChild; child;) {
        const next = child.nextSibling;
        if (!retained.has(child)) child.remove();
        child = next;
    }
    let next = container.firstChild;
    for (const node of nodes) {
        if (node === next) next = next.nextSibling;
        else if (node.parentNode === container && container.moveBefore) container.moveBefore(node, next);
        else container.insertBefore(node, next);
    }
    if (blendMasks.has(container) || container.lastElementChild?.hasAttribute("data-flash-composite")) updateBlendMask(container);
}

function displayChildren(container, owner, repaint) {
    const order = [], suppressed = new Set();
    let timeline = new Set();
    const render = repaint ?? (() => reconcileChildren(container, order.map(view => displayNodes.get(view))));
    const checkedIndex = (index, end) => {
        index = Number(index);
        if (!Number.isInteger(index) || index < 0 || index >= end) throw new RangeError("Index d’enfant hors limites.");
        return index;
    };
    const list = {
        order,
        addChild(view) { return list.addChildAt(view, order.length); },
        addChildAt(view, index) {
            const node = displayNodes.get(view);
            if (!node || node.contains(container)) throw new TypeError("Objet d’affichage incompatible.");
            index = checkedIndex(index, order.length + 1);
            const previous = displayOwners.get(node);
            if (previous === list) order.splice(order.indexOf(view), 1);
            else if (previous) previous.removeChild(view);
            order.splice(index, 0, view);
            suppressed.delete(view);
            displayParents.set(node, owner);
            displayOwners.set(node, list);
            render();
            if (previous !== list) {
                authoredClips.get(node)?.initializeTree();
                node.dispatchEvent(scriptEvent("added", true));
                if (container.isConnected) {
                    if (authoredClips.has(node)) scriptedNodes.add(node);
                    node.dispatchEvent(scriptEvent("addedToStage"));
                }
            }
            return view;
        },
        removeChild(view) {
            const node = displayNodes.get(view);
            const index = list.getChildIndex(view);
            node.dispatchEvent(scriptEvent("removed", true));
            if (container.isConnected) node.dispatchEvent(scriptEvent("removedFromStage"));
            order.splice(index, 1);
            node.remove();
            scriptedNodes.delete(node);
            if (timeline.has(view)) suppressed.add(view);
            displayParents.delete(node);
            displayOwners.delete(node);
            render();
            return view;
        },
        removeChildAt(index) { return list.removeChild(list.getChildAt(index)); },
        getChildAt(index) { return order[checkedIndex(index, order.length)]; },
        getChildIndex(view) {
            const index = order.indexOf(view);
            if (index < 0) throw new RangeError("L’objet n’est pas un enfant de ce conteneur.");
            return index;
        },
        setChildIndex(view, index) {
            index = checkedIndex(index, order.length);
            order.splice(list.getChildIndex(view), 1);
            order.splice(index, 0, view);
            render();
        },
        swapChildrenAt(first, second) {
            checkedIndex(first, order.length);
            checkedIndex(second, order.length);
            [order[first], order[second]] = [order[second], order[first]];
            render();
        },
        swapChildren(first, second) { list.swapChildrenAt(list.getChildIndex(first), list.getChildIndex(second)); },
        contains(view) {
            const node = displayNodes.get(view);
            return Boolean(node && (order.includes(view) || container.contains(node)));
        },
        getChildByName(name) { return order.find(view => view.name === name) ?? null; },
        synchronize(current) {
            const present = new Set(current);
            for (let index = order.length - 1; index >= 0; --index) {
                const view = order[index];
                if (!timeline.has(view) || present.has(view)) continue;
                const node = displayNodes.get(view);
                if (displayOwners.get(node) === list) {
                    displayOwners.delete(node);
                    displayParents.delete(node);
                }
                order.splice(index, 1);
            }
            for (const view of suppressed) if (!present.has(view)) suppressed.delete(view);
            const positions = new Set(order);
            const following = new Array(current.length);
            let next;
            for (let index = current.length - 1; index >= 0; --index) {
                following[index] = next;
                if (positions.has(current[index])) next = current[index];
            }
            let previous;
            for (let index = 0; index < current.length; ++index) {
                const view = current[index];
                if (suppressed.has(view)) continue;
                if (!positions.has(view)) {
                    const node = displayNodes.get(view);
                    const next = following[index];
                    order.splice(next ? order.indexOf(next) : previous ? order.indexOf(previous) + 1 : 0, 0, view);
                    displayParents.set(node, owner);
                    displayOwners.set(node, list);
                }
                previous = view;
            }
            timeline = present;
        },
    };
    return list;
}

function graphics(container) {
    let fill = "none", fillAlpha = 1, stroke = "none", strokeAlpha = 1, strokeWidth = 0;
    let x = 0, y = 0, path = null, commands = "";
    const color = value => "#" + (Number(value) & 0xffffff).toString(16).padStart(6, "0");
    const attributes = () => ({ fill, "fill-opacity": fillAlpha, stroke, "stroke-opacity": strokeAlpha,
        "stroke-width": strokeWidth * 20 });
    const add = (kind, values) => container.append(element(kind, { ...attributes(), ...values }));
    const point = (px, py) => [Number(px) * 20, Number(py) * 20].join(" ");
    const draw = command => {
        if (!path) {
            commands = "M " + point(x, y);
            path = element("path", attributes());
            container.append(path);
        }
        commands += " " + command;
        path.setAttribute("d", commands);
    };
    return {
        clear() { container.replaceChildren(); path = null; x = y = 0; fill = stroke = "none"; },
        lineStyle(width, value = 0, alpha = 1) {
            path = null; strokeWidth = width ?? 0;
            stroke = width == null ? "none" : color(value); strokeAlpha = alpha;
        },
        beginFill(value, alpha = 1) { path = null; fill = color(value); fillAlpha = alpha; },
        beginBitmapFill(bitmap, matrix = null, repeat = true, smooth = false) {
            if (!bitmap?.width || !bitmap.height || !bitmap.canvas) throw new TypeError("BitmapData du remplissage invalide.");
            const id = "bitmap-pattern-" + (++masks), w = bitmap.width * 20, h = bitmap.height * 20;
            const pattern = element("pattern", { id, patternUnits: "userSpaceOnUse", width: repeat ? w : 2000000, height: repeat ? h : 2000000 });
            if (matrix) pattern.setAttribute("patternTransform", `matrix(${matrix.a} ${matrix.b} ${matrix.c} ${matrix.d} ${matrix.tx * 20} ${matrix.ty * 20})`);
            const image = element("image", { href: bitmap.imageUrl(), width: w, height: h });
            if (!smooth) image.style.imageRendering = "pixelated";
            pattern.append(image);
            container.append(pattern);
            bitmapPatterns.set(pattern, { bitmap, matrix, repeat });
            path = null; fill = "url(#" + id + ")"; fillAlpha = 1;
        },
        endFill() { path = null; fill = "none"; },
        moveTo(nextX, nextY) { if (path) draw("M " + point(nextX, nextY)); x = nextX; y = nextY; },
        lineTo(nextX, nextY) { draw("L " + point(nextX, nextY)); x = nextX; y = nextY; },
        curveTo(cx, cy, nextX, nextY) { draw("Q " + point(cx, cy) + " " + point(nextX, nextY)); x = nextX; y = nextY; },
        drawRect(left, top, width, height) { add("rect", { x: left * 20, y: top * 20, width: width * 20, height: height * 20 }); },
        drawRoundRect(left, top, width, height, ellipseWidth, ellipseHeight = ellipseWidth) {
            if (![left, top, width, height, ellipseWidth, ellipseHeight].every(value => Number.isFinite(Number(value)))) return;
            const x = Number(left), y = Number(top), w = Number(width), h = Number(height);
            add("rect", { x: (x + Math.min(0, w)) * 20, y: (y + Math.min(0, h)) * 20,
                width: Math.abs(w) * 20, height: Math.abs(h) * 20,
                rx: Math.min(Math.abs(w), Math.abs(ellipseWidth)) * 10, ry: Math.min(Math.abs(h), Math.abs(ellipseHeight)) * 10 });
        },
        drawCircle(cx, cy, radius) { add("circle", { cx: cx * 20, cy: cy * 20, r: radius * 20 }); },
        drawEllipse(left, top, width, height) {
            add("ellipse", { cx: left * 20 + width * 10, cy: top * 20 + height * 10, rx: width * 10, ry: height * 10 });
        },
    };
}

function glowFilter(color = 0xff0000, alpha = 1, blurX = 6, blurY = 6, strength = 2, quality = 1, inner = false, knockout = false) {
    return { color, alpha, blurX, blurY, strength, quality, inner, knockout };
}

function dropShadowFilter(distance = 4, angle = 45, color = 0, alpha = 1, blurX = 4, blurY = 4,
    strength = 1, quality = 1, inner = false, knockout = false, hideObject = false) {
    return { distance, angle, color, alpha, blurX, blurY, strength, quality, inner, knockout, hideObject };
}

function bevelFilter(distance = 4, angle = 45, highlightColor = 0xffffff, highlightAlpha = 1, shadowColor = 0,
    shadowAlpha = 1, blurX = 4, blurY = 4, strength = 1, quality = 1, type = "inner", knockout = false) {
    return { distance, angle, highlightColor, highlightAlpha, shadowColor, shadowAlpha, blurX, blurY, strength, quality, type, knockout };
}

function displacementFilter(mapBitmap = null, mapPoint = null, componentX = 0, componentY = 0, scaleX = 0, scaleY = 0,
    mode = "wrap", color = 0, alpha = 0) {
    return { mapBitmap, mapPoint, componentX, componentY, scaleX, scaleY, mode, color, alpha };
}

function filterEffect(effect) {
    const type = displayTypes.get(effect);
    const x = Math.max(0, Number(effect?.blurX) || 0), y = Math.max(0, Number(effect?.blurY) || 0);
    const quality = Math.max(1, Math.min(15, effect?.quality | 0));
    if (type === "BlurFilter") return [1, x, y, quality];
    if (type !== "GlowFilter" && type !== "DropShadowFilter" && type !== "BevelFilter") return null;
    const radius = (Number(effect.angle) || 0) * Math.PI / 180;
    const distance = Number(effect.distance) || 0;
    const dx = distance * Math.cos(radius), dy = distance * Math.sin(radius);
    const hex = color => "#" + (color & 0xffffff).toString(16).padStart(6, "0");
    if (type === "BevelFilter") return [3, x, y, dx, dy, hex(effect.highlightColor), hex(effect.shadowColor),
        Math.max(0, Number(effect.highlightAlpha * effect.strength) || 0), Math.max(0, Number(effect.shadowAlpha * effect.strength) || 0),
        quality, effect.type, effect.knockout];
    const flags = (effect.hideObject ? 0 : 32) | (effect.inner ? 128 : 0) | (effect.knockout ? 64 : 0);
    return [2, x, y, dx, dy, hex(effect.color),
        Math.max(0, Number(effect.alpha * effect.strength) || 0), flags, quality];
}

)JS" R"JS(let colorFilterSequence = 0;
const colorFilters = new FinalizationRegistry(filter => filter.remove());

function displayTransform(node, effects = () => [], staticChild = false) {
    let color, reusable, applied;
    const put = url => {
        if (applied === url) return;
        applied = url;
        if (url) node.style.filter = url;
        else node.style.removeProperty("filter");
    };
    const clear = () => {
        if (!reusable) return;
        reusable.target?.style.removeProperty("filter");
        colorFilters.unregister(reusable.filter);
        reusable.filter.remove();
        reusable = null;
    };
    const update = () => {
        const rendered = effects().map(filterEffect).filter(Boolean);
        let transform = color ? [color.redMultiplier * 256, color.greenMultiplier * 256, color.blueMultiplier * 256,
            color.alphaMultiplier * 256, color.redOffset, color.greenOffset, color.blueOffset, color.alphaOffset] : null;
        if (transform?.every((value, index) => value === (index < 4 ? 256 : 0))) transform = null;
        if (transform) bitmapTints.set(node, transform);
        else bitmapTints.delete(node);
        if (transform && !rendered.length) {
            // A tint on a one-layer symbol can follow its authored effect in the same filter.
            const child = staticChild && node.childElementCount === 1 && transform[3] === 256 && transform[7] === 0 ?
                node.firstElementChild : null;
            const sourceId = child?.getAttribute("filter")?.match(/^url\(#(effect-\d+)\)$/)?.[1];
            const target = sourceId ? child : null;
            if (reusable && (reusable.target !== target || reusable.sourceId !== sourceId)) clear();
            if (!reusable) {
                const id = "color-" + (++colorFilterSequence);
                // Include descendant glows and shadows in the tinted surface.
                const outsets = node.querySelector("[filter]") ? "-100%" : "0%";
                const extent = outsets === "0%" ? "100%" : "300%";
                const filter = (sourceId && document.getElementById(sourceId)?.cloneNode(true)) ||
                    element("filter", { "color-interpolation-filters": "sRGB", x: outsets, y: outsets, width: extent, height: extent });
                filter.id = id;
                const transfer = element("feComponentTransfer", { result: "colored" });
                const channels = [..."RGBA"].map(name => element("feFunc" + name, { type: "linear" }));
                transfer.append(...channels);
                filter.append(transfer);
                symbols.append(filter);
                colorFilters.register(node, filter, filter);
                reusable = { filter, transfer, channels, url: "url(#" + id + ")", values: [], target, sourceId };
                if (target) target.style.filter = reusable.url;
            }
            reusable.channels.forEach((channel, index) => {
                if (transform[index] !== reusable.values[index]) channel.setAttribute("slope", transform[index] / 256);
                if (transform[index + 4] !== reusable.values[index + 4]) channel.setAttribute("intercept", transform[index + 4] / 255);
            });
            if ((transform[7] > 0) !== (reusable.values[7] > 0)) {
                if (transform[7] > 0) preserveCoverage(reusable.filter, "colored");
                else reusable.filter.replaceChildren(reusable.transfer);
            }
            reusable.values = transform;
            put(target ? "" : reusable.url);
        } else {
            clear();
            put(rendered.length ? svgFilter(transform, rendered) : "");
        }
    };
    return {
        get colorTransform() { return color ?? new ColorTransform(); },
        set colorTransform(value) { color = value; update(); },
        update,
    };
}

function scriptSprite(kind, offsetX = () => 0, effectChanged) {
    const node = element("g"), drawing = element("g"), children = element("g");
    let visibleView;
    node.append(drawing, children);
    const childList = displayChildren(children, () => visibleView);
    const access = pointerAccess(node, ["Sprite", "MovieClip", "SimpleButton", "TextField", "Loader"].includes(kind));
    const view = Object.assign(Object.create(null), eventView(node), childList, {
        graphics: graphics(drawing), name: "",
        getBounds: target => displayBounds(node, target), getRect: target => displayBounds(node, target),
        localToGlobal: point => transformPoint(point, node, stage),
        globalToLocal: point => transformPoint(point, stage, node),
    });
    const transform = { x: 0, y: 0, scaleX: 1, scaleY: 1, rotation: 0, alpha: 1, visible: true };
    let effects = [], blend = "normal";
    const appearance = displayTransform(node, () => effects);
    view.transform = appearance;
    for (const key of Object.keys(transform))
        Object.defineProperty(view, key, {
            get: () => key === "x" ? transform.x + offsetX() * transform.scaleX : transform[key],
            set: value => {
                const next = key === "visible" ? Boolean(value) :
                    key === "x" ? Number(value) - offsetX() * transform.scaleX : Number(value);
                if (transform[key] === next || (typeof next === "number" && !Number.isFinite(next * 20))) return;
                transform[key] = next;
                if (key === "alpha") node.setAttribute("opacity", next);
                else if (key === "visible") node.setAttribute("display", next ? "inline" : "none");
                else {
                    const { x, y, scaleX, scaleY, rotation } = transform;
                    node.setAttribute("transform", "translate(" + x * 20 + " " + y * 20 + ") rotate(" + rotation +
                        ") scale(" + scaleX + " " + scaleY + ")");
                }
            },
        });
    Object.defineProperties(view, {
        numChildren: { get: () => childList.order.length },
        width: { configurable: true, get: () => displayDimension(node, "width"),
            set: value => resizeDisplay(node, view, "width", value) },
        height: { configurable: true, get: () => displayDimension(node, "height"),
            set: value => resizeDisplay(node, view, "height", value) },
        parent: { get: () => displayParents.get(node)?.() ?? null },
        stage: { get: () => node.isConnected ? stageView : null },
        root: { get: () => node.isConnected ? root?.view ?? null : null },
        mask: { get: () => scriptMasks.get(node)?.view ?? null, set: value => changeMask(node, value) },
        mouseX: { get: () => positionIn(node).x },
        mouseY: { get: () => positionIn(node).y },
        buttonMode: { get: () => node.style.cursor === "pointer", set: value => { node.style.cursor = value ? "pointer" : ""; } },
        mouseEnabled: {
            get: () => access.mouseEnabled, set: value => { access.mouseEnabled = value; },
        },
        mouseChildren: {
            get: () => access.mouseChildren, set: value => { access.mouseChildren = value; },
        },
        filters: {
            get: () => effects.slice(),
            set: value => {
                effects = Array.isArray(value) ? value.slice() : [];
                appearance.update();
                effectChanged?.();
            },
        },
        blendMode: { get: () => blend, set: value => {
            blend = String(value);
            displayBlend(node, blendModes[blend] ?? blend);
        } },
    });
    if (kind === "MovieClip") Object.assign(view, {
        currentFrame: 1, totalFrames: 1, currentLabels: [], currentLabel: null, currentFrameLabel: null, stop() {}, play() {},
    });
    view.bindView = instance => {
        visibleView = instance;
        displayNodes.set(instance, node);
        displayViews.set(node, instance);
        displayTypes.set(instance, kind);
    };
    view.bindView(view);
    return view;
}

function scriptTimer(initialDelay, initialRepeats = 0) {
    const node = new EventTarget();
    let timeout = null, currentCount = 0, running = false;
    let delay, repeatCount;
    const stop = () => { clearTimeout(timeout); timeout = null; running = false; };
    const schedule = () => {
        timeout = setTimeout(() => {
            timeout = null;
            ++currentCount;
            node.dispatchEvent(scriptEvent("timer"));
            if (running && repeatCount && currentCount >= repeatCount) {
                stop();
                node.dispatchEvent(scriptEvent("timerComplete"));
            } else if (running && timeout === null) schedule();
        }, delay);
    };
    const timer = Object.assign(eventView(node), {
        start() {
            if (running || repeatCount && currentCount >= repeatCount) return;
            running = true;
            schedule();
        },
        stop,
        bindView(instance) { displayViews.set(node, instance); },
        reset() { stop(); currentCount = 0; },
    });
    Object.defineProperties(timer, {
        delay: {
            get: () => delay,
            set: value => {
                if (!Number.isFinite(value) || value < 0) throw new RangeError("Intervalle Timer invalide.");
                delay = value;
                if (running) { stop(); timer.start(); }
            },
        },
        repeatCount: {
            get: () => repeatCount,
            set: value => { repeatCount = Math.max(0, value | 0); if (repeatCount && currentCount >= repeatCount) stop(); },
        },
        currentCount: { get: () => currentCount },
        running: { get: () => running },
    });
    timer.delay = initialDelay;
    timer.repeatCount = initialRepeats;
    displayViews.set(node, timer);
    displayTypes.set(timer, "Timer");
    return timer;
}

)JS" R"JS(function rgba(value, opaque = false) {
    return "rgba(" + [value >>> 16 & 255, value >>> 8 & 255, value & 255, opaque ? 1 : (value >>> 24) / 255].join(",") + ")";
}

const tiledBitmaps = new WeakMap();

class BitmapData {
    constructor(width, height, transparent = true, color = 0xffffffff) {
        this.canvas = document.createElement("canvas");
        this.listeners = new Set();
        this.locked = false;
        this.revision = 0;
        this.initialize(width, height, transparent, color);
    }
    initialize(width, height, transparent = true, color = 0xffffffff, image) {
        width = Math.trunc(Number(width));
        height = Math.trunc(Number(height));
        transparent = Boolean(transparent);
        if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 ||
            width > 8192 || height > 8192 || width * height > 0x1000000)
            throw new RangeError("Dimensions BitmapData invalides.");
        if (this.context && transparent !== this.transparent) this.canvas = document.createElement("canvas");
        this.canvas.width = width;
        this.canvas.height = height;
        this.context = this.canvas.getContext("2d", { alpha: transparent, willReadFrequently: true });
        this.transparent = transparent;
        if (image) {
            this.context.drawImage(image, 0, 0);
            this.update();
            this.linkedUrl = image.src;
        } else {
            this.fillRect(this.rect, color);
            this.pristine = this.transparent && color >>> 24 === 0;
        }
    }
    get width() { return this.canvas.width; }
    get height() { return this.canvas.height; }
    get rect() { return new Rectangle(0, 0, this.width, this.height); }
    imageUrl() { return this.linkedUrl || (this.maskImage ??= this.canvas.toDataURL()); }
    lock() { this.locked = true; }
    unlock() { this.locked = false; this.update(); }
    update() {
        ++this.revision;
        this.pristine = false;
        this.sample = null;
        this.maskImage = null;
        this.linkedUrl = "";
        if (!this.locked) for (const observer of this.listeners) {
            const redraw = observer.deref();
            if (redraw) redraw();
            else this.listeners.delete(observer);
        }
    }
    getPixel32(x, y) {
        x = Math.trunc(x); y = Math.trunc(y);
        if (x < 0 || y < 0 || x >= this.width || y >= this.height) return 0;
        this.sample ??= this.context.getImageData(0, 0, this.width, this.height).data;
        const pixel = (y * this.width + x) * 4;
        const data = this.sample;
        return ((data[pixel + 3] << 24) | (data[pixel] << 16) | (data[pixel + 1] << 8) | data[pixel + 2]) >>> 0;
    }
    getPixel(x, y) { return this.getPixel32(x, y) & 0xffffff; }
    setPixel(x, y, rgb) {
        x = Math.trunc(x); y = Math.trunc(y);
        if (x < 0 || y < 0 || x >= this.width || y >= this.height) return;
        const alpha = !this.transparent ? 255 : this.sample ?
            this.sample[(y * this.width + x) * 4 + 3] : this.context.getImageData(x, y, 1, 1).data[3];
        this.setPixel32(x, y, (alpha << 24) | (rgb & 0xffffff));
    }
    setPixel32(x, y, color) {
        x = Math.trunc(x); y = Math.trunc(y);
        if (x < 0 || y < 0 || x >= this.width || y >= this.height) return;
        this.pixel ??= this.context.createImageData(1, 1);
        const bytes = this.pixel.data;
        bytes[0] = color >>> 16 & 255;
        bytes[1] = color >>> 8 & 255;
        bytes[2] = color & 255;
        bytes[3] = this.transparent ? color >>> 24 : 255;
        this.context.putImageData(this.pixel, x, y);
        this.update();
    }
    clone() {
        const copy = new BitmapData(this.width, this.height, this.transparent, 0);
        copy.context.drawImage(this.canvas, 0, 0);
        return copy;
    }
    dispose() {
        this.canvas.width = 0;
        this.canvas.height = 0;
        this.update();
        this.listeners.clear();
    }
    fillRect(rect, color) {
        const {x, y, width, height} = rect;
        this.context.clearRect(x, y, width, height);
        this.context.fillStyle = rgba(color, !this.transparent);
        this.context.fillRect(x, y, width, height);
        this.update();
    }
    noise(seed, low = 0, high = 255, channels = 7, grayscale = false) {
        const image = this.context.createImageData(this.width, this.height);
        let random = (seed >>> 0) || 1;
        const next = () => {
            random ^= random << 13;
            random ^= random >>> 17;
            random ^= random << 5;
            return low + (random >>> 0) % Math.max(1, high - low + 1);
        };
        for (let index = 0; index < image.data.length; index += 4) {
            const shade = grayscale ? next() : 0;
            for (let channel = 0; channel < 3; ++channel)
                image.data[index + channel] = grayscale ? shade : channels & (1 << channel) ? next() : 0;
            image.data[index + 3] = this.transparent && (channels & 8) ? next() : 255;
        }
        this.context.putImageData(image, 0, 0);
        this.update();
    }
    perlinNoise(baseX, baseY, octaves, seed, stitch = false, fractal = false, channels = 7, grayscale = false, offsets = []) {
        const width = this.width, height = this.height;
        const image = this.context.createImageData(width, height);
        const passes = Math.max(1, Math.min(10, octaves | 0));
        const mix = (a, b, ratio) => a + (b - a) * ratio;
        const fade = n => n * n * n * (n * (n * 6 - 15) + 10);
        const axis = (size, base, octave, offset) => {
            const scale = (2 ** octave) / Math.max(1, Math.abs(base));
            const period = stitch ? Math.max(1, Math.round(size * scale)) : 0;
            const factor = period ? period / size : scale;
            const origin = Number(offset) || 0;
            const wrap = value => period ? ((value % period) + period) % period : value;
            return Array.from({ length: size }, (_, index) => {
                const position = (index + origin) * factor;
                const left = Math.floor(position), fraction = position - left;
                return { first: wrap(left), second: wrap(left + 1), fraction, weight: fade(fraction) };
            });
        };
        const grid = Array.from({ length: passes }, (_, octave) => ({
            x: axis(width, baseX, octave, offsets?.[octave]?.x),
            y: axis(height, baseY, octave, offsets?.[octave]?.y),
            amplitude: 2 ** -octave,
        }));
        const total = grid.reduce((sum, item) => sum + item.amplitude, 0);
        const gradient = (x, y, dx, dy, salt) => {
            let hash = Math.imul(x, 0x1f123bb5) ^ Math.imul(y, 0x5f356495) ^ salt;
            hash = Math.imul(hash ^ (hash >>> 16), 0x45d9f3b);
            return (hash & 1 ? -dx : dx) + (hash & 2 ? -dy : dy);
        };
        const noiseAt = (x, y, channel) => {
            let value = 0;
            const salt = (seed | 0) + channel * 0x1f123bb5;
            for (const cell of grid) {
                const column = cell.x[x], row = cell.y[y];
                const { first: x0, second: x1, fraction: dx, weight: horizontal } = column;
                const { first: y0, second: y1, fraction: dy, weight: vertical } = row;
                const a = mix(gradient(x0, y0, dx, dy, salt), gradient(x1, y0, dx - 1, dy, salt), horizontal);
                const b = mix(gradient(x0, y1, dx, dy - 1, salt), gradient(x1, y1, dx - 1, dy - 1, salt), horizontal);
                const sample = mix(a, b, vertical);
                value += (fractal ? sample : Math.abs(sample)) * cell.amplitude;
            }
            return fractal ? 128 + (value / total) * 127 : (value / total) * 255;
        };
        for (let y = 0; y < height; ++y) for (let x = 0; x < width; ++x) {
            const index = (y * width + x) * 4;
            const shade = grayscale ? noiseAt(x, y, 0) : 0;
            for (let channel = 0; channel < 3; ++channel)
                image.data[index + channel] = grayscale ? shade : channels & (1 << channel) ? noiseAt(x, y, channel) : 0;
            image.data[index + 3] = this.transparent && (channels & 8) ? noiseAt(x, y, 3) : 255;
        }
        this.context.putImageData(image, 0, 0);
        this.update();
    }
    colorTransform(rect, transform) {
        if (rect.width <= 0 || rect.height <= 0) return;
        if (transform.redMultiplier === 1 && transform.greenMultiplier === 1 && transform.blueMultiplier === 1 &&
            !transform.redOffset && !transform.greenOffset && !transform.blueOffset && !transform.alphaOffset &&
            transform.alphaMultiplier >= 0 && transform.alphaMultiplier <= 1) {
            if (!this.transparent || transform.alphaMultiplier === 1) return;
            const context = this.context;
            context.save();
            const area = new Path2D();
            area.rect(rect.x, rect.y, rect.width, rect.height);
            context.clip(area);
            context.globalCompositeOperation = "destination-in";
            context.globalAlpha = transform.alphaMultiplier;
            context.fillStyle = "white";
            context.fillRect(rect.x, rect.y, rect.width, rect.height);
            context.restore();
            this.update();
            return;
        }
        const image = this.context.getImageData(rect.x, rect.y, rect.width, rect.height);
        const channels = ["red", "green", "blue", "alpha"];
        const multipliers = channels.map(channel => transform[channel + "Multiplier"]);
        const offsets = channels.map(channel => transform[channel + "Offset"]);
        for (let index = 0; index < image.data.length; ++index) {
            const channel = index & 3;
            image.data[index] = image.data[index] * multipliers[channel] + offsets[channel];
        }
        this.context.putImageData(image, rect.x, rect.y);
        this.update();
    }
)JS" R"JS(    applyFilter(source, rect, target, filter) {
        if (!source?.width || !source.height || rect.width < 1 || rect.height < 1) return;
        if (displayTypes.get(filter) === "DisplacementMapFilter" && filter.mapBitmap?.width && filter.mapBitmap.height) {
            this.context.putImageData(displacePixels(this.context, source, rect, filter), target.x, target.y);
            this.update();
            return;
        }
        const pixels = document.createElement("canvas");
        pixels.width = Math.ceil(rect.width);
        pixels.height = Math.ceil(rect.height);
        const context = pixels.getContext("2d");
        const effect = filterEffect(filter);
        if (effect) context.filter = svgFilter(null, [effect], 1);
        context.drawImage(source.canvas, rect.x, rect.y, rect.width, rect.height, 0, 0, rect.width, rect.height);
        this.context.clearRect(target.x, target.y, rect.width, rect.height);
        this.context.drawImage(pixels, target.x, target.y);
        this.update();
    }
    copyPixels(source, rect, target, alpha = null, alphaPoint = null, mergeAlpha = false) {
        if (!source?.canvas || rect.width <= 0 || rect.height <= 0) return;
        if (!alpha && mergeAlpha && rect.x === 0 && rect.y === 0 && rect.width === source.width && rect.height === source.height) {
            this.context.drawImage(source.canvas, target.x, target.y);
            this.update();
            return;
        }
        let pixels = source.canvas;
        let x = rect.x, y = rect.y;
        if (alpha?.canvas) {
            pixels = document.createElement("canvas");
            pixels.width = Math.ceil(rect.width);
            pixels.height = Math.ceil(rect.height);
            const context = pixels.getContext("2d");
            context.drawImage(source.canvas, x, y, rect.width, rect.height, 0, 0, rect.width, rect.height);
            context.globalCompositeOperation = "destination-in";
            context.drawImage(alpha.canvas, -(alphaPoint?.x ?? 0), -(alphaPoint?.y ?? 0));
            x = y = 0;
        }
        if (!mergeAlpha) this.context.clearRect(target.x, target.y, rect.width, rect.height);
        this.context.drawImage(pixels, x, y, rect.width, rect.height, target.x, target.y, rect.width, rect.height);
        this.update();
    }
    tilePixels(source, x, y) {
        if (!source?.width || !source.height || !this.width || !this.height || source === this) return false;
        let cached = tiledBitmaps.get(source);
        if (cached?.revision !== source.revision) {
            const pattern = this.context.createPattern(source.canvas, "repeat");
            if (!pattern) return false;
            cached = { revision: source.revision, pattern };
            tiledBitmaps.set(source, cached);
        }
        this.context.save();
        this.context.translate(x, y);
        this.context.fillStyle = cached.pattern;
        this.context.fillRect(-x, -y, this.width, this.height);
        this.context.restore();
        this.update();
        return true;
    }
    copyChannel(source, rect, target, sourceChannel, targetChannel) {
        const channels = [1, 2, 4, 8];
        const readChannel = channels.indexOf(sourceChannel), writeChannel = channels.indexOf(targetChannel);
        if (readChannel < 0 || writeChannel < 0) return;
        const sx = Math.trunc(rect.x), sy = Math.trunc(rect.y), tx = Math.trunc(target.x), ty = Math.trunc(target.y);
        const x = Math.max(0, -sx, -tx), y = Math.max(0, -sy, -ty);
        const width = Math.min(Math.trunc(rect.width), source.width - sx, this.width - tx) - x;
        const height = Math.min(Math.trunc(rect.height), source.height - sy, this.height - ty) - y;
        if (width <= 0 || height <= 0) return;
        const from = source.context.getImageData(sx + x, sy + y, width, height).data;
        const to = this.context.getImageData(tx + x, ty + y, width, height);
        for (let index = 0; index < from.length; index += 4) to.data[index + writeChannel] = from[index + readChannel];
        this.context.putImageData(to, tx + x, ty + y);
        this.update();
    }
    draw(source, matrix = null, color = null, blend = null, clip = null, smoothing = false) {
        const pixels = source?.canvas ?? source?.bitmapData?.canvas;
        const node = displayNodes.get(source);
        if (!pixels && !node) throw new TypeError("Surface BitmapData ou DisplayObject attendue.");
        const resources = !pixels && this.pristine && !color && !blend ? {} : null;
        this.paint(pixels, node, matrix, blend, clip, smoothing, resources);
        if (color) this.colorTransform(clip ?? this.rect, color);
        else this.update();
        if (resources?.waiting?.size) {
            const snapshot = cloneBitmapDisplay(node), target = new WeakRef(this), revision = this.revision;
            const transform = matrix && { ...matrix }, bounds = clip && { ...clip };
            Promise.allSettled([...resources.waiting].map(image => image.decode())).then(() => {
                const bitmap = target.deref();
                if (!bitmap || bitmap.revision !== revision) return;
                bitmap.context.clearRect(0, 0, bitmap.width, bitmap.height);
                bitmap.paint(null, snapshot, transform, null, bounds, smoothing);
                bitmap.update();
            }).catch(scriptProblem);
        }
    }
    paint(pixels, node, matrix, blend, clip, smoothing, resources) {
        const context = this.context;
        if (pixels && !clip && (!blend || blend === "normal") &&
            (!matrix || (!matrix.b && !matrix.c && matrix.a > 0 && matrix.d > 0))) {
            const previous = context.imageSmoothingEnabled;
            context.imageSmoothingEnabled = Boolean(smoothing);
            if (matrix) context.drawImage(pixels, matrix.tx, matrix.ty, pixels.width * matrix.a, pixels.height * matrix.d);
            else context.drawImage(pixels, 0, 0);
            context.imageSmoothingEnabled = previous;
            return;
        }
        context.save();
        try {
            if (clip) {
                context.beginPath();
                context.rect(clip.x, clip.y, clip.width, clip.height);
                context.clip();
            }
            if (matrix) context.transform(matrix.a, matrix.b, matrix.c, matrix.d, matrix.tx, matrix.ty);
            if (blend && blend !== "normal") context.globalCompositeOperation =
                ({ add: "lighter", alpha: "destination-in", erase: "destination-out", layer: "source-over" })[blend] ?? blend;
            context.imageSmoothingEnabled = Boolean(smoothing);
            if (pixels) context.drawImage(pixels, 0, 0);
            else {
                context.scale(1 / 20, 1 / 20);
                paintDisplay(node, context, true, resources);
            }
        } finally { context.restore(); }
    }
}

)JS" R"JS(const displacementBuffers = new WeakMap();

function displacePixels(context, source, rect, effect) {
    const width = Math.trunc(rect.width), height = Math.trunc(rect.height);
    const { mapBitmap, mapPoint, mode, color, alpha } = effect;
    const sourceData = source.sample ??= source.context.getImageData(0, 0, source.width, source.height).data;
    const mapData = mapBitmap.sample ??= mapBitmap.context.getImageData(0, 0, mapBitmap.width, mapBitmap.height).data;
    const [sourceWidth, sourceHeight, mapWidth, mapHeight] = [source.width, source.height, mapBitmap.width, mapBitmap.height];
    const left = Math.trunc(rect.x), top = Math.trunc(rect.y);
    const mapLeft = Math.trunc(mapPoint?.x ?? 0), mapTop = Math.trunc(mapPoint?.y ?? 0);
    const lanes = { 1: 0, 2: 1, 4: 2, 8: 3 }, cx = lanes[effect.componentX], cy = lanes[effect.componentY];
    const dx = (Number(effect.scaleX) || 0) / 256, dy = (Number(effect.scaleY) || 0) / 256;
    const edgeColor = [color >>> 16 & 255, color >>> 8 & 255, color & 255, Math.round(255 * alpha)];
    let image = displacementBuffers.get(context);
    if (image?.width !== width || image.height !== height) {
        image = context.createImageData(width, height);
        displacementBuffers.set(context, image);
    }
    const output = image.data;
    for (let row = 0, offset = 0; row < height; ++row) {
        const y = top + row, my = y - mapTop;
        for (let col = 0; col < width; ++col, offset += 4) {
            const x = left + col, mx = x - mapLeft;
            let sx = x, sy = y;
            if (mx >= 0 && mx < mapWidth && my >= 0 && my < mapHeight) {
                const mapIndex = (my * mapWidth + mx) * 4;
                if (cx !== undefined) sx += Math.round((mapData[mapIndex + cx] - 128) * dx);
                if (cy !== undefined) sy += Math.round((mapData[mapIndex + cy] - 128) * dy);
            }
            if (sx < 0 || sx >= sourceWidth || sy < 0 || sy >= sourceHeight) {
                if (mode === "color") { output.set(edgeColor, offset); continue; }
                if (mode === "ignore") { sx = x; sy = y; }
                else if (mode === "clamp") {
                    sx = Math.min(sourceWidth - 1, Math.max(0, sx)); sy = Math.min(sourceHeight - 1, Math.max(0, sy));
                } else {
                    sx = (sx % sourceWidth + sourceWidth) % sourceWidth; sy = (sy % sourceHeight + sourceHeight) % sourceHeight;
                }
            }
            const index = (sy * sourceWidth + sx) * 4;
            for (let component = 0; component < 4; ++component) output[offset + component] = sourceData[index + component];
        }
    }
    return image;
}

const tintSurfaces = [];

function paintTinted(node, context, tint, resources) {
    const box = localDisplayBounds(node), matrix = context.getTransform();
    const corners = [matrix.transformPoint({x: box.x, y: box.y}), matrix.transformPoint({x: box.x + box.width, y: box.y}),
        matrix.transformPoint({x: box.x, y: box.y + box.height}), matrix.transformPoint({x: box.x + box.width, y: box.y + box.height})];
    const measured = box.width > 0 && box.height > 0;
    const left = measured ? Math.max(0, Math.floor(Math.min(...corners.map(point => point.x)))) : 0;
    const top = measured ? Math.max(0, Math.floor(Math.min(...corners.map(point => point.y)))) : 0;
    const width = measured ? Math.min(context.canvas.width, Math.ceil(Math.max(...corners.map(point => point.x)))) - left : context.canvas.width;
    const height = measured ? Math.min(context.canvas.height, Math.ceil(Math.max(...corners.map(point => point.y)))) - top : context.canvas.height;
    if (!(width > 0 && height > 0)) return;
    const surface = tintSurfaces.pop() ?? document.createElement("canvas");
    if (surface.width !== width) surface.width = width;
    if (surface.height !== height) surface.height = height;
    const paint = surface.getContext("2d", { willReadFrequently: true });
    try {
        paint.resetTransform();
        paint.clearRect(0, 0, width, height);
        paint.imageSmoothingEnabled = context.imageSmoothingEnabled;
        paint.setTransform(matrix.a, matrix.b, matrix.c, matrix.d, matrix.e - left, matrix.f - top);
        paintDisplayContents(node, paint, resources);
        const image = paint.getImageData(0, 0, width, height), data = image.data;
        for (let offset = 0; offset < data.length; offset += 4) {
            if (!data[offset + 3]) continue;
            for (let channel = 0; channel < 4; ++channel)
                data[offset + channel] = data[offset + channel] * tint[channel] / 256 + tint[channel + 4];
        }
        paint.putImageData(image, 0, 0);
        context.resetTransform();
        context.drawImage(surface, left, top);
    } finally {
        if (width * height <= 262144 && tintSurfaces.length < 4) tintSurfaces.push(surface);
    }
}

function cloneBitmapDisplay(node) {
    const copy = cloneDisplay(node);
    const transfer = (source, destination) => {
        const tint = bitmapTints.get(source) ?? authoredTints.get(source);
        if (tint) bitmapTints.set(destination, tint);
        const bitmap = bitmapSurfaces.get(source);
        if (bitmap) bitmapSurfaces.set(destination, bitmap);
        for (let i = 0; i < source.children.length; ++i) transfer(source.children[i], destination.children[i]);
    };
    transfer(node, copy);
    return copy;
}

function paintDisplay(node, context, root = false, resources) {
    if (!root && (node.getAttribute("display") === "none" || node.getAttribute("visibility") === "hidden")) return;
    context.save();
    try {
        const list = root ? null : node.transform?.baseVal;
        for (let i = 0; list && i < list.numberOfItems; ++i) {
            const {a, b, c, d, e, f} = list.getItem(i).matrix;
            context.transform(a, b, c, d, e, f);
        }
        if (!root) context.globalAlpha *= Number(node.getAttribute("opacity") ?? 1);
        const tint = bitmapTints.get(node) ?? authoredTints.get(node);
        if (tint) paintTinted(node, context, tint, resources);
        else paintDisplayContents(node, context, resources);
    } finally { context.restore(); }
}

function paintDisplayContents(node, context, resources) {
        const value = name => Number(node.getAttribute(name) ?? 0);
        switch (node.localName) {
        case "use": {
            const href = node.getAttribute("href");
            const item = href?.startsWith("#") && document.getElementById(href.slice(1));
            if (item) paintDisplay(item, context, false, resources);
            break;
        }
        case "image": {
            const url = new URL(node.getAttribute("href"), documentUrl).href;
            const crisp = stageView.quality === "low" ? pixelMapImages.get(url)?.image : null;
            const image = crisp?.naturalWidth ? crisp : rasterImage(url);
            if (image.complete && image.naturalWidth) context.drawImage(image, value("x"), value("y"), value("width"), value("height"));
            else if (!image.complete && resources) (resources.waiting ??= new Set()).add(image);
            break;
        }
        case "foreignObject": {
            const surface = bitmapSurfaces.get(node)?.()?.canvas;
            if (surface?.width && surface?.height)
                context.drawImage(surface, value("x"), value("y"), value("width"), value("height"));
            break;
        }
        case "path":
        case "rect":
        case "circle":
        case "ellipse": {
            const shape = new Path2D(node.getAttribute("d") ?? "");
            if (node.localName === "rect") shape.rect(value("x"), value("y"), value("width"), value("height"));
            if (node.localName === "circle") shape.arc(value("cx"), value("cy"), value("r"), 0, 2 * Math.PI);
            if (node.localName === "ellipse") shape.ellipse(value("cx"), value("cy"), value("rx"), value("ry"), 0, 0, 2 * Math.PI);
            const fill = node.getAttribute("fill") ?? "black", stroke = node.getAttribute("stroke");
            const alpha = context.globalAlpha;
            if (fill !== "none") {
                const id = fill.match(/^url\(#(.+)\)$/)?.[1];
                const pattern = id && bitmapPatterns.get(document.getElementById(id));
                let brush = fill;
                if (pattern) {
                    brush = context.createPattern(pattern.bitmap.canvas, pattern.repeat ? "repeat" : "no-repeat");
                    const {a, b, c, d, tx, ty} = pattern.matrix ?? new Matrix();
                    brush?.setTransform(new DOMMatrix([a * 20, b * 20, c * 20, d * 20, tx * 20, ty * 20]));
                }
                context.fillStyle = brush ?? "transparent";
                context.globalAlpha = alpha * Math.min(1, Number(node.getAttribute("fill-opacity") ?? 1));
                context.fill(shape, node.getAttribute("fill-rule") === "evenodd" ? "evenodd" : "nonzero");
            }
            if (stroke && stroke !== "none") {
                context.strokeStyle = stroke;
                context.globalAlpha = alpha * Math.min(1, Number(node.getAttribute("stroke-opacity") ?? 1));
                context.lineWidth = Number(node.getAttribute("stroke-width") ?? 1);
                context.stroke(shape);
            }
            break;
        }
        default:
            for (const child of node.children)
                if (child instanceof SVGElement) paintDisplay(child, context, false, resources);
        }
}

function scriptBitmap(data, pixelSnapping = "auto", smoothing = false) {
    const view = scriptSprite("Bitmap", undefined, () => redraw());
    const node = displayNodes.get(view);
    const surface = element("foreignObject", { transform: "scale(20)" });
    const canvas = document.createElement("canvas");
    canvas.width = canvas.height = 0;
    const context = canvas.getContext("2d");
    canvas.style.cssText = "display:block;width:100%;height:100%";
    surface.append(canvas);
    node.firstChild.append(surface);
    let bitmap = null;
    bitmapSurfaces.set(surface, () => bitmap);
    const drawPixels = () => {
        if (!canvas.width || !canvas.height) return;
        const displacement = view.filters.find(filter => displayTypes.get(filter) === "DisplacementMapFilter" && filter.mapBitmap?.width && filter.mapBitmap.height);
        if (displacement) context.putImageData(displacePixels(context, bitmap, bitmap.rect, displacement), 0, 0);
        else {
            context.globalCompositeOperation = "copy";
            context.drawImage(bitmap.canvas, 0, 0);
        }
        if (node.closest("[data-flash-composite]")) surface.setAttribute("data-flash-pixels", bitmap.revision);
    };
    const redraw = () => {
        const width = bitmap?.width ?? 0, height = bitmap?.height ?? 0;
        if (canvas.width !== width || canvas.height !== height) {
            canvas.width = width;
            canvas.height = height;
            surface.setAttribute("width", width);
            surface.setAttribute("height", height);
        }
        if (width && height) pendingBitmapDraws.add(drawPixels);
        else pendingBitmapDraws.delete(drawPixels);
    };
    const observer = new WeakRef(redraw);
    view.pixelSnapping = pixelSnapping;
    Object.defineProperties(view, {
        smoothing: { get: () => smoothing, set: value => {
            smoothing = Boolean(value);
            canvas.style.imageRendering = smoothing ? "auto" : "pixelated";
        } },
        bitmapData: {
            get: () => bitmap,
            set: value => {
                bitmap?.listeners?.delete(observer);
                bitmap = value ?? null;
                bitmap?.listeners?.add(observer);
                redraw();
            },
        },
        width: { get: () => (bitmap?.width ?? 0) * Math.abs(view.scaleX), set: value => {
            if (bitmap?.width) view.scaleX = Number(value) / bitmap.width * (Math.sign(view.scaleX) || 1);
        } },
        height: { get: () => (bitmap?.height ?? 0) * Math.abs(view.scaleY), set: value => {
            if (bitmap?.height) view.scaleY = Number(value) / bitmap.height * (Math.sign(view.scaleY) || 1);
        } },
    });
    view.smoothing = smoothing;
    view.bitmapData = data;
    return view;
}

class Point {
    constructor(x = 0, y = 0) { this.x = x; this.y = y; }
    clone() { return new Point(this.x, this.y); }
    get length() { return Math.hypot(this.x, this.y); }
    normalize(length) {
        const previous = this.length;
        if (previous) { this.x *= length / previous; this.y *= length / previous; }
    }
    offset(dx, dy) { this.x += dx; this.y += dy; }
    equals(p) { return this.x === p?.x && this.y === p?.y; }
    add(p) { return new Point(this.x + p.x, this.y + p.y); }
    subtract(p) { return new Point(this.x - p.x, this.y - p.y); }
}

class ColorTransform {
    constructor(redMultiplier = 1, greenMultiplier = 1, blueMultiplier = 1, alphaMultiplier = 1,
        redOffset = 0, greenOffset = 0, blueOffset = 0, alphaOffset = 0) {
        Object.assign(this, { redMultiplier, greenMultiplier, blueMultiplier, alphaMultiplier,
            redOffset, greenOffset, blueOffset, alphaOffset });
    }
    get color() { return (this.redOffset & 255) << 16 | (this.greenOffset & 255) << 8 | this.blueOffset & 255; }
    set color(value) {
        this.redMultiplier = this.greenMultiplier = this.blueMultiplier = 0;
        this.redOffset = value >>> 16 & 255;
        this.greenOffset = value >>> 8 & 255;
        this.blueOffset = value & 255;
    }
    concat(other) {
        for (const channel of ["red", "green", "blue", "alpha"]) {
            const multiplier = channel + "Multiplier", offset = channel + "Offset";
            this[offset] += this[multiplier] * other[offset];
            this[multiplier] *= other[multiplier];
        }
    }
}

class Matrix {
    constructor(a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0) {
        Object.assign(this, { a, b, c, d, tx, ty });
    }
    clone() { return new Matrix(this.a, this.b, this.c, this.d, this.tx, this.ty); }
    concat(after) {
        const left = new DOMMatrix([after.a, after.b, after.c, after.d, after.tx, after.ty]);
        const {a, b, c, d, e, f} = left.multiply(new DOMMatrix([this.a, this.b, this.c, this.d, this.tx, this.ty]));
        Object.assign(this, { a, b, c, d, tx: e, ty: f });
    }
    translate(x, y) { this.tx += x; this.ty += y; }
    scale(x, y) { this.concat(new Matrix(x, 0, 0, y)); }
    transformPoint(point) { return new Point(this.a * point.x + this.c * point.y + this.tx, this.b * point.x + this.d * point.y + this.ty); }
}

class Rectangle {
    constructor(x = 0, y = 0, width = 0, height = 0) { Object.assign(this, { x, y, width, height }); }
    get left() { return this.x; } set left(value) { this.width = this.right - value; this.x = value; }
    get right() { return this.x + this.width; } set right(value) { this.width = value - this.x; }
    get top() { return this.y; } set top(value) { this.height = this.bottom - value; this.y = value; }
    get bottom() { return this.y + this.height; } set bottom(value) { this.height = value - this.y; }
    clone() { return new Rectangle(this.x, this.y, this.width, this.height); }
    contains(x, y) { return x >= this.left && x < this.right && y >= this.top && y < this.bottom; }
    containsPoint(point) { return this.contains(point.x, point.y); }
    offset(x, y) { this.x += x; this.y += y; }
}

)JS" R"JS(function displayBounds(node, target) {
    const bounds = localDisplayBounds(node);
    const { x, y, width, height } = bounds;
    const targetNode = displayNodes.get(target);
    if (!targetNode || targetNode === node) return new Rectangle(x / 20, y / 20, width / 20, height / 20);
    const to = targetNode.getScreenCTM();
    const from = to && node.getScreenCTM();
    if (!from) return new Rectangle(x / 20, y / 20, width / 20, height / 20);
    const matrix = to.inverse().multiply(from);
    if (![matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f].every(Number.isFinite))
        return new Rectangle(x / 20, y / 20, width / 20, height / 20);
    const box = transformedBounds(bounds, matrix);
    return new Rectangle(box.x / 20, box.y / 20, box.width / 20, box.height / 20);
}

function localDisplayMatrix(node) {
    const list = node.transform?.baseVal;
    if (list?.numberOfItems === 1) return list.getItem(0).matrix;
    let matrix = new DOMMatrix();
    for (let i = 0; i < (list?.numberOfItems ?? 0); ++i) matrix = matrix.multiply(list.getItem(i).matrix);
    return matrix;
}

function transformedBounds({x, y, width, height}, {a, b, c, d, e, f}) {
    return new Rectangle(a * x + c * y + e + Math.min(0, a * width) + Math.min(0, c * height),
        b * x + d * y + f + Math.min(0, b * width) + Math.min(0, d * height),
        Math.abs(a * width) + Math.abs(c * height), Math.abs(b * width) + Math.abs(d * height));
}

function childDisplayBounds(node, measure) {
    let result;
    for (const child of node.children) {
        if (!(child instanceof SVGGraphicsElement) || child.getAttribute("display") === "none" ||
            child.hasAttribute("data-flash-hit")) continue;
        const bounds = measure(child);
        if (!bounds || (!bounds.width && !bounds.height)) continue;
        const box = transformedBounds(bounds, localDisplayMatrix(child));
        if (!result) result = box;
        else result = new Rectangle(Math.min(result.left, box.left), Math.min(result.top, box.top),
            Math.max(result.right, box.right) - Math.min(result.left, box.left),
            Math.max(result.bottom, box.bottom) - Math.min(result.top, box.top));
    }
    return result;
}

function detachedDisplayBounds(node) {
    const value = key => Number(node.getAttribute(key) ?? 0);
    switch (node.localName) {
    case "use": {
        const href = node.getAttribute("href");
        const source = href?.startsWith("#") && document.getElementById(href.slice(1));
        return source ? detachedDisplayBounds(source) : null;
    }
    case "rect": case "image": case "foreignObject":
        return new Rectangle(value("x"), value("y"), value("width"), value("height"));
    case "circle": case "ellipse": {
        const x = value("r") || value("rx"), y = value("r") || value("ry");
        return new Rectangle(value("cx") - x, value("cy") - y, x * 2, y * 2);
    }
    case "path": {
        const coordinates = node.getAttribute("d")?.match(/-?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?/gi)?.map(Number) ?? [];
        if (!coordinates.length) return null;
        let left = coordinates[0], right = left, top = coordinates[1], bottom = top;
        for (let i = 2; i + 1 < coordinates.length; i += 2) {
            left = Math.min(left, coordinates[i]); right = Math.max(right, coordinates[i]);
            top = Math.min(top, coordinates[i + 1]); bottom = Math.max(bottom, coordinates[i + 1]);
        }
        return new Rectangle(left, top, right - left, bottom - top);
    }
    default: return childDisplayBounds(node, detachedDisplayBounds);
    }
}

function localDisplayBounds(node) {
    // Button hit geometry participates in pointer testing only, even in an enclosing popup's bounds.
    if (node.querySelector("[data-flash-hit]")) return childDisplayBounds(node, localDisplayBounds) ?? new Rectangle();
    const bounds = node.getBBox();
    if (bounds.width || bounds.height) return bounds;
    return detachedDisplayBounds(node) ?? bounds;
}

function displayDimension(node, dimension) {
    const { width, height } = localDisplayBounds(node);
    const matrix = localDisplayMatrix(node);
    return (dimension === "width" ? Math.abs(matrix.a) * width + Math.abs(matrix.c) * height :
        Math.abs(matrix.b) * width + Math.abs(matrix.d) * height) / 20;
}

function resizeDisplay(node, view, dimension, value) {
    const previous = displayDimension(node, dimension);
    const scale = dimension === "width" ? "scaleX" : "scaleY";
    if (previous && Number.isFinite(Number(value))) view[scale] *= Number(value) / previous;
}

function scriptDispatcher() {
    const events = new EventTarget();
    const view = Object.assign(eventView(events), { bindView: instance => displayViews.set(events, instance) });
    view.bindView(view);
    return view;
}

function scriptButton(up, over, down, hit) {
    const view = scriptSprite("SimpleButton");
    const node = displayNodes.get(view);
    const drawing = element("g"), hitArea = element("g", { opacity: 0 });
    const states = { upState: up, overState: over, downState: down, hitTestState: hit };
    let current = "upState", enabled = true, useHandCursor = true, pressed = false, hitSource, hitCopy;
    hitArea.setAttribute("data-flash-hit", "");
    node.append(drawing, hitArea);
    const apply = () => {
        const visible = displayNodes.get(states[current] ?? states.upState);
        const target = displayNodes.get(states.hitTestState);
        drawing.replaceChildren(...(visible ? [visible] : []));
        if (target !== hitSource) {
            hitSource = target;
            hitCopy = target ? cloneDisplay(target) : null;
            hitArea.replaceChildren(...(hitCopy ? [hitCopy] : []));
        }
        drawing.style.pointerEvents = target ? "none" : "";
        node.style.pointerEvents = enabled ? "" : "none";
        hitArea.toggleAttribute("data-flash-decoration", !enabled);
        node.style.cursor = enabled && useHandCursor ? "pointer" : "";
    };
    for (const key of Object.keys(states))
        Object.defineProperty(view, key, {
            get: () => states[key] ?? null,
            set: value => { states[key] = value; apply(); },
        });
    Object.defineProperties(view, {
        enabled: { get: () => enabled, set: value => { enabled = Boolean(value); apply(); } },
        useHandCursor: { get: () => useHandCursor, set: value => { useHandCursor = Boolean(value); apply(); } },
    });
    node.setAttribute("role", "button");
    node.setAttribute("tabindex", "0");
    node.addEventListener("pointerenter", () => { current = pressed ? "downState" : "overState"; apply(); });
    node.addEventListener("pointerdown", () => { pressed = true; current = "downState"; apply(); });
    node.addEventListener("pointerup", () => { pressed = false; current = "overState"; apply(); });
    node.addEventListener("pointerleave", () => { pressed = false; current = "upState"; apply(); });
    node.addEventListener("keydown", event => {
        if (enabled && !event.repeat && (event.key === " " || event.key === "Enter")) {
            event.preventDefault();
            node.dispatchEvent(new MouseEvent("click", { bubbles: true }));
        }
    });
    apply();
    return view;
}

)JS" R"JS(function httpAddress(value, base = mainBundle?.source ?? documentUrl) {
    const original = new URL(String(value), base);
    const path = String(value).split(/[?#]/)[0];
    const route = httpRoutes[original.pathname] ?? httpRoutes[path];
    const url = route ? new URL(route, documentUrl) : original;
    if (url !== original) for (const [key, item] of original.searchParams) url.searchParams.append(key, item);
    if (url.protocol !== "http:" && url.protocol !== "https:") throw new TypeError("Adresse HTTP invalide.");
    return url;
}

const browserMedia = mediaAdapters({
    dispatcher: scriptDispatcher,
    emit: (view, type, detail) => {
        const observed = view.hasEventListener(type);
        view.dispatchEvent(scriptEvent(type, false, false, detail));
        if (!observed && detail?.text && (type === "ioError" || type === "securityError"))
            scriptProblem(new Error(detail.text));
    },
    resolve: httpAddress,
    socketEndpoint: (host, port) => {
        const name = String(host ?? "").trim();
        const key = name.toLowerCase() + ":" + Number(port);
        const value = Object.hasOwn(sockets, key) ? sockets[key] : /^(ws|wss):\/\//i.test(name) ? name : null;
        if (!value) return null;
        const url = new URL(value, location.href);
        if (url.protocol === "http:") url.protocol = "ws:";
        if (url.protocol === "https:") url.protocol = "wss:";
        if (url.protocol !== "ws:" && url.protocol !== "wss:") throw new TypeError("Adresse WebSocket invalide.");
        return url;
    },
    problem: scriptProblem,
});

function applicationDomain(parent = null) {
    const libraries = [];
    return {
        parentDomain: parent,
        install(library) { if (!libraries.includes(library)) libraries.push(library); },
        hasDefinition(name) {
            const key = String(name).replace("::", ".");
            return libraries.some(library => library.has(key)) || Boolean(parent?.hasDefinition(key));
        },
        getDefinition(name) {
            const key = String(name).replace("::", ".");
            const library = libraries.find(item => item.has(key));
            if (library) return library.definition(key);
            if (parent?.hasDefinition(key)) return parent.getDefinition(key);
            throw new ReferenceError("Définition ActionScript introuvable : " + name);
        },
    };
}

)JS" R"JS(function moviePage(raw, url, bundle) {
    if (catalog && url.origin === documentUrl.origin) {
        const path = decodeURIComponent(url.pathname);
        const source = decodeURIComponent(bundle.source.pathname);
        const route = Object.hasOwn(movieRoutes, source) ? movieRoutes[source] : null;
        const requested = route && Object.hasOwn(route, path) ? route[path] : path;
        const entry = catalog[requested] ?? catalog[requested.toLowerCase()];
        if (entry) return new URL(entry, documentUrl);
    }
    const path = raw.split(/[?#]/)[0];
    const local = Object.entries(bundle.scene.dependencies).find(([source]) => source.split(/[?#]/)[0] === path);
    return local ? new URL(local[1], bundle.page) : null;
}

function bitmapRenderer(type) {
    const usesBitmap = entry => entry?.call && /"BitmapData"|\["draw"\]|\["drawWithQuality"\]/.test(entry.call.toString());
    return type && (usesBitmap(type.construct) || usesBitmap(type.initialize) ||
        Object.values(type.methods).some(usesBitmap) || Object.values(type.statics).some(usesBitmap));
}

function timelineAssetUrls(scene, page, roots, visited = new Set(), frameLimit = Infinity) {
    const urls = [];
    const visit = id => {
        if (visited.has(id)) return;
        visited.add(id);
        const url = scene.assets[id]?.url;
        if (url) urls.push(new URL(url, page).href);
        const frames = scene.clips[id]?.frames ?? [];
        for (let i = 0; i < Math.min(frameLimit, frames.length); ++i)
            for (const row of frames[i][0]) visit(timelineAsset(row, scene));
        for (const state of scene.buttons[id]?.states ?? []) for (const row of state) visit(timelineAsset(row, scene));
    };
    for (const id of roots) visit(id);
    return urls;
}

function preloadBitmapSources(scene, definitions, page) {
    const realms = new Map();
    const renders = name => {
        if (!realms.has(name)) {
            realms.set(name, false);
            const type = definitions[name];
            if (type) realms.set(name, bitmapRenderer(type) || renders(type.base));
        }
        return realms.get(name);
    };
    const roots = Object.entries(scene.clips).filter(([, clip]) => clip.type && renders(clip.type)).map(([id]) => Number(id));
    const urls = timelineAssetUrls(scene, page, roots);
    return Promise.all(urls.map(url => rasterImage(url).decode().catch(() => {})));
}

async function preloadLinkedPixels(scene, page) {
    const loaded = new Map();
    await Promise.all(Object.entries(scene.assets).filter(([, asset]) => asset.type && asset.url).map(async ([id, asset]) => {
        const image = rasterImage(new URL(asset.url, page).href);
        try { await image.decode(); loaded.set(Number(id), image); }
        catch { /* The usual blank BitmapData is available if the asset is missing. */ }
    }));
    if (loaded.size) linkedPixels.set(scene, loaded);
}

)JS" R"JS(function trimMovieRequests() {
    while (cachedMovieBytes > movieByteBudget || movieRequests.size > 32) {
        const [url, request] = movieRequests.entries().next().value;
        cachedMovieBytes -= cachedMovieSizes.get(request) ?? 0;
        movieRequests.delete(url);
    }
}

async function loadMovie(page, address, original, domain) {
    if (page.origin !== documentUrl.origin) throw new Error("Le projet lié doit être sur la même origine.");
    let pending = movieRequests.get(page.href);
    if (pending) {
        movieRequests.delete(page.href);
        movieRequests.set(page.href, pending);
    } else {
        pending = Promise.all([
            fetch(new URL("scene.json", page)).then(response => {
                if (!response.ok) throw new Error("Scène du projet lié indisponible.");
                return response.json();
            }),
            import(new URL("classes.js", page).href),
            import(new URL("actions.js", page).href),
            original ? original() : fetch(new URL("source.bin", page)).then(response => response.ok ? response.arrayBuffer() : null),
        ]).then(async ([scene, classes, script, bytes]) => {
            const lazy = lazyAssetPaths.some(prefix => address.pathname.startsWith(prefix));
            const pixels = Promise.all([loadPixelMapImages(scene, page), preloadLinkedPixels(scene, page), loadFonts(scene, page)]);
            if (script.actions.length || Object.values(scene.clips).some(clip => clip.legacyInitialize))
                await Promise.all([pixels, ...Object.values(scene.assets).filter(asset => asset.url).map(asset =>
                    rasterImage(new URL(asset.url, page).href).decode().catch(() => {}))]);
            else await Promise.all([pixels, preloadBitmapSources(scene, classes.definitions, page)]);
            return { scene, definitions: classes.definitions, actions: script.actions, bytes, domains: new WeakMap(), lazy };
        });
        movieRequests.set(page.href, pending);
        pending.then(assets => {
            if (movieRequests.get(page.href) !== pending) return;
            const size = assets.bytes?.byteLength ?? 0;
            cachedMovieSizes.set(pending, size);
            cachedMovieBytes += size;
            trimMovieRequests();
        }, () => { if (movieRequests.get(page.href) === pending) movieRequests.delete(page.href); });
        trimMovieRequests();
    }
    const assets = await pending;
    let bundle = assets.domains.get(domain);
    if (!bundle) {
        bundle = makeBundle(assets.scene, assets.actions, assets.definitions, page, address);
        bundle.domain = domain;
        if (assets.lazy) bundle.primedClips = new Set();
        assets.domains.set(domain, bundle);
        domain.install(bundle.classes);
    }
    return { bundle, bytes: assets.bytes };
}

function scriptLoader(owner = mainBundle) {
    const view = scriptSprite("Loader"), node = displayNodes.get(view);
    const screen = element("g");
    const info = scriptDispatcher();
    node.append(screen);
    let pending, clip;
    Object.assign(info, { bytes: null, bytesLoaded: 0, bytesTotal: 0, loaderURL: location.href, url: "", applicationDomain: null, content: null });
    Object.defineProperty(view, "contentLoaderInfo", { value: info, enumerable: true });
    Object.defineProperty(view, "content", { get: () => info.content });
    const event = (type, fields) => info.dispatchEvent(scriptEvent(type, false, false, fields));
    const reset = () => {
        pending?.abort();
        pending = null;
        if (clip) {
            clip.detach();
            scriptedNodes.delete(clip.node);
            clip = null;
        }
        screen.replaceChildren();
        Object.assign(info, { bytes: null, bytesLoaded: 0, bytesTotal: 0, content: null, applicationDomain: null });
    };
    const loadImage = (url, signal) => new Promise((resolve, reject) => {
        const image = new Image();
        image.onload = () => resolve(image);
        image.onerror = () => reject(new Error("Image introuvable : " + url));
        signal.addEventListener("abort", () => { image.onload = image.onerror = null; image.src = ""; reject(new DOMException("Chargement annulé.", "AbortError")); }, { once: true });
        image.src = url;
    });
    view.close = () => { pending?.abort(); pending = null; };
    view.unload = () => { reset(); event("unload"); };
    view.unloadAndStop = view.unload;
    const start = () => {
        if (!clip || !node.isConnected) return;
        scriptedNodes.add(clip.node);
        clip.start();
        clip.syncStream();
    };
    node.addEventListener("addedToStage", start);
    node.addEventListener("removedFromStage", () => clip?.detach());
    const load = async (request, context, binary) => {
        reset();
        const current = new AbortController();
        pending = current;
        try {
            const raw = String(request?.url ?? request ?? "");
            const registered = binary && movieBytes.get(binary);
            const [url, options] = registered ? [registered.url, {}] :
                browserMedia.requestOptions(request, value => httpAddress(value, owner.source));
            info.url = url.href;
            const page = registered?.page ?? (!binary && moviePage(raw, url, owner));
            event("open");
            if (page) {
                const domain = context?.applicationDomain ?? applicationDomain(mainDomain);
                const { bundle, bytes } = await loadMovie(page, url, binary ? () => binary.bytes().buffer : null, domain);
                if (pending !== current) return;
                info.applicationDomain = domain;
                info.bytes = bytes && new ByteArray(bytes, true);
                if (info.bytes) movieBytes.set(info.bytes, { page, url });
                info.bytesLoaded = info.bytesTotal = bytes?.byteLength ?? 0;
                const display = element("g");
                const layer = { row: [0, 0, [1, 0, 0, 1, 0, 0], 1, "", 0, true],
                    node: display, overrides: Object.create(null) };
                clip = new Clip(0, display, [], null, layer, true, bundle);
                clip.info = info;
                authoredClips.set(display, clip);
                displayParents.set(display, () => view);
                info.content = clip.view;
                clip.seek(1, false);
                screen.append(display);
                clip.initializeTree();
                start();
            } else {
                const bytes = binary?.bytes();
                if (/\.swf$/i.test(url.pathname) || bytes && [67, 70, 90].includes(bytes[0]) && bytes[1] === 87 && bytes[2] === 83)
                    throw new Error("Projet SWF lié introuvable : " + raw);
                let blob;
                if (bytes) blob = new Blob([bytes]);
                else {
                    const response = await fetch(url, { ...options, signal: current.signal });
                    if (!response.ok) throw new Error("HTTP " + response.status + " · " + url);
                    blob = await response.blob();
                }
                const objectUrl = URL.createObjectURL(blob);
                try {
                    const image = await loadImage(objectUrl, current.signal);
                    if (pending !== current) return;
                    const content = scriptSprite("DisplayObject");
                    const surface = element("foreignObject", { width: image.naturalWidth * 20, height: image.naturalHeight * 20 });
                    image.style.cssText = "display:block;width:100%;height:100%";
                    surface.append(image);
                    displayNodes.get(content).append(surface);
                    screen.append(displayNodes.get(content));
                    info.content = content;
                    info.bytes = new ByteArray(await blob.arrayBuffer());
                    info.bytesLoaded = info.bytesTotal = blob.size;
                } finally { URL.revokeObjectURL(objectUrl); }
            }
            if (pending === current) {
                event("progress", { bytesLoaded: info.bytesLoaded, bytesTotal: info.bytesTotal });
                event("init");
                event("complete");
            }
        } catch (error) {
            if (pending === current && error.name !== "AbortError") event("ioError", { text: error.message });
        } finally { if (pending === current) pending = null; }
    };
    view.load = (request, context) => load(request, context);
    view.loadBytes = (bytes, context) => load("", context, bytes);
    return view;
}

function nativeObject(name, ...args) {
    const view = makeNativeObject(name, ...args);
    if (typeof view === "object" && view !== null) displayTypes.set(view, name);
    return view;
}

function makeNativeObject(name, ...args) {
    switch (name) {
    case "Sprite": case "MovieClip": case "Shape": return scriptSprite(name);
    case "SimpleButton": return scriptButton(...args);
    case "TextField": {
        const field = createTextField({}, (type, detail) => node.dispatchEvent(scriptEvent(type, true, false, detail)));
        const view = scriptSprite("TextField", field.offsetX);
        const node = displayNodes.get(view);
        node.append(field.node);
        for (const [key, property] of Object.entries(Object.getOwnPropertyDescriptors(field)))
            if (key !== "node" && key !== "offsetX") Object.defineProperty(view, key, property);
        return view;
    }
    case "TextFormat": return new TextFormat(...args);
    case "StyleSheet": return new StyleSheet();
    case "BitmapData": return new BitmapData(...args);
    case "Bitmap": return scriptBitmap(...args);
    case "GlowFilter": return glowFilter(...args);
    case "BlurFilter": {
        const [blurX = 4, blurY = 4, quality = 1] = args;
        return { blurX, blurY, quality };
    }
    case "DropShadowFilter": return dropShadowFilter(...args);
    case "BevelFilter": return bevelFilter(...args);
    case "DisplacementMapFilter": return displacementFilter(...args);
    case "ColorTransform": return new ColorTransform(...args);
    case "Matrix": return new Matrix(...args);
    case "Font": return Object.assign({ fontName: "sans-serif", fontStyle: "regular", fontType: "device" }, args[0]);
    case "ByteArray": return new ByteArray();
    case "URLRequest": return new URLRequest(...args);
    case "URLRequestHeader": return new URLRequestHeader(...args);
    case "URLVariables": return new URLVariables(...args);
    case "URLLoader": return browserMedia.urlLoader(...args);
    case "Socket": case "XMLSocket": {
        const view = browserMedia.socket(name === "XMLSocket");
        if (args[0]) view.connect(...args);
        return view;
    }
    case "LocalConnection": return browserMedia.localConnection();
    case "Sound": return browserMedia.sound(...args);
    case "SoundTransform": return new SoundTransform(...args);
    case "SoundLoaderContext": return { bufferTime: args[0] ?? 1000, checkPolicyFile: Boolean(args[1]) };
    case "Loader": return scriptLoader();
    case "ApplicationDomain": return applicationDomain(args[0] ?? mainDomain);
    case "LoaderContext": return { checkPolicyFile: Boolean(args[0]), applicationDomain: args[1], securityDomain: args[2] };
    case "XML": case "XMLList": return scriptXml(args[0], name);
    case "Timer": return scriptTimer(...args);
    case "Point": return new Point(...args);
    case "Rectangle": return new Rectangle(...args);
    case "EventDispatcher": return scriptDispatcher();
    default: throw new Error("Construction Flash à porter : " + name);
    }
}

const localObjects = new Map();
const sharedObject = {
    getLocal(name, path = "", secure = false) {
        if (secure && location.protocol !== "https:") return null;
        const key = "flashstudio:shared:" + JSON.stringify([path || mainBundle?.source.pathname || "/", String(name)]);
        if (localObjects.has(key)) return localObjects.get(key);
        let stored;
        try { stored = JSON.parse(localStorage.getItem(key)); } catch {}
        const object = {
            data: Object.assign(Object.create(null), stored),
            flush() {
                try { localStorage.setItem(key, JSON.stringify(this.data)); return "flushed"; }
                catch { return "pending"; }
            },
            clear() {
                for (const property of Object.keys(this.data)) delete this.data[property];
                try { localStorage.removeItem(key); } catch {}
            },
        };
        localObjects.set(key, object);
        return object;
    },
};

)JS" R"JS(let numLock = false, capsLock = false;
function trackLockKeys(event) {
    numLock = event.getModifierState("NumLock");
    capsLock = event.getModifierState("CapsLock");
}
for (const type of ["keydown", "keyup", "pointerdown"]) stage.addEventListener(type, trackLockKeys, true);

const nativeSymbols = {
    BitmapDataChannel: { RED: 1, GREEN: 2, BLUE: 4, ALPHA: 8 },
    BitmapFilterType: { INNER: "inner", OUTER: "outer", FULL: "full" },
    DisplacementMapFilterMode: { WRAP: "wrap", CLAMP: "clamp", IGNORE: "ignore", COLOR: "color" },
    URLRequestMethod: { GET: "GET", POST: "POST", PUT: "PUT", DELETE: "DELETE", HEAD: "HEAD", OPTIONS: "OPTIONS" },
    URLLoaderDataFormat: { TEXT: "text", BINARY: "binary", VARIABLES: "variables" },
    Endian: { BIG_ENDIAN: "bigEndian", LITTLE_ENDIAN: "littleEndian" },
    StageScaleMode: { NO_SCALE: "noScale", EXACT_FIT: "exactFit", SHOW_ALL: "showAll", NO_BORDER: "noBorder" },
    StageAlign: { TOP: "T", BOTTOM: "B", LEFT: "L", RIGHT: "R", TOP_LEFT: "TL", TOP_RIGHT: "TR", BOTTOM_LEFT: "BL", BOTTOM_RIGHT: "BR" },
    BlendMode: { NORMAL: "normal", LAYER: "layer", MULTIPLY: "multiply", SCREEN: "screen", LIGHTEN: "lighten", DARKEN: "darken",
        DIFFERENCE: "difference", ADD: "add", SUBTRACT: "subtract", INVERT: "invert", ALPHA: "alpha", ERASE: "erase", OVERLAY: "overlay", HARDLIGHT: "hardlight" },
    Mouse: { show: () => stage.removeAttribute("data-flash-hide-cursor"), hide: () => stage.setAttribute("data-flash-hide-cursor", "") },
    Keyboard: { get numLock() { return numLock; }, get capsLock() { return capsLock; },
        ENTER: 13, SPACE: 32, LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40, SHIFT: 16, CONTROL: 17, ESCAPE: 27, TAB: 9 },
    TextFieldType: { INPUT: "input", DYNAMIC: "dynamic" },
    TextFieldAutoSize: { NONE: "none", LEFT: "left", RIGHT: "right", CENTER: "center" },
    Security: { loadPolicyFile() {}, allowDomain() {}, allowInsecureDomain() {} },
    Capabilities: { version: navigator.appVersion },
    SharedObject: sharedObject,
    ExternalInterface: {
        available: true,
        call(name, ...args) {
            const names = String(name).split(".");
            let receiver = globalThis.flashStudioCallbacks;
            for (const key of names.slice(0, -1)) {
                if (!receiver || !Object.hasOwn(receiver, key)) return null;
                receiver = receiver[key];
            }
            const key = names.at(-1);
            const method = receiver && Object.hasOwn(receiver, key) ? receiver[key] : null;
            return typeof method === "function" ? Reflect.apply(method, receiver, args) : null;
        },
    },
    SecurityDomain: { currentDomain: Object.freeze({ origin: location.origin }) },
    SoundMixer: {
        get soundTransform() { return browserMedia.masterTransform; },
        set soundTransform(value) {
            browserMedia.masterTransform = value;
            for (const clip of activeStreams) clip.stream.audio.volume = browserMedia.masterVolume;
        },
        stopAll: () => { browserMedia.stopAll(); root?.stopSounds(); },
    },
    getTimer: () => Math.trunc(performance.now() - playbackEpoch),
    navigateToURL: browserMedia.navigate,
    getDefinitionByName: name => mainDomain.getDefinition(name),
    getQualifiedClassName(value) {
        if (value === null) return "null";
        if (value === undefined) return "void";
        const type = convertedClassName(value);
        if (type) return type.replace(/\.([^.]+)$/, "::$1");
        if (typeof value === "number") return Number.isInteger(value) && value >= -2147483648 && value <= 2147483647 ? "int" : "Number";
        if (Array.isArray(value)) return "Array";
        const native = displayTypes.get(value);
        if (native) {
            if (["TextField", "TextFormat", "Font", "StyleSheet"].includes(native)) return "flash.text::" + native;
            if (["MovieClip", "Sprite", "Bitmap", "BitmapData", "Loader", "Shape", "SimpleButton", "DisplayObject"].includes(native))
                return "flash.display::" + native;
            return native;
        }
        return ({ string: "String", boolean: "Boolean", function: "Function" })[typeof value] ?? "Object";
    },
    ApplicationDomain: { get currentDomain() { return mainDomain; } },
};

)JS" R"JS(const nativeClasses = new Map();
const registeredFonts = new Map();
const classHooks = {
    native: nativeObject,
    bindNativeView(native, view) {
        const type = displayTypes.get(native);
        if (type !== undefined) displayTypes.set(view, type);
    },
    nativeClass(name) {
        if (!nativeClasses.has(name)) {
            const type = function (...args) { return nativeObject(name, ...args); };
            Object.defineProperty(type, Symbol.hasInstance, { value: value => classHooks.isType(value, name) });
            if (name === "Font") {
                type.registerFont = fontClass => {
                    if (registeredFonts.has(fontClass)) return;
                    const font = new fontClass();
                    if (!(font instanceof type)) throw new TypeError("Classe de police ActionScript invalide.");
                    registeredFonts.set(fontClass, font);
                };
                type.enumerateFonts = () => [...registeredFonts.values()];
            }
            nativeClasses.set(name, type);
        }
        return nativeClasses.get(name);
    },
    nativeSymbol: name => nativeSymbols[name],
    event: (kind, type, bubbles, cancelable, detail, extra) => {
        const fields = kind === "StatusEvent" ? { code: detail, level: extra } :
            kind === "ProgressEvent" ? { bytesLoaded: detail, bytesTotal: extra } :
            kind === "HTTPStatusEvent" ? { status: detail } :
            kind === "DataEvent" ? { text: detail, data: detail } : { text: detail };
        return Object.assign(scriptEvent(type, bubbles, cancelable, undefined, kind), fields);
    },
    eventName: name => name.toLowerCase().replace(/_([a-z])/g, (_, letter) => letter.toUpperCase()),
    isType: (value, type) => {
        if (type.endsWith("Event")) return isEvent(value, type);
        if (type === "DisplayObject") return displayNodes.has(value);
        const displayed = displayTypes.get(value);
        if (type === "Sprite") return displayed === "MovieClip" || displayed === "Sprite";
        if (type === "DisplayObjectContainer") return displayed === "MovieClip" || displayed === "Sprite" || displayed === "Loader";
        if (type === "InteractiveObject")
            return ["MovieClip", "Sprite", "Loader", "SimpleButton", "TextField"].includes(displayed);
        if (type === "BitmapFilter") return ["GlowFilter", "BlurFilter", "DropShadowFilter", "BevelFilter", "DisplacementMapFilter"].includes(displayed);
        if (type === "Point") return value instanceof Point;
        if (type === "Rectangle") return value instanceof Rectangle;
        if (type === "Matrix") return value instanceof Matrix;
        if (type === "ColorTransform") return value instanceof ColorTransform;
        if (type === "ByteArray") return value instanceof ByteArray;
        if (type === "URLRequest") return value instanceof URLRequest;
        if (type === "URLVariables") return value instanceof URLVariables;
        if (type === "SoundTransform") return value instanceof SoundTransform;
        if (type === "SoundChannel") return browserMedia.isSoundChannel(value);
        if (type === "XML" || type === "XMLList") return isXml(value, type);
        return displayTypes.get(value) === type;
    },
    host: (chain, defs, bundle) => {
        const symbol = bundle?.symbols.get(chain[0]);
        const base = defs[chain.at(-1)]?.base;
        if (base === "flash.display.BitmapData") {
            const pixels = nativeObject("BitmapData", 1, 1, true, 0);
            pixels.linkedImage = linkedPixels.get(bundle.scene)?.get(symbol);
            return pixels;
        }
        if (symbol !== undefined) {
            if (bundle.scene.clips[symbol]) return makeLinkedClip(symbol, bundle);
            if (bundle.scene.buttons[symbol]) return makeLinkedButton(symbol, bundle);
            const sound = bundle.scene.audio[symbol];
            if (sound) {
                const view = browserMedia.embeddedSound(new URL(sound, bundle.page).href, bundle.scene.audioTiming?.[symbol]);
                displayTypes.set(view, "Sound");
                return view;
            }
        }
        if (base === "flash.events.Event") return inheritedEvent();
        if (base === "flash.events.EventDispatcher") return scriptDispatcher();
        if (base === "flash.display.Sprite" || base === "flash.display.MovieClip") return scriptSprite(base.split(".").pop());
        if (base === "flash.display.SimpleButton") return scriptButton();
        const native = base?.split(".").pop();
        if (native === "Loader") return scriptLoader(bundle);
        if (native === "Timer") return nativeObject("Timer", 0);
        if (native === "Font") return nativeObject(native, bundle?.scene.fontClasses?.[chain[0]]);
        if (["Shape", "Bitmap", "TextField", "Socket", "XMLSocket", "URLLoader", "LocalConnection", "Sound", "ByteArray"].includes(native))
            return nativeObject(native);
        return null;
    },
    initializeBase: (base, host, args) => {
        if (base === "flash.events.Event") host?.initialize(...args);
        if (base === "flash.display.BitmapData" && host) {
            const image = host.linkedImage;
            if (image) host.initialize(image.naturalWidth, image.naturalHeight, true, 0, image);
            else host.initialize(...args);
            delete host.linkedImage;
        }
        if (base === "flash.display.Bitmap" && host) {
            host.bitmapData = args[0] ?? null;
            host.pixelSnapping = args[1] ?? "auto";
            host.smoothing = Boolean(args[2]);
        }
        if (base === "flash.utils.Timer" && host) {
            host.delay = Number(args[0]);
            host.repeatCount = args[1] ?? 0;
        }
        if (base === "flash.display.SimpleButton" && host)
            for (const [index, property] of ["upState", "overState", "downState", "hitTestState"].entries())
                if (index < args.length) host[property] = args[index];
        if (host && args[0] && (base === "flash.net.Socket" || base === "flash.net.XMLSocket")) host.connect(...args);
        if (host && args[0] && (base === "flash.media.Sound" || base === "flash.net.URLLoader")) host.load(...args);
    },
};
function makeBundle(scene, actions, definitions, page, address = page) {
    const bundle = { id: bundleSequence++, scene, actions, page, symbols: new Map(), initialized: new Set(),
        source: new URL(scene.workspace?.source ?? address, page) };
    for (const [id, clip] of Object.entries(scene.clips))
        if (id !== "0" && clip.type && !bundle.symbols.has(clip.type)) bundle.symbols.set(clip.type, Number(id));
    for (const [id, button] of Object.entries(scene.buttons ?? {}))
        if (button.type && !bundle.symbols.has(button.type)) bundle.symbols.set(button.type, Number(id));
    for (const [id, asset] of Object.entries(scene.assets))
        if (asset.type && !bundle.symbols.has(asset.type)) bundle.symbols.set(asset.type, Number(id));
    for (const [name, id] of Object.entries(scene.soundClasses ?? {})) bundle.symbols.set(name, id);
    bundle.classes = new ConvertedClasses(definitions, {
        ...classHooks,
        host: (chain, defs) => classHooks.host(chain, defs, bundle),
        native: (name, ...args) => bundleNative(bundle, name, args),
        nativeSymbol(name) {
            if (name === "navigateToURL")
                return (request, target) => browserMedia.navigate(request, target, value => httpAddress(value, bundle.source));
            if (name === "getDefinitionByName") return value => {
                const key = String(value).replace("::", ".");
                return bundle.classes.has(key) ? bundle.classes.definition(key) : (bundle.domain ?? mainDomain).getDefinition(key);
            };
            if (name === "ApplicationDomain") return { currentDomain: bundle.domain ?? mainDomain };
            return nativeSymbols[name];
        },
    });
    return bundle;
}
function bundleNative(bundle, name, args) {
    if (name === "Loader") return scriptLoader(bundle);
    const address = value => httpAddress(value, bundle.source);
    let value;
    if (name === "URLLoader")
        value = browserMedia.urlLoader(args[0], address,
            (request, url) => moviePage(String(request?.url ?? request), url, bundle),
            (bytes, page, url) => movieBytes.set(bytes, { page, url }));
    else if (name === "Sound") value = browserMedia.sound(args[0], address);
    else return nativeObject(name, ...args);
    displayTypes.set(value, name);
    return value;
}
const mainDomain = applicationDomain();

const loaderNode = new EventTarget();
const loaderInfo = Object.assign(eventView(loaderNode), {
    url: location.href, loaderURL: location.href, parameters: Object.fromEntries(new URLSearchParams(location.search)),
    bytesLoaded: 1, bytesTotal: 1,
});
displayViews.set(loaderNode, loaderInfo);
const stageView = Object.assign(Object.create(null), eventView(stage));
stageView.loaderInfo = loaderInfo;
let stageQuality = "high", focusRequest;
stageView.contains = view => {
    const node = displayNodes.get(view);
    return Boolean(node && stage.contains(node));
};
Object.defineProperties(stageView, {
    quality: { get: () => stageQuality, set: value => {
        const mode = String(value).toLowerCase();
        if (stageQuality !== mode) { stageQuality = mode; stage.dataset.flashQuality = mode; }
    } },
    focus: { get: () => {
        if (nextFocus) return nextFocus.view;
        const active = document.activeElement;
        return active === stage ? displayObjectFor(pointerEditor?.node) : displayObjectFor(active);
    },
        set: view => {
            const request = {};
            focusRequest = request;
            if (!view) stage.focus({ preventScroll: true });
            else if (view.focus?.() === false) requestAnimationFrame(() => {
                if (focusRequest === request) view.focus?.();
            });
        } },
    mouseX: { get: () => positionIn(stage).x },
    mouseY: { get: () => positionIn(stage).y },
    stageWidth: { get: () => data.viewBox[2] / 20 },
    stageHeight: { get: () => data.viewBox[3] / 20 },
    frameRate: { get: () => data.fps },
});
displayViews.set(stage, stageView);
displayNodes.set(stageView, stage);

function displayView(owner, clip, layer, node) {
    const state = Object.create(null), handlers = eventView(node), bound = new Map();
    let effects = [];
    const single = clip?.data.frames.length === 1 && clip.data.frames[0][0];
    const appearance = displayTransform(node, () => effects, single?.length === 1 && Boolean(single[0][9]?.length));
    const access = pointerAccess(node, Boolean(clip || layer?.field || layer?.button));
    const points = {
        localToGlobal: point => transformPoint(point, node, stage),
        globalToLocal: point => transformPoint(point, stage, node),
    };
    const view = new Proxy(state, {
        get(_, name) {
            if (typeof name !== "string") return undefined;
            if (Object.hasOwn(handlers, name)) return handlers[name];
            if (name === "stage") return stageView;
            if (name === "loaderInfo") return clip?.info ?? owner?.info ?? loaderInfo;
            if (name === "parent") return displayParents.get(node)?.() ?? (layer?.standalone ? null : owner?.view ?? null);
            if (name === "root") return root?.view ?? null;
            if (name === "name") return layer?.overrides.name ?? layer?.row[4] ?? "";
            if (name === "mask") return scriptMasks.get(node)?.view ?? null;
            if (name === "transform") return appearance;
            if (name === "filters") return effects.slice();
            if (name === "focus") return layer?.field?.focus ?? (() => node.focus({ preventScroll: true }));
            if (name === "getBounds" || name === "getRect") return target => displayBounds(node, target);
            if (name === "mouseX") return positionIn(node).x;
            if (name === "mouseY") return positionIn(node).y;
            if (Object.hasOwn(points, name)) return points[name];
            if (Object.hasOwn(access, name)) return access[name];
            if (clip) {
                if (name === "currentFrame") return clip.frame;
                if (name === "totalFrames") return clip.data.frames.length;
                if (name === "currentLabels") return clip.labels.slice();
                if (name === "currentLabel") return clip.labels.findLast(label => label.frame <= clip.frame)?.name ?? null;
                if (name === "currentFrameLabel") return clip.labels.find(label => label.frame === clip.frame)?.name ?? null;
                if (timelineMethods.has(name)) {
                    if (!bound.has(name)) bound.set(name, clip[name].bind(clip));
                    return bound.get(name);
                }
                if (name === "numChildren") return clip.childList.order.length;
                if (Object.hasOwn(clip.childList, name)) return clip.childList[name];
                const named = clip.namedLayer(name);
                if (named && state[name] == null) return named.view;
            }
            if (layer?.field && Object.hasOwn(layer.field, name) && name !== "node") return layer.field[name];
            if (layer?.button && Object.hasOwn(layer.button, name)) return layer.button[name];
            if (name === "width" || name === "height") return displayDimension(node, name);
            if (name === "x" && layer?.field) return property(layer, name) + layer.field.offsetX() * property(layer, "scaleX");
            if (layer && visualProperties.has(name)) return property(layer, name);
            return state[name];
        },
        set(_, name, value) {
            if (name === "mask") changeMask(node, value);
            else if (name === "filters") { effects = Array.isArray(value) ? value.slice() : []; appearance.update(); }
            else if (Object.hasOwn(access, name)) access[name] = value;
            else if (layer?.field && Object.hasOwn(layer.field, name) && name !== "node") layer.field[name] = value;
            else if (layer?.button && Object.hasOwn(layer.button, name)) layer.button[name] = value;
            else if (layer && (name === "width" || name === "height")) resizeDisplay(node, view, name, value);
            else if (name === "x" && layer?.field)
                (owner ?? clip).updateProperty(layer, name, Number(value) - layer.field.offsetX() * property(layer, "scaleX"));
            else if (layer && visualProperties.has(name)) (owner ?? clip).updateProperty(layer, name, value);
            else state[name] = value;
            return true;
        },
    });
    displayViews.set(node, view);
    displayNodes.set(view, node);
    displayTypes.set(view, clip ? "MovieClip" : layer.field ? "TextField" : layer.button ? "SimpleButton" : "DisplayObject");
    return { view, state };
}

)JS" R"JS(class Clip {
    constructor(id, node, ancestors = [], parent, layer, bindType = true, bundle = parent?.bundle ?? mainBundle) {
        this.id = id;
        this.node = node;
        this.ancestors = [...ancestors, id];
        this.parent = parent;
        this.parentLayer = layer;
        this.bundle = bundle;
        this.info = parent?.info;
        this.data = bundle.scene.clips[id];
        this.labels = Object.entries(this.data.labels).map(([name, frame]) => ({ name, frame }))
            .sort((left, right) => left.frame - right.frame);
        this.layers = new Map();
        this.variables = Object.create(null);
        this.frame = 0;
        this.playing = true;
        this.started = false;
        this.calls = 0;
        this.maskNodes = new Map();
        this.timelineChildren = [];
        this.animated = this.data.frames.length > 1;
        this.frameScripts = new Map();
        this.childList = displayChildren(node, () => this.view, () => this.refreshMasks());
        const { view } = displayView(parent, this, layer, node);
        this.view = view;
        if (this.data.type && bindType) {
            try {
                this.instance = bundle.classes.attach(this.data.type, view);
                this.view = this.instance.view;
                displayViews.set(node, this.view);
                displayNodes.set(this.view, node);
                displayTypes.set(this.view, "MovieClip");
            } catch (error) { scriptProblem(error); }
        }
    }

    play() { this.playing = true; this.syncStream(); }
    stop() { this.playing = false; this.syncStream(); }
    stopSounds() {
        for (const group of sounds.values()) for (const channel of group) channel.stop();
        sounds.clear();
    }
    gotoAndPlay(frame) { this.play(); this.seek(frame); }
    gotoAndStop(frame) { this.stop(); this.seek(frame); }
    nextFrame() { this.gotoAndStop(this.frame + 1); }
    prevFrame() { this.gotoAndStop(this.frame - 1); }
    addFrameScript(...bindings) {
        for (let i = 0; i + 1 < bindings.length; i += 2) {
            const frame = Number(bindings[i]) + 1, callback = bindings[i + 1];
            if (!Number.isInteger(frame) || frame < 1 || frame > this.data.frames.length) continue;
            if (callback == null) this.frameScripts.delete(frame);
            else if (typeof callback === "function") this.frameScripts.set(frame, callback);
            else throw new TypeError("Script d’image ActionScript invalide.");
        }
    }
    elapsed() { return Math.trunc(performance.now() - playbackEpoch); }

    initializeTree() {
        if (this.treeInitialized) return;
        this.treeInitialized = true;
        for (const layer of this.layers.values()) layer.child?.initializeTree();
        try { this.instance?.initialize(); }
        catch (error) { scriptProblem(error); }
    }

    start(dispatchAdded = true) {
        if (this.started) {
            if (this.detached) {
                this.detached = false;
                for (const layer of this.layers.values()) layer.child?.start();
                this.refreshMasks();
                this.syncStream(true);
            }
            return;
        }
        if (!this.frame) this.seek(1, false);
        this.started = true;
        const primed = this.bundle.primedClips;
        if (primed && this.animated && !primed.has(this.id))
            for (const url of timelineAssetUrls(this.bundle.scene, this.bundle.page, [this.id], primed, 4)) rasterImage(url);
        this.initializeTree();
        if (!this.bundle.initialized.has(this.id)) {
            this.bundle.initialized.add(this.id);
            this.run(this.data.legacyInitialize);
        }
        for (const layer of this.layers.values()) layer.child?.start();
        if (dispatchAdded) this.node.dispatchEvent(scriptEvent("addedToStage"));
        if (this.frame === 1) this.enterFrame();
        this.syncStream(true);
    }

    namedLayer(name) {
        if (!this.namedLayers) {
            let named;
            for (const layer of this.layers.values()) {
                const key = layer.overrides.name ?? layer.row[4];
                if (key && !named?.has(key)) (named ??= new Map()).set(key, layer);
            }
            this.namedLayers = named ?? unnamedLayers;
        }
        return this.namedLayers.get(name);
    }

    control(name, method, frame) {
        const child = this.namedLayer(name)?.child;
        if (child) child[method](frame);
    }

    set(name, property, value) {
        const named = this.namedLayer(name);
        if (named) named.view[property] = value;
    }

    get(name, key) { return this.namedLayer(name)?.view[key]; }

    updateProperty(layer, name, value) {
        if (coordinates.has(name)) {
            const number = Number(value);
            if (!Number.isFinite(number * 20)) return;
            value = number;
        }
        if (Object.is(layer.overrides[name], value)) return;
        layer.overrides[name] = value;
        if (name === "name") { this.namedLayers = null; return; }
        this.update(layer, name !== "alpha" && coordinates.has(name));
    }

    releaseStream() {
        this.stream?.audio.pause();
        this.stream = null;
        activeStreams.delete(this);
    }

    syncStream(force = false) {
        if (!this.data.streams.length || !this.started) return;
        const track = this.data.streams.find(([first, last]) => this.frame >= first && this.frame <= last);
        if (!track) { this.releaseStream(); return; }
        if (this.stream?.track !== track) {
            this.releaseStream();
            const audio = new Audio(new URL(track[2], this.bundle.page));
            audio.volume = browserMedia.masterVolume;
            this.stream = { track, audio };
            activeStreams.add(this);
            audio.addEventListener("loadedmetadata", () => {
                if (this.stream?.audio === audio) this.syncStream(true);
            }, { once: true });
            force = true;
        }
        const audio = this.stream.audio;
        if (!running || !this.playing) { audio.pause(); return; }
        const [, , , offset] = track;
        const time = Math.max(0, (this.frame - track[0]) / this.bundle.scene.fps + offset);
        if (audio.readyState && (force || Math.abs(audio.currentTime - time) > 0.2))
            audio.currentTime = time;
        if (audio.paused && !audio.ended) audio.play().catch(() => {});
    }

    run(action, ...parameters) {
        const actions = this.bundle.actions;
        if (action === null || !actions[action] || this.calls >= 32) return;
        try { ++this.calls; return actions[action](this, ...parameters); }
        catch (error) { scriptProblem(error); }
        finally { --this.calls; }
    }

    makeLayer(row, ancestry = this.ancestors) {
        const data = this.bundle.scene;
        const id = row[1];
        const node = element("g");
        const layer = { row, node, child: null, view: null, overrides: Object.create(null) };
        if (ancestry.length <= 24 && !ancestry.includes(id)) {
            if (data.clips[id]) {
                layer.child = new Clip(id, node, ancestry, this, layer);
                layer.view = layer.child.view;
                layer.child.seek(1, false);
            } else if (data.buttons[id]) {
                layer.button = makeButton(data.buttons[id], node, this, [...ancestry, id]);
            } else {
                const definition = data.assets[id];
                if (definition?.text === undefined) {
                    layer.assetId = timelineAsset(row, data);
                    layer.asset = asset(layer.assetId, this.bundle);
                    node.setAttribute("data-flash-static", "");
                    node.append(layer.asset);
                }
                else {
                    layer.field = createTextField(definition, (type, detail) => node.dispatchEvent(scriptEvent(type, true, false, detail)),
                        data.fonts?.[definition.fontId] ?? data.fontClasses?.[definition.fontClass]);
                    node.append(layer.field.node);
                }
            }
        }
        if (!layer.view) layer.view = displayView(this, null, layer, node).view;
        this.update(layer);
        return layer;
    }

    update(layer, geometryOnly = false) {
        const { row, node, overrides } = layer;
        let [a, b, c, d, x, y] = row[2];
        if (overrides.x !== undefined) x = Number(overrides.x) * 20;
        if (overrides.y !== undefined) y = Number(overrides.y) * 20;
        if (overrides.scaleX !== undefined) {
            const ratio = overrides.scaleX / (Math.hypot(a, b) || 1);
            a *= ratio; b *= ratio;
        }
        if (overrides.scaleY !== undefined) {
            const ratio = overrides.scaleY / (Math.hypot(c, d) || 1);
            c *= ratio; d *= ratio;
        }
        if (overrides.rotation !== undefined) {
            const rotation = overrides.rotation * Math.PI / 180 - Math.atan2(b, a);
            const cos = Math.cos(rotation), sin = Math.sin(rotation);
            [a, b, c, d] = [a*cos-b*sin, a*sin+b*cos, c*cos-d*sin, c*sin+d*cos];
        }
        const put = (name, value) => { if (node.getAttribute(name) !== String(value)) node.setAttribute(name, value); };
        if ([a, b, c, d, x, y].every(Number.isFinite))
            put("transform", "matrix(" + [a,b,c,d,x,y].join(" ") + ")");
        if (geometryOnly) return;
        if (layer.asset && row[11] !== layer.ratio) {
            layer.ratio = row[11];
            const id = timelineAsset(row, this.bundle.scene);
            if (id !== layer.assetId) {
                const replacement = asset(id, this.bundle);
                layer.asset.replaceWith(replacement);
                layer.asset = replacement;
                layer.assetId = id;
            }
        }
        const color = row[8], effects = row[9];
        const needsColor = color && (color.slice(0, 3).some(value => value !== 256) || color.slice(4).some(Boolean));
        const transform = !needsColor ? null : overrides.alpha === undefined ? color :
            [...color.slice(0, 3), Math.round(overrides.alpha * 256), ...color.slice(4, 7), 0];
        if (transform) authoredTints.set(node, transform);
        else authoredTints.delete(node);
        if (transform || effects?.length) put("filter", svgFilter(transform, effects ?? []));
        else if (node.hasAttribute("filter")) node.removeAttribute("filter");
        layer.opacity = transform ? 1 : overrides.alpha ?? row[3];
        put("opacity", layer.opacity);
        this.updateLayerBlend(layer);
        if (overrides.buttonMode !== undefined) node.style.cursor = overrides.buttonMode ? "pointer" : "";
        put("display", (overrides.visible ?? row[6]) ? "inline" : "none");
    }

    updateLayerBlend(layer) {
        const { row, node, overrides } = layer;
        const authored = overrides.blendMode ?? row[10];
        let blend = blendModes[authored] ?? authored;
        // SVG opacity isolates a group. A single blended child must see the backdrop outside that group.
        const child = (!blend || blend === "normal") && layer.opacity < 1 && layer.child?.layers.size === 1 &&
            node.childElementCount === 1 ? node.firstElementChild : null;
        const hoisted = child?.style.mixBlendMode ? child : null;
        if (layer.hoistedBlend !== hoisted) {
            layer.hoistedBlend?.removeAttribute("data-flash-blend-hoisted");
            hoisted?.setAttribute("data-flash-blend-hoisted", "");
            layer.hoistedBlend = hoisted;
        }
        if (hoisted) blend = hoisted.style.mixBlendMode;
        if (blend || node.style.mixBlendMode || node.style.isolation) displayBlend(node, blend);
    }

)JS" R"JS(    refreshMasks() {
        const items = new Map(), masksByLayer = new Map();
        const layers = [...this.layers].sort((a, b) => a[0] - b[0]);
        this.childList.synchronize(layers.map(([, layer]) => layer.view));
        const visibleLayers = new Set(this.childList.order);
        this.timelineChildren = [];
        let active = [];
        for (const [depth, layer] of layers) {
            if (!visibleLayers.has(layer.view)) continue;
            if (layer.child?.animated) this.timelineChildren.push(layer.child);
            if (active.length) active = active.filter(mask => mask.end >= depth);
            if (layer.row[5] > depth) {
                let mask = this.maskNodes.get(layer.node);
                if (!mask) {
                    mask = element("mask", { id: "mask-" + (++masks), "mask-type": "alpha" });
                    symbols.append(mask);
                }
                layer.maskKey = undefined;
                if (layer.node.parentNode !== mask) mask.append(layer.node);
                masksByLayer.set(layer.node, mask);
                active.push({ id: mask.id, end: layer.row[5] });
                items.set(layer.view, null);
            } else {
                const key = active.map(mask => mask.id).join(" ");
                if (layer.maskKey !== key) {
                    layer.maskKey = key;
                    let node = layer.node;
                    for (const mask of active) {
                        const wrapper = element("g", { mask: "url(#" + mask.id + ")" });
                        wrapper.append(node);
                        node = wrapper;
                    }
                    layer.rendered = node;
                }
                items.set(layer.view, layer.rendered);
            }
        }
        for (const [node, mask] of this.maskNodes) if (!masksByLayer.has(node)) mask.remove();
        this.maskNodes = masksByLayer;
        this.animated = this.data.frames.length > 1 || this.timelineChildren.length > 0;
        const ordered = this.childList.order.map(view => items.has(view) ? items.get(view) : displayNodes.get(view));
        reconcileChildren(this.node, ordered.filter(Boolean));
    }

    detach() {
        this.detached = true;
        this.releaseStream();
        for (const layer of this.layers.values()) layer.child?.detach();
        for (const mask of this.maskNodes.values()) mask.remove();
        this.maskNodes.clear();
    }

    seek(requested, dispatch = true) {
        const frame = typeof requested === "string" && Object.hasOwn(this.data.labels, requested)
            ? this.data.labels[requested] : Math.trunc(Number(requested));
        if (!Number.isFinite(frame) || this.calls >= 32) return;
        const target = Math.max(1, Math.min(frame, this.data.frames.length));
        if (target === this.frame) return;
        const rebuild = target !== this.frame + 1;
        let [updates, removals, color] = this.data.frames[target - 1];
        if (rebuild) {
            const snapshot = timelineSnapshot(this.data, target);
            const placements = snapshot.placements;
            color = snapshot.color;
            removals = [...this.layers.keys()].filter(depth => !placements.has(depth));
            updates = placements.values();
        }
        if (this === root && color) background.setAttribute("fill", color);
        let changed = false;
        let addedClips;
        for (const depth of removals) {
            this.layers.get(depth)?.child?.detach();
            changed = this.layers.delete(depth) || changed;
        }
        if (changed) this.namedLayers = null;
        for (const row of updates) {
            const old = this.layers.get(row[0]);
            if (old?.row === row) continue;
            if (old?.row[1] === row[1] && old.row[7] === row[7]) {
                if (old.row[5] !== row[5]) changed = true;
                if (old.row[4] !== row[4]) this.namedLayers = null;
                old.row = row;
                this.update(old);
            } else {
                old?.child?.detach();
                const layer = this.makeLayer(row);
                this.layers.set(row[0], layer);
                this.namedLayers = null;
                if (this.started && layer.child) (addedClips ??= []).push(layer.child);
                changed = true;
            }
        }
        this.frame = target;
        if (changed) this.refreshMasks();
        if (this.parent && this.parentLayer.opacity < 1) this.parent.updateLayerBlend(this.parentLayer);
        if (addedClips) {
            for (const clip of addedClips) clip.initializeTree();
            for (const clip of addedClips) if (clip.node.isConnected) clip.start();
        }
        if (dispatch) this.enterFrame();
        this.syncStream(rebuild);
    }

    enterFrame() {
        const [, , , action, events] = this.data.frames[this.frame - 1];
        for (const sound of events) playSound(sound, this.bundle);
        this.run(action);
        try { this.frameScripts.get(this.frame)?.(); }
        catch (error) { scriptProblem(error); }
    }

    tick() {
        const children = this.timelineChildren;
        if (this.playing && this.data.frames.length > 1)
            this.seek(this.frame % this.data.frames.length + 1);
        for (const child of children) if (child.node.isConnected) child.tick();
    }
}

)JS" R"JS(const timelineSnapshots = new WeakMap();
function timelineSnapshot(timeline, frame) {
    let cache = timelineSnapshots.get(timeline);
    const existing = cache?.get(frame);
    if (existing) return existing;
    const placements = new Map();
    let color = "#fff";
    for (let index = 0; index < frame; ++index) {
        const [rows, removed, fill] = timeline.frames[index];
        for (const depth of removed) placements.delete(depth);
        for (const row of rows) placements.set(row[0], row);
        if (fill) color = fill;
    }
    const snapshot = { placements, color };
    // Share immutable placement rows across instances of the same symbol.
    if (placements.size <= 256) {
        if (!cache) timelineSnapshots.set(timeline, cache = new Map());
        if (cache.size >= 4) cache.delete(cache.keys().next().value);
        cache.set(frame, snapshot);
    }
    return snapshot;
}

function makeLinkedClip(id, bundle) {
    const node = element("g");
    const layer = { row: [0, id, [1, 0, 0, 1, 0, 0], 1, "", 0, true], node, overrides: Object.create(null) };
    const clip = new Clip(id, node, [], null, layer, false, bundle);
    clip.seek(1, false);
    clip.view.bindView = view => {
        clip.view = view;
        displayViews.set(node, view);
        displayNodes.set(view, node);
        displayTypes.set(view, "MovieClip");
    };
    authoredClips.set(node, clip);
    node.addEventListener("addedToStage", () => clip.start(false));
    node.addEventListener("removedFromStage", () => clip.detach());
    return clip.view;
}

function makeLinkedButton(id, bundle) {
    const owner = bundle.buttonOwner ??= new Clip(0, element("g"), [], null, null, false, bundle);
    const layer = owner.makeLayer([0, id, [1, 0, 0, 1, 0, 0], 1, "", 0, true], []);
    layer.standalone = true;
    layer.view.bindView = view => {
        layer.view = view;
        displayViews.set(layer.node, view);
        displayNodes.set(view, layer.node);
        displayTypes.set(view, "SimpleButton");
    };
    return layer.view;
}

const frameEvent = scriptEvent("enterFrame");
const frameDispatch = [];

function animate(time) {
    requestAnimationFrame(animate);
    if (running) {
        accumulated += Math.min(time - previous, 100);
        const interval = 1000 / data.fps;
        while (accumulated >= interval) {
            for (const reference of frameListeners) frameDispatch.push(reference);
            for (const reference of frameDispatch) {
                const node = reference.deref();
                if (node) node.dispatchEvent(frameEvent);
                else frameListeners.delete(reference);
            }
            frameDispatch.length = 0;
            root.tick();
            for (const node of scriptedNodes) {
                if (!node.isConnected) { scriptedNodes.delete(node); continue; }
                const clip = authoredClips.get(node);
                if (clip?.animated) {
                    const movieInterval = 1000 / clip.bundle.scene.fps;
                    clip.time = (clip.time ?? 0) + interval;
                    while (clip.time + 0.01 >= movieInterval) {
                        clip.tick();
                        clip.time -= movieInterval;
                    }
                }
            }
            accumulated -= interval;
        }
    }
    previous = time;
    for (const draw of pendingBitmapDraws) draw();
    pendingBitmapDraws.clear();
    if (activeScriptMasks.size) updateScriptMasks();
}

try {
    const response = await fetch(new URL("scene.json", documentUrl));
    if (!response.ok) throw new Error("scene.json: " + response.status);
    data = await response.json();
    const [network, index] = await Promise.all([
        fetch(new URL(data.workspace?.network ?? "network.json", documentUrl)).catch(() => null),
        data.workspace && fetch(new URL(data.workspace.catalog, documentUrl)),
    ]);
    if (index?.ok) {
        catalog = Object.create(null);
        for (const [path, page] of Object.entries((await index.json()).movies)) {
            catalog[path.toLowerCase()] ??= page;
            catalog[path] = page;
        }
    }
    mainBundle = makeBundle(data, actions, definitions, documentUrl);
    const bitmapSources = Promise.all([preloadBitmapSources(data, definitions, documentUrl), preloadLinkedPixels(data, documentUrl), loadFonts(data, documentUrl)]);
    mainDomain.install(mainBundle.classes);
    loaderInfo.url = mainBundle.source.href;
    loaderInfo.loaderURL = loaderInfo.url;
    if (data.workspace) {
        const catalogueLink = document.createElement("a");
        catalogueLink.href = "/catalogue.html";
        catalogueLink.textContent = "Catalogue des animations";
        button.parentElement.append(catalogueLink);
    }
    if (network?.ok) {
        const config = await network.json();
        sockets = Object.assign(Object.create(null), Object.fromEntries(
            Object.entries(config.sockets ?? {}).map(([key, endpoint]) => [key.trim().toLowerCase(), endpoint])));
        httpRoutes = config.http ?? httpRoutes;
        movieRoutes = config.movieRoutes ?? movieRoutes;
        lazyAssetPaths = Array.isArray(config.lazyAssetPaths) ?
            config.lazyAssetPaths.filter(path => typeof path === "string" && path.startsWith("/") && path.endsWith("/")) : [];
        loaderInfo.parameters = Object.assign(Object.create(null), config.parameters, loaderInfo.parameters);
    }
    if (data.hasSource) {
        const source = await fetch(new URL("source.bin", documentUrl));
        if (!source.ok) throw new Error("source.bin: " + source.status);
        const bytes = new Uint8Array(await source.arrayBuffer());
        loaderInfo.bytes = new ByteArray(bytes, true);
        loaderInfo.bytesLoaded = loaderInfo.bytesTotal = bytes.length;
    }
    const linked = document.querySelector("#linked");
    for (const [name, url] of Object.entries(data.dependencies)) {
        const item = document.createElement("li"), link = document.createElement("a");
        link.textContent = name;
        link.href = url;
        item.append(link);
        linked.lastElementChild.append(item);
        linked.hidden = false;
    }
    stage.setAttribute("viewBox", data.viewBox.join(" "));
    document.documentElement.style.setProperty("--scene-width", data.viewBox[2] / 20 + "px");
    const [x, y, width, height] = data.viewBox;
    for (const [key, value] of Object.entries({ x, y, width, height, fill: "#fff" })) background.setAttribute(key, value);
    status.textContent = "";
    await bitmapSources;
    root = new Clip(0, art);
    root.seek(1, false);
    stage.tabIndex = -1;
    stage.addEventListener("pointerdown", event => {
        if (!event.isPrimary || event.button !== 0) return;
        const editor = event.target.closest("input, textarea, select, button, a, [contenteditable=true], [contenteditable=plaintext-only]");
        pointerEditor = editor?.matches("input, textarea, [contenteditable]") ? { node: editor, id: event.pointerId } : null;
        if (pointerEditor) editor.focus({ preventScroll: true });
        else if (!editor) stage.focus({ preventScroll: true });
    }, true);
    for (const type of ["pointerup", "pointercancel"]) document.addEventListener(type, event => {
        if (event.pointerId !== pointerEditor?.id) return;
        const { node } = pointerEditor;
        pointerEditor = null;
        // Authored popup managers can remove and reinsert a window when it is raised.
        if (type === "pointerup" && node.isConnected && node !== document.activeElement)
            node.focus({ preventScroll: true });
    }, true);
    for (const type of ["pointerup", "keydown"])
        stage.addEventListener(type, () => browserMedia.resumeAudio(), { capture: true, passive: true });
    button.addEventListener("click", () => {
        browserMedia.resumeAudio();
        running = !running;
        if (!entered) {
            entered = true;
            root.start();
            loaderNode.dispatchEvent(scriptEvent("complete"));
        }
        for (const clip of activeStreams) clip.syncStream();
        button.textContent = running ? "Pause" : "Lire";
        if (running) stage.focus({ preventScroll: true });
    });
    button.disabled = false;
    if (!Object.values(data.clips).some(clip => clip.streams.length || clip.frames.some(frame => frame[4].length))) button.click();
    requestAnimationFrame(animate);
} catch (error) {
    console.error(error);
    status.textContent = location.protocol === "file:" ?
        "Ouvre ce projet avec un serveur HTTP local. Consulte LISEZ-MOI.txt." : "Impossible de charger la scène convertie.";
}
)JS";

}
}

namespace flash
{
namespace
{
struct Packages
{
    std::filesystem::path root, destination;
    std::map<std::filesystem::path, std::filesystem::path> converted;
    unsigned sequence{};
    std::size_t expanded{};
    std::mutex* visuals{};
};

struct WorkspaceMovie
{
    std::filesystem::path path;
    std::string url;
    std::string package;
    std::shared_ptr<const Movie> edited;
};

std::string method_value(const web::WebMethod& method)
{
    if (method.function.empty() && method.diagnostic.empty())
        return "null";
    return std::format("{{ hidden: {}, error: {}, call: {} }}", method.private_access,
        method.diagnostic.empty() ? "null" : web::json_string(method.diagnostic),
        method.function.empty() ? "null" : method.function);
}

template<class Entries, class Format>
std::string class_entries(const Entries& entries, Format format)
{
    std::string output = "{";
    for (const auto& [key, member] : entries)
    {
        if (output.size() > 1)
            output += ',';
        output += "\n    [" + web::json_string(key) + "]: " + format(member);
    }
    return output + (entries.empty() ? "}" : "\n}");
}

std::string class_modules(const web::WebProgram& program)
{
    const auto slot_value = [](const web::WebSlot& slot)
    {
        return std::format("{{ hidden: {}, type: {}, value: {} }}", slot.private_access, web::json_string(slot.type),
            slot.value.empty() ? "null" : slot.value);
    };
    return "export const definitions = " + class_entries(program.classes, [&](const web::WebClass& type)
    {
        std::string interfaces = "[";
        for (const auto& name : type.interfaces)
        {
            if (interfaces.size() > 1)
                interfaces += ", ";
            interfaces += web::json_string(name);
        }
        return std::format("{{\n    base: {},\n    sealed: {},\n    interfaces: {}],\n    construct: {},\n    initialize: {},\n"
            "    slots: {},\n    staticSlots: {},\n    methods: {},\n    statics: {}\n}}", web::json_string(type.base), type.sealed, interfaces,
            method_value(type.constructor), method_value(type.initialize),
            class_entries(type.slots, slot_value), class_entries(type.static_slots, slot_value),
            class_entries(type.methods, method_value), class_entries(type.statics, method_value));
    }) + ";\n";
}

std::string utf8_path(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string shared_visual(const Asset& asset, const std::filesystem::path& root,
    const std::filesystem::path& package, std::mutex& mutex, ExportReport& report)
{
    std::array<UCHAR, 32> digest{};
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
        const_cast<PUCHAR>(asset.bytes.data()), static_cast<ULONG>(asset.bytes.size()),
        digest.data(), static_cast<ULONG>(digest.size()))))
        throw std::runtime_error("Impossible de calculer l’empreinte d’une ressource HTML5.");
    constexpr std::wstring_view hex = L"0123456789abcdef";
    std::wstring filename;
    filename.reserve(64 + asset.extension.size());
    for (const auto value : digest)
    {
        filename += hex[value >> 4];
        filename += hex[value & 15];
    }
    filename += asset.extension;
    const auto path = root / "assets" / filename;
    {
        const std::scoped_lock lock(mutex);
        if (!std::filesystem::exists(path))
        {
            write_file(path, asset.bytes);
            ++report.files;
        }
    }
    return utf8_path(path.lexically_relative(package));
}

bool inside(const std::filesystem::path& root, const std::filesystem::path& path)
{
    const auto relative = path.lexically_relative(root);
    return !root.empty() && !relative.empty() && !relative.has_root_path() && *relative.begin() != "..";
}

std::filesystem::path normalized_path(const std::filesystem::path& path)
{
    std::error_code error;
    const auto canonical = std::filesystem::canonical(path, error);
    return error ? std::filesystem::absolute(path).lexically_normal() : canonical;
}

std::filesystem::path resolve_swf(const std::filesystem::path& root, const std::filesystem::path& source, std::string_view value)
{
    if (root.empty() || source.empty() || value.find(':') != std::string_view::npos)
        return {};
    auto address = std::string(value.substr(0, value.find_first_of("?#")));
    std::ranges::replace(address, '\\', '/');
    if (address.starts_with("//") || address.find('\0') != std::string::npos)
        return {};
    const auto path = std::filesystem::path(std::u8string(address.begin(), address.end()));
    const auto candidate = (path.has_root_directory() ? root / path.relative_path() : source.parent_path() / path).lexically_normal();
    if (!inside(root, candidate))
        return {};
    std::error_code error;
    const auto target = std::filesystem::canonical(candidate, error);
    if (!error)
        return inside(root, target) && std::filesystem::is_regular_file(target, error) ? target : std::filesystem::path{};

    // Attribute checks can still succeed when the filesystem denies opening a directory to resolve its full name.
    auto checked = root;
    for (const auto& component : candidate.lexically_relative(root))
    {
        checked /= component;
        const auto attributes = GetFileAttributesW(checked.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return {};
    }
    return std::filesystem::is_regular_file(checked, error) ? checked : std::filesystem::path{};
}

ExportReport write_package(const Movie& movie, const std::filesystem::path& source, const std::filesystem::path& relative,
    std::string_view title, Packages& packages, std::string_view workspace_url = {})
{
    const auto destination = packages.destination / relative;
    std::filesystem::create_directories(destination);
    ExportReport report;
    std::set<std::string> references;
    const auto scripts = web::movie_scripts(movie, destination / "sources", report, workspace_url.empty() ? &references : nullptr);
    std::string links = "{";
    for (const auto& reference : references)
    {
        try
        {
            const auto path = resolve_swf(packages.root, source, reference);
            if (path.empty())
            {
                web::omission(report, std::format("Dépendance SWF non résolue : {}", reference));
                continue;
            }
            auto found = packages.converted.find(path);
            if (found == packages.converted.end())
            {
                if (packages.sequence >= 32)
                {
                    web::omission(report, std::format("Dépendance hors limite d’export : {}", reference));
                    continue;
                }
                const auto target = std::filesystem::path("dependencies") / std::format("film-{}", ++packages.sequence);
                found = packages.converted.emplace(path, target).first;
                try
                {
                    const auto child = Movie::open(path);
                    if (child.header().expanded_size > 2 * max_file_size - packages.expanded)
                        throw std::runtime_error("Budget dépassé pour l’ensemble des SWF liés.");
                    packages.expanded += child.header().expanded_size;
                    const auto converted = write_package(child, path, target, utf8_path(path.stem()), packages);
                    report.files += converted.files;
                    if (converted.failed)
                        web::omission(report, std::format("Dépendance {} : {} points de portage dans {}/LISEZ-MOI.txt.",
                            reference, converted.failed, utf8_path(target.lexically_relative(relative))));
                }
                catch (...)
                {
                    packages.converted.erase(found);
                    throw;
                }
            }
            if (links.size() > 1)
                links += ',';
            links += web::json_string(reference) + ':' +
                web::json_string(utf8_path((found->second / "index.html").lexically_relative(relative)));
        }
        catch (const std::exception& error) { web::omission(report, std::format("Dépendance {} : {}", reference, error.what())); }
    }
    std::vector<std::string> actions;
    std::function<std::string(const Asset&)> share_visual;
    if (packages.visuals)
        share_visual = [&](const Asset& asset) { return shared_visual(asset, packages.destination, destination, *packages.visuals, report); };
    auto scene = web::export_timeline(movie, destination, report, actions, scripts.clips, share_visual);
    scene.pop_back();
    scene += ",\"hasSource\":true";
    scene += ",\"dependencies\":" + links + "}";
    std::string audio_classes = "{", font_classes = "{", fonts = "{";
    std::map<unsigned, std::string> font_assets;
    for (std::size_t index = 0; index < movie.tags().size(); ++index)
    {
        const auto& tag = movie.tags()[index];
        if (!tag.character_id || (tag.code != 48 && tag.code != 75)) continue;
        const auto id = *tag.character_id;
        try
        {
            const auto face = web::web_font(movie, index);
            if (!face) continue;
            const bool has_glyphs = !face->file.bytes.empty();
            auto metadata = std::format("{{\"fontName\":{},\"fontStyle\":{},\"fontType\":{}",
                web::json_string(face->name), web::json_string(face->style),
                web::json_string(has_glyphs ? "embedded" : "device"));
            if (has_glyphs)
            {
                std::string path;
                if (share_visual) path = share_visual(face->file);
                else
                {
                    path = std::format("assets/font-{}.ttf", id);
                    write_file(destination / path, face->file.bytes);
                    ++report.files;
                }
                metadata += ",\"url\":" + web::json_string(path);
            }
            metadata += '}';
            font_assets.emplace(id, metadata);
            if (fonts.size() > 1) fonts += ',';
            fonts += web::json_string(std::to_string(id)) + ':' + metadata;
        }
        catch (const std::runtime_error& error)
        {
            web::omission(report, std::format("Police {} : {}", id, error.what()));
        }
    }
    for (const auto& [id, script] : scripts.clips)
    {
        const auto index = movie.character(id);
        if (!index || script.type.empty())
            continue;
        const auto kind = movie.tags()[*index].code;
        if (kind == 14)
        {
            if (audio_classes.size() > 1)
                audio_classes += ',';
            audio_classes += web::json_string(script.type) + ':' + std::to_string(id);
        }
        else if (kind == 48 || kind == 75 || kind == 91)
            try
            {
                if (const auto face = font_assets.find(id); face != font_assets.end())
                {
                    if (font_classes.size() > 1) font_classes += ',';
                    font_classes += web::json_string(script.type) + ':' + face->second;
                    continue;
                }
                Reader font(movie.data(*index));
                font.skip(2);
                const auto flags = font.u8();
                std::string name;
                if (kind == 91)
                    name = font.cstring();
                else
                {
                    font.skip(1);
                    const auto raw = font.take(font.u8());
                    const std::string_view label(reinterpret_cast<const char*>(raw.data()), raw.size());
                    name = label.substr(0, label.find('\0'));
                }
                if (!name.empty())
                {
                    const auto style = flags & 1 ? (flags & 2 ? "boldItalic" : "bold") : (flags & 2 ? "italic" : "regular");
                    if (font_classes.size() > 1)
                        font_classes += ',';
                    font_classes += web::json_string(script.type) + ":{\"fontName\":" + web::json_string(name) +
                        ",\"fontStyle\":" + web::json_string(style) + ",\"fontType\":" +
                        web::json_string(kind == 91 ? "embeddedCFF" : "embedded") + '}';
                }
            }
            catch (const std::runtime_error& error)
            {
                web::omission(report, std::format("Police {} : {}", id, error.what()));
            }
    }
    scene += ",\"soundClasses\":" + audio_classes + '}';
    scene += ",\"fontClasses\":" + font_classes + '}';
    scene += ",\"fonts\":" + fonts + '}';
    if (!workspace_url.empty())
        scene += ",\"workspace\":{\"catalog\":\"../../catalog.json\",\"network\":\"../../network.json\",\"source\":" +
            web::json_string(workspace_url) + "}";
    scene += "}";
    write_text(destination / "scene.json", scene);
    std::string script = "export const actions = [\n";
    for (const auto& action : actions)
    {
        for (std::size_t start = 0; start < action.size();)
        {
            const auto end = action.find('\n', start);
            const auto outer = start == 0 || end == std::string::npos;
            script += std::string(outer ? "    " : "        ") +
                action.substr(start, end == std::string::npos ? end : end - start) +
                (end == std::string::npos ? ",\n" : "\n");
            start = end == std::string::npos ? action.size() : end + 1;
        }
    }
    script += "];\n";
    write_text(destination / "actions.js", script);
    write_text(destination / "classes.js", class_modules(scripts));
    if (workspace_url.empty())
    {
        write_text(destination / "as3.js", web::class_runtime);
        write_text(destination / "text.js", web::text_runtime);
        write_text(destination / "media.js", web::media_runtime);
        write_text(destination / "network.json", "{\n  \"http\": {},\n  \"sockets\": {},\n  \"tcp\": {}\n}\n");
        write_text(destination / "stage.js", web::stage_script);
        write_text(destination / "stage.css", web::css);
        write_text(destination / "serve.py", web::local_server);
        report.files += 7;
    }
    write_file(destination / "source.bin", movie.uncompressed());
    ++report.files;

    std::string notes = "Projet HTML5 Flash Studio\n\n";
    notes += workspace_url.empty() ?
        "Démarrage local : py -3 serve.py, puis ouvrir http://127.0.0.1:8000/ dans le navigateur.\n"
        "Hébergement : publier le dossier sur un serveur statique.\n\n" :
        "Le serveur local et le catalogue se trouvent à la racine de l’export.\n\n";
    notes +=
        "scene.json : placement des objets, images clés, labels, sprites, boutons et sons.\n"
        "assets/ : ressources converties ou extraites.\n"
        "classes.js : classes ActionScript 3 converties en JavaScript, avec les diagnostics des méthodes à porter.\n"
        "actions.js : fonctions JavaScript issues des scripts ActionScript 1 et 2 convertis.\n"
        "sources/ : ActionScript récupéré pour poursuivre le portage.\n";
    notes += workspace_url.empty() ?
        "dependencies/ : projets web des SWF liés détectés dans l’espace de travail.\n"
        "network.json : routes HTTP, WebSocket et TCP. Les routes TCP passent par serve.py.\n" :
        "../../catalog.json : chemins SWF et pages converties de l’espace de travail.\n"
        "../../network.json : routes HTTP, WebSocket et TCP. Les routes TCP passent par serve.py.\n";
    notes += "\n"
        "La scène utilise SVG et JavaScript dans le navigateur. Les appels externes, les effets et les scripts sans équivalent"
        " dans ce convertisseur demandent un portage.\n";
    if (scripts.as3_bodies)
        notes += std::format("\nAS3 : {} corps compilés sur {}. La compilation ne vérifie pas la disponibilité des services utilisés à l’exécution.\n",
            scripts.compiled_bodies, scripts.as3_bodies);
    notes += std::format("\n{} points de portage signalés.\n", report.failed);
    for (const auto& error : report.errors)
        notes += "- " + error + '\n';
    if (report.failed > report.errors.size())
        notes += std::format("- {} autres points.\n", report.failed - report.errors.size());
    write_text(destination / "LISEZ-MOI.txt", notes);
    const auto prefix = workspace_url.empty() ? "" : "../../";
    write_text(destination / "index.html", std::format(web::page, web::html_text(title),
        std::string(prefix) + "stage.css", std::string(prefix) + "stage.js"));
    report.files += 5;
    return report;
}

std::string ascii_lower(std::string text)
{
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    return text;
}

std::string read_small_text(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 1024 * 1024)
        return {};
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

std::string web_path(const std::filesystem::path& path)
{
    return "/" + utf8_path(path);
}

std::string url_path(std::string_view path)
{
    std::string escaped;
    for (const unsigned char c : path)
    {
        if (c <= 32 || c >= 127 || c == '#' || c == '?' || c == '%' || c == '"' || c == '<' || c == '>')
            escaped += std::format("%{:02X}", c);
        else
            escaped += static_cast<char>(c);
    }
    return escaped;
}

void find_flash_routes(const std::filesystem::path& project, std::map<std::string, std::filesystem::path>& movies)
{
    const auto app = read_small_text(project / "app.js");
    const std::regex route(R"rx((?:^|\n)[ \t]*app\.get\(\s*["'](/[^"':*\r\n]+\.swf)["'][\s\S]{0,400}?res\.sendFile\(\s*path\.join\(\s*["']((?:\\[\\/]|[^"'\\\r\n])+?\.swf)["']\s*\))rx", std::regex::icase);
    for (auto it = std::sregex_iterator(app.begin(), app.end(), route); it != std::sregex_iterator(); ++it)
    {
        auto path = (*it)[2].str();
        std::size_t pos{};
        while ((pos = path.find("\\\\", pos)) != std::string::npos)
            path.erase(pos++, 1);
        const auto source = std::filesystem::path(std::u8string(path.begin(), path.end()));
        std::error_code error;
        if (source.is_absolute() && std::filesystem::is_regular_file(source, error))
            movies.try_emplace((*it)[1].str(), normalized_path(source));
    }
}

bool public_asset(std::string_view extension)
{
    static const std::set<std::string_view> extensions{
        ".apng", ".avif", ".bmp", ".css", ".csv", ".flac", ".gif", ".ico", ".jpeg", ".jpg", ".json",
        ".m4a", ".mp3", ".mp4", ".ogg", ".otf", ".png", ".svg", ".ttf", ".txt", ".wav", ".webm", ".webp", ".woff",
        ".woff2", ".xml"
    };
    return extensions.contains(extension);
}

void scan_web_folder(const std::filesystem::path& folder, const std::filesystem::path& output, bool public_files,
    std::map<std::string, std::filesystem::path>& movies, std::string& warnings, const std::filesystem::path& skip = {})
{
    std::error_code error;
    auto walker = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, error);
    for (; walker != std::filesystem::recursive_directory_iterator(); walker.increment(error))
    {
        const auto& item = *walker;
        const auto path = item.path();
        const auto name = ascii_lower(utf8_path(path.filename()));
        if (item.is_directory(error))
        {
            std::error_code marker_error;
            if (path == output || path == skip || name == "node_modules" || name == "ruffle" ||
                name == "__flashstudio" || name.starts_with('.') ||
                std::filesystem::is_regular_file(path / "__flashstudio" / "catalog.json", marker_error))
                walker.disable_recursion_pending();
            continue;
        }
        if (item.is_symlink(error) || !item.is_regular_file(error))
            continue;
        const auto extension = ascii_lower(utf8_path(path.extension()));
        const auto relative = path.lexically_relative(folder);
        if (extension == ".swf")
            movies.try_emplace(web_path(relative), path);
        else if (public_files && public_asset(extension))
        {
            const auto destination = output / relative;
            std::filesystem::create_directories(destination.parent_path(), error);
            if (!error)
                std::filesystem::copy_file(path, destination, std::filesystem::copy_options::skip_existing, error);
            if (error)
                warnings += "- Copie impossible pour " + utf8_path(relative) + "\n";
        }
    }
    if (error)
        warnings += "- Parcours incomplet de " + utf8_path(folder.filename()) + "\n";
}

std::string workspace_network(const std::filesystem::path& project, const std::filesystem::path& public_root)
{
    std::string sockets;
    std::string backend = "null";
    std::smatch match;
    const auto app = read_small_text(project / "app.js");
    const auto config = read_small_text(project / "config.json");
    const std::regex web_port(R"rx("webServerPort"\s*:\s*([0-9]{1,5})\b)rx");
    if (app.find("config.webServerPort") != std::string::npos && std::regex_search(config, match, web_port))
    {
        const auto port = std::stoi(match[1].str());
        if (port > 0 && port <= 65535)
            backend = std::to_string(port);
    }
    const auto service = read_small_text(project / "assets" / "bbl" / "GlobalProperties.js");
    if (std::regex_search(service, std::regex(R"(\bUSE_WEBSOCKET\s*:\s*true\b)")))
    {
        const std::regex socket_config(R"rx(<socket\s+port="([0-9]{1,5})"\s+host="(localhost|127\.0\.0\.1)"\s*/?>)rx");
        std::map<std::string, std::string> endpoints;
        for (const auto* filename : {"params.xml", "chat/params.xml"})
        {
            const auto parameters = read_small_text(public_root / filename);
            if (!std::regex_search(parameters, match, socket_config))
                continue;
            const auto base = std::stoi(match[1].str());
            if (base < 1 || base > 65535)
                continue;
            const auto host = match[2].str();
            for (unsigned offset = 0; offset < 16 && base + offset <= 65535; ++offset)
            {
                const auto port = std::to_string(base + offset);
                if (offset && app.find("GlobalProperties.DEFAULT_PORT + " + std::to_string(offset) + ",") == std::string::npos)
                    continue;
                const auto address = host + ":" + port;
                endpoints.emplace(address, "ws://" + address + "/");
            }
        }
        for (const auto& [address, endpoint] : endpoints)
        {
            if (!sockets.empty())
                sockets += ',';
            sockets += web::json_string(address) + ':' + web::json_string(endpoint);
        }
    }
    const auto launch = read_small_text(public_root / "index.html");
    const auto guest = std::regex_search(launch, std::regex(R"rx(\bconst\s+session\s*=\s*["']0["'])rx")) &&
        std::regex_search(launch, std::regex(R"(\bSESSION\s*:\s*session\b)"));
    const auto parameters = guest ? ",\"parameters\":{\"SESSION\":\"0\"}" : "";
    std::error_code error;
    const auto effects = guest && std::filesystem::is_directory(public_root / "data" / "fx", error) ?
        ",\"lazyAssetPaths\":[\"/data/fx/\",\"/data.old/fx/\"]" : "";
    return "{\"http\":{},\"sockets\":{" + sockets + "},\"tcp\":{},\"backendPort\":" + backend + parameters + effects + "}\n";
}
}

std::string html5_classes(const Movie& movie)
{
    ExportReport report;
    return class_modules(web::movie_scripts(movie, {}, report, nullptr));
}

ExportReport export_html5(const Movie& movie, const std::filesystem::path& destination, std::string_view title,
    const std::filesystem::path& source, const std::filesystem::path& workspace)
{
    if (std::filesystem::exists(destination) &&
        (!std::filesystem::is_directory(destination) || !std::filesystem::is_empty(destination)))
        throw std::runtime_error("Choisissez un dossier vide pour créer le projet exporté.");
    const auto input = source.empty() ? std::filesystem::path{} : normalized_path(source);
    auto root = workspace.empty() ? input.parent_path() : normalized_path(workspace);
    if (!input.empty() && !inside(root, input))
        root = input.parent_path();
    Packages packages{std::move(root), std::filesystem::absolute(destination), {}};
    packages.expanded = movie.header().expanded_size;
    if (!input.empty())
        packages.converted.emplace(input, ".");
    return write_package(movie, input, ".", title, packages);
}

WebWorkspaceReport export_html5_workspace(const std::filesystem::path& root, const std::filesystem::path& destination,
    const std::filesystem::path& preferred, const WebMovieOverrides& open_movies,
    const std::function<void(std::size_t, std::size_t)>& progress)
{
    const auto selected_root = normalized_path(root);
    const auto output = normalized_path(destination);
    if (!std::filesystem::is_directory(selected_root))
        throw std::runtime_error("Dossier source introuvable.");
    if (std::filesystem::exists(output) &&
        (!std::filesystem::is_directory(output) || !std::filesystem::is_empty(output)))
        throw std::runtime_error("Choisissez un dossier vide pour créer le projet exporté.");
    const auto selected_htdocs = ascii_lower(utf8_path(selected_root.filename())) == "htdocs";
    const auto parent = selected_root.parent_path();
    const auto project = selected_htdocs && std::filesystem::is_regular_file(parent / "app.js") ? parent : selected_root;
    const auto htdocs = selected_root / "htdocs";
    const auto nested_htdocs = !selected_htdocs && std::filesystem::is_directory(htdocs);
    const auto public_root = nested_htdocs ? htdocs : selected_root;
    std::map<std::string, std::filesystem::path> sources;
    std::string warnings;
    const auto shared = output / "__flashstudio";
    std::filesystem::create_directories(shared);
    scan_web_folder(public_root, output, selected_htdocs || nested_htdocs, sources, warnings);
    if (nested_htdocs)
        scan_web_folder(selected_root, output, false, sources, warnings, public_root);
    find_flash_routes(project, sources);
    std::map<std::filesystem::path, std::shared_ptr<const Movie>> overrides;
    for (const auto& [path, movie] : open_movies)
    {
        if (!movie || path.empty())
            continue;
        const auto name = normalized_path(path);
        if (!inside(selected_root, name) && std::ranges::none_of(sources, [&](const auto& source) { return source.second == name; }))
            continue;
        const auto& base = inside(public_root, name) ? public_root : selected_root;
        if (inside(base, name))
            sources.try_emplace(web_path(name.lexically_relative(base)), name);
        overrides.emplace(name, movie);
    }
    if (sources.empty())
        throw std::runtime_error("Aucun SWF à exporter dans ce dossier.");

    write_text(shared / "stage.js", web::stage_script);
    write_text(shared / "stage.css", web::css);
    write_text(shared / "as3.js", web::class_runtime);
    write_text(shared / "text.js", web::text_runtime);
    write_text(shared / "media.js", web::media_runtime);
    write_text(shared / "network.json", workspace_network(project, public_root));
    write_text(output / "serve.py", web::local_server);

    std::vector<WorkspaceMovie> jobs;
    std::map<std::filesystem::path, std::size_t> by_file;
    std::map<std::string, std::size_t> aliases;
    for (const auto& [url, path] : sources)
    {
        const auto [found, added] = by_file.try_emplace(path, jobs.size());
        if (added)
        {
            const auto edit = overrides.find(path);
            jobs.push_back({path, url, std::format("movies/{:05}", jobs.size() + 1),
                edit == overrides.end() ? nullptr : edit->second});
        }
        else if (url == "/chat/app.swf")
            jobs[found->second].url = url;
        aliases.emplace(url, found->second);
    }
    struct Result { std::size_t omissions{}; std::string error; };
    std::vector<Result> results(jobs.size());
    std::atomic_size_t next{};
    std::size_t done{};
    std::mutex progress_lock, visual_lock;
    std::filesystem::create_directories(shared / "assets");
    const auto convert = [&]
    {
        while (true)
        {
            const auto index = next.fetch_add(1);
            if (index >= jobs.size())
                break;
            const auto& job = jobs[index];
            auto& result = results[index];
            try
            {
                const auto movie = job.edited ? job.edited : std::make_shared<const Movie>(Movie::open(job.path));
                Packages packages{project, shared, {}, 0, 0, &visual_lock};
                result.omissions = write_package(*movie, job.path, job.package, utf8_path(job.path.stem()), packages,
                    url_path(job.url)).failed;
            }
            catch (const std::exception& error) { result.error = error.what(); }
            catch (...) { result.error = "Erreur inconnue."; }
            if (progress)
            {
                const std::scoped_lock lock(progress_lock);
                ++done;
                if (done == jobs.size() || done % 20 == 0)
                    progress(done, jobs.size());
            }
        }
    };
    if (progress)
        progress(0, jobs.size());
    {
        std::vector<std::jthread> workers;
        const auto count = (std::min)({jobs.size(), std::size_t{4}, std::size_t{(std::max)(1u, std::thread::hardware_concurrency())}});
        for (std::size_t thread = 0; thread < count; ++thread)
            workers.emplace_back(convert);
    }

    WebWorkspaceReport report;
    std::string rows;
    std::string entries;
    const auto selected = preferred.empty() ? std::filesystem::path{} : normalized_path(preferred);
    std::size_t start = jobs.size();
    unsigned start_priority = 0;
    for (const auto& [url, index] : aliases)
    {
        const auto& job = jobs[index];
        const auto& result = results[index];
        if (!result.error.empty())
            continue;
        const auto page = "/__flashstudio/" + job.package + "/index.html";
        if (!entries.empty())
            entries += ",\n";
        entries += "    " + web::json_string(url) + ": " + web::json_string(page);
        rows += "<li><a href=\"" + page + "\">" + web::html_text(url) + "</a></li>\n";
        const auto key = ascii_lower(url);
        const unsigned priority = key == "/chat/app.swf" ? 5 : job.path == selected ? 4 : key == "/app.swf" ||
            key == "/main.swf" || key == "/chat.swf" ? 3 : job.path.parent_path() == public_root ? 2 : 1;
        if (priority > start_priority)
        {
            start = index;
            start_priority = priority;
        }
    }
    std::string details = std::move(warnings);
    for (std::size_t index = 0; index < jobs.size(); ++index)
    {
        const auto& result = results[index];
        const auto& job = jobs[index];
        if (!result.error.empty())
        {
            ++report.failed;
            details += "- " + job.url + " · échec de l’export\n";
        }
        else
        {
            ++report.converted;
            if (result.omissions)
            {
                ++report.partial;
                details += std::format("- {} · {} points de portage · __flashstudio/{}/LISEZ-MOI.txt\n",
                    job.url, result.omissions, job.package);
            }
        }
    }
    const auto entry = start == jobs.size() ? "/catalogue.html" : "/__flashstudio/" + jobs[start].package + "/index.html";
    write_text(shared / "catalog.json", "{\n  \"entry\": " + web::json_string(entry) + ",\n  \"movies\": {\n" + entries + "\n  }\n}\n");
    write_text(output / "catalogue.html", "<!doctype html><html lang=\"fr\"><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"><title>Animations HTML5</title>"
        "<link rel=\"stylesheet\" href=\"/__flashstudio/stage.css\"><main class=\"catalogue\"><h1>Animations HTML5</h1>"
        "<a href=\"/\">Ouvrir le film principal</a><label for=\"filter\">Rechercher un film</label>"
        "<input type=\"search\" id=\"filter\"><ul>" + rows + "</ul></main>"
        "<script>const filter = document.querySelector('#filter'); const movies = document.querySelectorAll('li');"
        "filter.addEventListener('input', () => { const query = filter.value.toLowerCase().trim();"
        "for (const movie of movies) movie.hidden = !movie.textContent.toLowerCase().includes(query); });</script></html>");
    write_text(output / "index.html", "<!doctype html><meta charset=\"utf-8\"><title>Projet HTML5</title>"
        "<a href=\"" + entry + "\">Ouvrir le film principal</a><a href=\"/catalogue.html\">Catalogue des animations</a>"
        "<script>location.replace(" + web::json_string(entry) + " + location.search + location.hash)</script>\n");
    write_text(output / "LISEZ-MOI.txt", std::format("Projet HTML5 Flash Studio\n\n"
        "Démarrage local : py -3 serve.py, puis ouvrir http://127.0.0.1:8000/.\n"
        "Catalogue : http://127.0.0.1:8000/catalogue.html\n"
        "Hébergement : publier le dossier à la racine du site. Les services HTTP et WebSocket du projet restent nécessaires.\n"
        "Configuration locale : __flashstudio/network.json\n"
        "Film principal : {}\n\n"
        "{} SWF exportés, dont {} avec des points de portage. {} échecs.\n\n{}",
        start == jobs.size() ? "" : jobs[start].url, report.converted, report.partial, report.failed, details));
    return report;
}
}
