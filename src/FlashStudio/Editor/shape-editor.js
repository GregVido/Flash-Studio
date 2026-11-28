import {
    bounds, components, equalPoint, fillPath, gradientMatrix, paintEdges, pointKey,
    primitive, replaceStyle, solid, strokePath, transform
} from "./shape-geometry.js";

const $ = id => document.getElementById(id);
const canvas = $("canvas"), drawing = $("drawing"), definitions = $("definitions");
const selection = $("selection"), transient = $("transient");
const bridge = window.chrome?.webview;
const locale = {
    fr: {
        undo: "Annuler", redo: "Rétablir", close: "Fermer", save: "Appliquer au SWF",
        select: "Sélection et déplacement", nodes: "Sommets et courbes", rectangle: "Rectangle", ellipse: "Ellipse",
        line: "Ligne", pen: "Polygone", zoomOut: "Réduire", zoomIn: "Agrandir", fit: "Ajuster",
        elements: "Éléments", item: "Élément", position: "Position et taille", width: "Largeur", height: "Hauteur",
        fill: "Remplissage", stroke: "Contour", color: "Couleur", opacity: "Opacité", none: "Aucun", solid: "Uni",
        linear: "Dégradé linéaire", radial: "Dégradé radial", bitmap: "Bitmap d’origine",
        stop: "Position (%)", remove: "Supprimer", addStop: "Ajouter une couleur", angle: "Angle (°)", thickness: "Épaisseur (px)",
        selectHint: "Glissez un élément pour le déplacer. Molette pour zoomer. Espace + glisser pour naviguer.",
        nodesHint: "Glissez les sommets carrés et les poignées rondes des courbes.",
        penHint: "Placez les sommets. Entrée ou double-clic pour fermer le polygone.",
        drawHint: "Tracez la forme en glissant sur le dessin.",
        noSelection: "Sélectionnez un élément du dessin pour modifier ses propriétés.",
        empty: "Dessin vide",
        discard: "Fermer et abandonner les modifications ?", keep: "Continuer l’édition", discardAction: "Abandonner"
    },
    en: {
        undo: "Undo", redo: "Redo", close: "Close", save: "Apply to SWF",
        select: "Select and move", nodes: "Nodes and curves", rectangle: "Rectangle", ellipse: "Ellipse",
        line: "Line", pen: "Polygon", zoomOut: "Zoom out", zoomIn: "Zoom in", fit: "Fit",
        elements: "Elements", item: "Element", position: "Position and size", width: "Width", height: "Height",
        fill: "Fill", stroke: "Stroke", color: "Color", opacity: "Opacity", none: "None", solid: "Solid",
        linear: "Linear gradient", radial: "Radial gradient", bitmap: "Original bitmap",
        stop: "Position (%)", remove: "Remove", addStop: "Add color", angle: "Angle (°)", thickness: "Width (px)",
        selectHint: "Drag an element to move it. Scroll to zoom. Hold Space and drag to pan.",
        nodesHint: "Drag square nodes and round curve handles.",
        penHint: "Place the vertices. Press Enter or double-click to close the polygon.",
        drawHint: "Drag on the canvas to draw the shape.",
        noSelection: "Select an element to edit its properties.",
        empty: "Empty drawing",
        discard: "Close and discard changes?", keep: "Keep editing", discardAction: "Discard"
    }
};
let language = locale.fr;
let model, textures, groups = [], selected, currentFill, currentStroke;
let view, baseWidth, tool = "select", gesture, polygon = [], space = false, saving = false;
let revision = 0, nextRevision = 0;
let reportedDirty = false;
const undo = [], redo = [];
const namespace = "http://www.w3.org/2000/svg";
const rgba = ([r, g, b, a]) => `rgba(${r},${g},${b},${a / 255})`;
const inPixels = twips => Number((twips / 20).toFixed(2));
const toTwips = pixels => Math.round(pixels * 20);
const hex = color => "#" + color.slice(0, 3).map(component => component.toString(16).padStart(2, "0")).join("");
const rgb = input => input.match(/[\da-f]{2}/gi).map(part => parseInt(part, 16));
const findGroup = () => groups.find(group => group.id === selected);

function svg(tag, attrs = {}) {
    const element = document.createElementNS(namespace, tag);
    for (const [key, value] of Object.entries(attrs)) element.setAttribute(key, value);
    return element;
}

function html(tag, content, classname) {
    const element = document.createElement(tag);
    if (content) element.textContent = content;
    if (classname) element.className = classname;
    return element;
}

function notice(message, error = false) {
    $("status").textContent = message;
    $("status").dataset.error = error;
}

function hint() {
    notice(language[tool === "select" ? "selectHint" : tool === "nodes" ? "nodesHint" : tool === "pen" ? "penHint" : "drawHint"]);
}

function updateActions() {
    $("undo").disabled = !undo.length || saving;
    $("redo").disabled = !redo.length || saving;
    $("save").disabled = !revision || saving;
    $("close").disabled = saving;
    document.querySelector("main").inert = saving;
    const dirty = Boolean(revision);
    if (dirty !== reportedDirty) {
        reportedDirty = dirty;
        bridge?.postMessage({ action: "dirty", value: dirty });
    }
}

function remember() {
    const limit = Math.max(3, Math.min(32, Math.floor(240000 / Math.max(1, model.edges.length))));
    undo.push({ model: structuredClone(model), selected, revision });
    if (undo.length > limit) undo.shift();
    redo.length = 0;
}

function commit(change) {
    remember();
    change();
    revision = ++nextRevision;
    render();
}

function changeHistory(source, target) {
    if (!source.length || saving) return;
    target.push({ model, selected, revision });
    ({ model, selected, revision } = source.pop());
    currentFill = currentStroke = undefined;
    render();
}

function drawPaint(paint, id) {
    if (!paint.type) return rgba(paint.color);
    if (paint.type >= 0x40) {
        const texture = textures[paint.bitmap];
        if (!texture) return "#566981";
        const pattern = svg("pattern", {
            id, patternUnits: "userSpaceOnUse", width: texture.width, height: texture.height,
            patternTransform: `matrix(${paint.matrix.join(" ")})`
        });
        pattern.append(svg("image", { href: texture.src, width: texture.width, height: texture.height }));
        definitions.append(pattern);
        return `url(#${id})`;
    }
    const linear = paint.type === 0x10;
    const gradient = svg(linear ? "linearGradient" : "radialGradient", {
        id, gradientUnits: "userSpaceOnUse",
        gradientTransform: `matrix(${paint.matrix.join(" ")})`,
        spreadMethod: ["pad", "reflect", "repeat", "pad"][paint.flags >> 6],
        "color-interpolation": paint.flags & 16 ? "linearRGB" : "sRGB"
    });
    if (linear) {
        gradient.setAttribute("x1", -16384); gradient.setAttribute("x2", 16384);
        gradient.setAttribute("y1", 0); gradient.setAttribute("y2", 0);
    } else {
        gradient.setAttribute("cx", 0); gradient.setAttribute("cy", 0); gradient.setAttribute("r", 16384);
        gradient.setAttribute("fx", Math.max(-16380, Math.min(16380, paint.focal * 64)));
    }
    for (const stop of paint.stops)
        gradient.append(svg("stop", { offset: stop.ratio / 255, "stop-color": hex(stop.color), "stop-opacity": stop.color[3] / 255 }));
    definitions.append(gradient);
    return `url(#${id})`;
}

function draw() {
    definitions.replaceChildren();
    const layers = [];
    for (const group of groups) {
        const { fills, strokes } = paintEdges(model, group);
        for (const [index, edges] of fills)
            layers.push({ group, index, edges, stroke: false });
        for (const [index, edges] of strokes)
            layers.push({ group, index, edges, stroke: true });
    }
    layers.sort((a, b) => a.group.layer - b.group.layer || Number(a.stroke) - Number(b.stroke) || a.index - b.index);
    const palettes = new Map();
    const output = document.createDocumentFragment();
    for (const { group, index, edges, stroke } of layers) {
        const key = (stroke ? "stroke-" : "fill-") + index;
        const style = stroke ? model.strokes[index] : model.fills[index];
        const paint = stroke ? style.paint : style;
        const attrs = { "data-group": group.id, [stroke ? "data-stroke" : "data-fill"]: index };
        if (!stroke && (paint.type === 0x41 || paint.type === 0x43) && textures[paint.bitmap]) {
            const texture = textures[paint.bitmap];
            const clipID = `clip-${group.id}-${index}`;
            const d = fillPath(edges);
            const rule = model.flags & 4 ? "nonzero" : "evenodd";
            const clip = svg("clipPath", { id: clipID });
            clip.append(svg("path", { d, "clip-rule": rule }));
            definitions.append(clip);
            const contents = svg("g", attrs);
            contents.append(svg("path", { d, fill: "transparent", "fill-rule": rule }));
            const image = svg("g", { "clip-path": `url(#${clipID})`, "pointer-events": "none" });
            image.append(svg("image", { href: texture.src, width: texture.width, height: texture.height, transform: `matrix(${paint.matrix.join(" ")})` }));
            contents.append(image);
            output.append(contents);
            continue;
        }
        if (!palettes.has(key)) palettes.set(key, drawPaint(paint, key));
        const brush = palettes.get(key);
        if (stroke) {
            const caps = ["round", "butt", "square", "round"];
            const joins = ["round", "bevel", "miter", "round"];
            output.append(svg("path", { ...attrs, d: strokePath(edges, style.flags & 4), fill: "none", stroke: brush,
                "stroke-width": Math.max(style.width, 1), "stroke-linecap": caps[(style.flags >> 14) & 3],
                "stroke-linejoin": joins[(style.flags >> 12) & 3], "stroke-miterlimit": Math.max(style.miter / 256, 1) }));
        } else {
            output.append(svg("path", { ...attrs, d: fillPath(edges), fill: brush, "fill-rule": model.flags & 4 ? "nonzero" : "evenodd" }));
        }
    }
    drawing.replaceChildren(output);
}

function drawSelection() {
    const group = findGroup();
    selection.removeAttribute("transform");
    selection.replaceChildren();
    if (!group || !view) return;
    const [x0, y0, x1, y1] = group.bounds;
    const size = view[2] / Math.max(canvas.clientWidth, 1);
    const neutral = { stroke: "#74f4d4", "stroke-width": 1, "vector-effect": "non-scaling-stroke", fill: "none" };
    selection.append(svg("rect", { ...neutral, x: x0 - size * 4, y: y0 - size * 4, width: x1 - x0 + size * 8,
        height: y1 - y0 + size * 8, "stroke-dasharray": "5 4" }));
    if (tool !== "nodes") return;
    const shown = p => p[0] >= view[0] - 10 * size && p[0] <= view[0] + view[2] + 10 * size &&
        p[1] >= view[1] - 10 * size && p[1] <= view[1] + view[3] + 10 * size;
    const points = new Map(), controls = [];
    for (const index of group.indices) {
        const [from, to, control] = model.edges[index];
        for (const node of [from, to])
            if (shown(node))
                points.set(pointKey(node), node);
        if (control && shown(control) && controls.length < 1000) {
            selection.append(svg("path", { ...neutral, stroke: "#a4b4c8", "stroke-dasharray": "2 3",
                d: `M${from.join(" ")} L${control.join(" ")} L${to.join(" ")}` }));
            controls.push(svg("circle", { ...neutral, class: "handle", "data-curve": index, cx: control[0], cy: control[1],
                r: 4 * size, fill: "#1f2732" }));
        }
    }
    selection.append(...controls);
    for (const [key, [x, y]] of [...points].slice(0, 1300))
        selection.append(svg("rect", { ...neutral, class: "handle", "data-node": key, x: x - 3.5 * size, y: y - 3.5 * size,
            width: 7 * size, height: 7 * size, fill: "#1f2732" }));
}

function setView(value) {
    view = value;
    canvas.setAttribute("viewBox", view.join(" "));
    $("zoom-level").textContent = Math.round(baseWidth / view[2] * 100) + " %";
    drawSelection();
}

function fit() {
    if (!model || canvas.clientWidth < 2 || canvas.clientHeight < 2) return;
    const [left, top, right, bottom] = model.edges.length ? bounds(model.edges) :
        [model.bounds[0], model.bounds[2], model.bounds[1], model.bounds[3]];
    const ratio = Math.max(canvas.clientWidth, 1) / Math.max(canvas.clientHeight, 1);
    const width = Math.max(120, (right - left) * 1.27, (bottom - top) * ratio * 1.27);
    const height = width / ratio;
    baseWidth = width;
    setView([(left + right - width) / 2, (top + bottom - height) / 2, width, height]);
}

function location(event) {
    const inverse = canvas.getScreenCTM()?.inverse();
    const point = inverse && new DOMPoint(event.clientX, event.clientY).matrixTransform(inverse);
    return point ? [Math.round(point.x), Math.round(point.y)] : [0, 0];
}

function zoom(factor, target) {
    if (!view || view[2] * factor < 20 || view[2] * factor > 1e8) return;
    const [x, y] = target ?? [view[0] + view[2] / 2, view[1] + view[3] / 2];
    setView([x + (view[0] - x) * factor, y + (view[1] - y) * factor, view[2] * factor, view[3] * factor]);
}

function input(label, type, value, change, options = {}) {
    const wrapper = html("label", label);
    const field = html("input");
    field.type = type;
    field.value = value;
    Object.assign(field, options);
    field.addEventListener("change", () => {
        if (type === "number" && (!field.value || !field.checkValidity())) {
            field.value = value;
            return;
        }
        change(type === "number" ? Number(field.value) : field.value);
    });
    wrapper.append(field);
    return wrapper;
}

function choice(label, entries, value, change) {
    const wrapper = html("label", label);
    const list = html("select");
    for (const [id, title] of entries) {
        const option = html("option", title);
        option.value = id;
        list.append(option);
    }
    list.value = value;
    list.addEventListener("change", () => change(Number(list.value)));
    wrapper.append(list);
    return wrapper;
}

function styleList(model, group, fields) {
    const styles = new Set();
    for (const i of group.indices) for (const field of fields) if (model.edges[i][field]) styles.add(model.edges[i][field]);
    return [...styles];
}

function properties() {
    const panel = $("properties");
    const group = findGroup();
    if (!group) {
        panel.replaceChildren(html("div", language.noSelection, "muted"));
        return;
    }
    const position = html("div", "", "property-group");
    const coordinates = html("div", "", "fields");
    const [left, top, right, bottom] = group.bounds;
    const x = inPixels(left), y = inPixels(top), w = inPixels(right - left), h = inPixels(bottom - top);
    coordinates.append(
        input("X (px)", "number", x, value => commit(() => transform(model, group, 1, 1, toTwips(value - x), 0)), { step: .05 }),
        input("Y (px)", "number", y, value => commit(() => transform(model, group, 1, 1, 0, toTwips(value - y))), { step: .05 }),
        input(language.width, "number", w, value => {
            if (w) commit(() => transform(model, group, value / w, 1, left * (1 - value / w), 0));
        }, { min: .05, step: .05, disabled: w === 0 }),
        input(language.height, "number", h, value => {
            if (h) commit(() => transform(model, group, 1, value / h, 0, top * (1 - value / h)));
        }, { min: .05, step: .05, disabled: h === 0 })
    );
    position.append(html("h2", language.position), coordinates);

    const fills = styleList(model, group, [3, 4]);
    const strokes = styleList(model, group, [5]);
    if (!fills.includes(currentFill)) currentFill = fills[0] ?? 0;
    if (!strokes.includes(currentStroke)) currentStroke = strokes[0] ?? 0;
    const fill = model.fills[currentFill], stroke = model.strokes[currentStroke];
    const fillPanel = html("div", "", "property-group"), strokePanel = html("div", "", "property-group");
    const setFill = value => commit(() => { currentFill = replaceStyle(model, group, "fills", currentFill, value); });
    const setStroke = value => commit(() => { currentStroke = replaceStyle(model, group, "strokes", currentStroke, value); });
    const type = !currentFill ? -1 : fill.type >= 0x40 ? 64 : fill.type === 0x13 ? 18 : fill.type;
    const defaultColor = () => fill.stops[0]?.color ?? (type === 0 ? fill.color : solid().color);
    fillPanel.append(html("h2", language.fill));
    if (fills.length > 1)
        fillPanel.append(choice(language.fill, fills.map(index => [index, "#" + index]), currentFill, value => { currentFill = value; properties(); }));
    const choices = [[-1, language.none], [0, language.solid], [16, language.linear], [18, language.radial]];
    if (type === 64) choices.push([64, language.bitmap]);
    fillPanel.append(choice(language.fill, choices, type, value => {
        if (value < 0) setFill(null);
        else if (!value) setFill(solid(defaultColor()));
        else if (value < 64) setFill({ ...solid(), type: value, matrix: gradientMatrix(group.bounds, value === 18, 0),
            stops: fill.stops.length > 1 ? fill.stops : [{ ratio: 0, color: defaultColor() }, { ratio: 255, color: [114, 90, 220, 255] }] });
    }));
    if (!type) {
        const colors = html("div", "", "fields");
        colors.append(input(language.color, "color", hex(fill.color), value => setFill(solid([...rgb(value), fill.color[3]]))));
        if (model.version >= 3)
            colors.append(input(language.opacity, "number", Math.round(fill.color[3] / 2.55),
                value => setFill(solid([...fill.color.slice(0, 3), Math.round(value * 2.55)])), { min: 0, max: 100 }));
        fillPanel.append(colors);
    } else if (type === 16 || type === 18) {
        if (type === 16)
            fillPanel.append(input(language.angle, "number", Math.round(Math.atan2(fill.matrix[1], fill.matrix[0]) * 180 / Math.PI),
                value => setFill({ ...fill, matrix: gradientMatrix(group.bounds, false, value) }), { step: 1 }));
        const stops = html("div", "", "stops");
        const setStops = result => setFill({ ...fill, stops: result.sort((a, b) => a.ratio - b.ratio) });
        fill.stops.forEach((stop, i) => {
            const row = html("div", "", "stop");
            const update = item => setStops(fill.stops.map((value, index) => index === i ? { ...value, ...item } : value));
            row.append(input(language.stop, "number", Math.round(stop.ratio / 2.55), value => update({ ratio: Math.round(value * 2.55) }), { min: 0, max: 100 }),
                input(language.color, "color", hex(stop.color), value => update({ color: [...rgb(value), stop.color[3]] })));
            const remove = html("button", "×");
            remove.title = language.remove;
            remove.setAttribute("aria-label", language.remove);
            remove.disabled = fill.stops.length <= 2;
            remove.onclick = () => setStops(fill.stops.filter((_, j) => j !== i));
            row.append(remove);
            stops.append(row);
        });
        const add = html("button", language.addStop);
        add.disabled = fill.stops.length >= (model.version === 4 ? 15 : 8);
        add.onclick = () => {
            let index = 0;
            for (let i = 1; i + 1 < fill.stops.length; i++)
                if (fill.stops[i + 1].ratio - fill.stops[i].ratio > fill.stops[index + 1].ratio - fill.stops[index].ratio) index = i;
            const a = fill.stops[index], b = fill.stops[index + 1] ?? a;
            setStops([...fill.stops, { ratio: Math.round((a.ratio + b.ratio) / 2), color: a.color.map((value, i) => Math.round((value + b.color[i]) / 2)) }]);
        };
        fillPanel.append(stops, add);
    }

    strokePanel.append(html("h2", language.stroke));
    const outline = currentStroke || stroke.width ? stroke : { flags: 0, miter: 1024, width: 20, paint: solid([19, 29, 42, 255]) };
    strokePanel.append(choice(language.stroke, [[0, language.none], [1, language.solid]], currentStroke ? 1 : 0,
        value => setStroke(value ? outline : null)));
    if (strokes.length > 1)
        strokePanel.append(choice(language.stroke, strokes.map(index => [index, "#" + index]), currentStroke, value => { currentStroke = value; properties(); }));
    if (currentStroke) {
        const colors = html("div", "", "fields");
        colors.append(input(language.color, "color", hex(outline.paint.stops[0]?.color ?? outline.paint.color), value =>
            setStroke({ ...outline, flags: outline.flags & ~0x0800, paint: solid([...rgb(value), 255]) })),
            input(language.thickness, "number", inPixels(outline.width), value => setStroke({ ...outline, width: toTwips(value) }),
                { min: 0, max: 3276.75, step: .05 }));
        strokePanel.append(colors);
    }
    panel.replaceChildren(position, fillPanel, strokePanel);
}

function renderList() {
    const list = $("elements");
    const position = list.scrollTop;
    const output = document.createDocumentFragment();
    groups.forEach((group, index) => {
        const button = html("button", `${language.item} ${index + 1}`);
        button.setAttribute("aria-current", group.id === selected);
        button.onclick = () => select(group.id);
        output.append(button);
    });
    if (!groups.length) output.append(html("div", language.empty, "muted"));
    list.replaceChildren(output);
    list.scrollTop = position;
}

function select(id, fill, stroke) {
    selected = id;
    currentFill = fill;
    currentStroke = stroke;
    drawSelection();
    properties();
    renderList();
}

function render() {
    groups = components(model.edges);
    if (!findGroup()) selected = undefined;
    draw();
    drawSelection();
    renderList();
    properties();
    const [x0, y0, x1, y1] = bounds(model.edges);
    $("dimensions").textContent = `${inPixels(x1 - x0)} × ${inPixels(y1 - y0)} px`;
    updateActions();
}

function switchTool(value) {
    gesture = undefined;
    tool = value;
    polygon = [];
    transient.replaceChildren();
    selection.removeAttribute("transform");
    canvas.dataset.tool = value;
    document.querySelectorAll("[data-tool]").forEach(button => button.setAttribute("aria-pressed", button.dataset.tool === value));
    hint();
    drawSelection();
}

function addShape(edges, open) {
    const valid = edges.filter(edge => !equalPoint(edge[0], edge[1]) || (edge[2] && !equalPoint(edge[0], edge[2])));
    if (!valid.length) return;
    commit(() => {
        const fill = open ? 0 : model.fills.push(solid()) - 1;
        const stroke = model.strokes.push({ width: 20, flags: 0, miter: 1024, paint: solid([25, 75, 81, 255]) }) - 1;
        const previous = model.edges.at(-1)?.[6] ?? 0;
        const layer = model.version === 1 || !model.edges.length ? previous : previous + 1;
        selected = model.edges.length;
        currentFill = fill; currentStroke = stroke;
        for (const edge of valid) model.edges.push([...edge, fill, 0, stroke, layer]);
    });
}

function finishPolygon() {
    const unique = polygon.filter((p, i) => !i || !equalPoint(p, polygon[i - 1]));
    if (unique.length >= 3) addShape(primitive("pen", null, null, unique), false);
    polygon = [];
    transient.replaceChildren();
}

function previewLines(edges, close = false) {
    transient.replaceChildren(svg("path", { d: close ? fillPath(edges) : strokePath(edges, true),
        fill: close ? "#6fdfc446" : "none", stroke: "#74f4d4", "stroke-width": 1.5, "vector-effect": "non-scaling-stroke" }));
}

function draftNode(group, drag, to) {
    return group.indices.map(index => {
        const original = model.edges[index];
        if (drag.curve !== undefined)
            return index === drag.curve ? [original[0], original[1], to] : original;
        return [pointKey(original[0]) === drag.node ? to : original[0], pointKey(original[1]) === drag.node ? to : original[1], original[2]];
    });
}

canvas.addEventListener("pointerdown", event => {
    if (!model || saving || event.button > 1) return;
    const from = location(event);
    canvas.focus({ preventScroll: true });
    if (space || event.button === 1) {
        gesture = { mode: "pan", x: event.clientX, y: event.clientY, startView: [...view] };
    } else if (tool === "select" || tool === "nodes") {
        const handle = event.target.closest(".handle");
        if (handle && findGroup()) {
            gesture = { mode: "node", from, group: findGroup(), node: handle.dataset.node,
                curve: handle.dataset.curve === undefined ? undefined : Number(handle.dataset.curve) };
        } else {
            const hit = event.target.closest("[data-group]");
            select(hit ? Number(hit.dataset.group) : undefined, Number(hit?.dataset.fill) || undefined, Number(hit?.dataset.stroke) || undefined);
            if (tool === "select" && hit) gesture = { mode: "move", from, group: findGroup() };
        }
    } else if (tool === "pen") {
        polygon.push(from);
        if (event.detail >= 2) finishPolygon();
        return;
    } else {
        gesture = { mode: "draw", from };
    }
    if (gesture) canvas.setPointerCapture(event.pointerId);
});

canvas.addEventListener("pointermove", event => {
    if (!model) return;
    const at = location(event);
    if (!gesture) {
        if (tool === "pen" && polygon.length) {
            const points = [...polygon, at];
            previewLines(points.slice(0, -1).map((point, i) => [point, points[i + 1], null]));
        }
        return;
    }
    if (gesture.mode === "pan") {
        const [x, y, width, height] = gesture.startView;
        const scale = width / canvas.clientWidth;
        setView([x - (event.clientX - gesture.x) * scale, y - (event.clientY - gesture.y) * scale, width, height]);
    } else if (gesture.mode === "move") {
        const position = `translate(${at[0] - gesture.from[0]} ${at[1] - gesture.from[1]})`;
        drawing.querySelectorAll(`[data-group="${gesture.group.id}"]`).forEach(item => item.setAttribute("transform", position));
        selection.setAttribute("transform", position);
    } else if (gesture.mode === "node") {
        previewLines(draftNode(gesture.group, gesture, at));
    } else {
        previewLines(primitive(tool, gesture.from, at), tool !== "line");
    }
});

function cancelGesture() {
    const moving = gesture?.mode === "move";
    gesture = undefined;
    transient.replaceChildren();
    if (moving) {
        drawing.querySelectorAll("[data-group][transform]").forEach(item => item.removeAttribute("transform"));
        selection.removeAttribute("transform");
    }
}

canvas.addEventListener("pointerup", event => {
    if (!gesture) return;
    const drag = gesture;
    const at = location(event);
    cancelGesture();
    if (drag.mode === "pan" || equalPoint(at, drag.from)) return;
    if (drag.mode === "move")
        commit(() => transform(model, drag.group, 1, 1, at[0] - drag.from[0], at[1] - drag.from[1]));
    else if (drag.mode === "node") {
        const updated = draftNode(drag.group, drag, at);
        commit(() => drag.group.indices.forEach((index, offset) => model.edges[index].splice(0, 3, ...updated[offset].slice(0, 3))));
    } else if (tool === "line" || (at[0] !== drag.from[0] && at[1] !== drag.from[1]))
        addShape(primitive(tool, drag.from, at), tool === "line");
});
canvas.addEventListener("pointercancel", cancelGesture);
canvas.addEventListener("dblclick", () => { if (tool === "pen" && !saving) finishPolygon(); });
canvas.addEventListener("wheel", event => {
    if (!view || gesture) return;
    event.preventDefault();
    zoom(Math.exp(Math.max(-240, Math.min(240, event.deltaY)) * .0018), location(event));
}, { passive: false });

function removeSelection() {
    const group = findGroup();
    if (!group) return;
    commit(() => {
        const removed = new Set(group.indices);
        model.edges = model.edges.filter((_, i) => !removed.has(i));
        selected = undefined;
    });
}

function save() {
    if (!model || saving || !revision) return;
    saving = true;
    updateActions();
    bridge?.postMessage({ action: "save", ...model });
}

function close() {
    if (saving || document.querySelector("dialog[open]")) return;
    if (!revision) {
        bridge?.postMessage({ action: "close" });
        return;
    }
    const dialog = html("dialog");
    dialog.append(html("p", language.discard));
    const keep = html("button", language.keep), discard = html("button", language.discardAction);
    keep.onclick = () => dialog.close();
    discard.onclick = () => bridge?.postMessage({ action: "close" });
    dialog.append(keep, discard);
    document.body.append(dialog);
    dialog.addEventListener("close", () => dialog.remove(), { once: true });
    dialog.showModal();
}

document.querySelectorAll("[data-tool]").forEach(button => { button.onclick = () => switchTool(button.dataset.tool); });
$("fit").onclick = fit;
$("zoom-in").onclick = () => zoom(.8);
$("zoom-out").onclick = () => zoom(1.25);
$("save").onclick = save;
$("close").onclick = close;
$("undo").onclick = () => changeHistory(undo, redo);
$("redo").onclick = () => changeHistory(redo, undo);
window.addEventListener("keydown", event => {
    if (!model || saving || document.querySelector("dialog[open]")) return;
    const key = event.key.toLowerCase();
    if (event.ctrlKey && key === "s") { event.preventDefault(); save(); return; }
    if (event.target.matches("input, select")) return;
    if (event.ctrlKey && (key === "z" || key === "y")) {
        event.preventDefault();
        if (key === "y" || event.shiftKey) changeHistory(redo, undo);
        else changeHistory(undo, redo);
        return;
    }
    if (key === " ") { event.preventDefault(); space = true; }
    else if (key === "escape") { cancelGesture(); polygon = []; select(undefined); }
    else if (key === "delete" || key === "backspace") { event.preventDefault(); removeSelection(); }
    else if (key === "enter" && tool === "pen") { event.preventDefault(); finishPolygon(); }
    else if (!event.ctrlKey && /^[varolp]$/.test(key))
        switchTool({ v: "select", a: "nodes", r: "rectangle", o: "ellipse", l: "line", p: "pen" }[key]);
});
window.addEventListener("keyup", event => { if (event.key === " ") space = false; });
window.addEventListener("blur", () => { space = false; cancelGesture(); });

bridge?.addEventListener("message", ({ data }) => {
    if (data.action === "theme") {
        document.documentElement.dataset.theme = data.value;
    } else if (data.action === "open") {
        language = locale[data.locale] ?? locale.en;
        document.documentElement.lang = data.locale;
        $("title").textContent = data.title;
        document.querySelectorAll("[data-label]").forEach(element => {
            if (element.dataset.tool) {
                element.title = language[element.dataset.label];
                element.setAttribute("aria-label", element.title);
            } else if (element.id === "zoom-in" || element.id === "zoom-out") {
                element.title = language[element.dataset.label];
                element.setAttribute("aria-label", element.title);
            } else element.textContent = language[element.dataset.label];
        });
        const { fills, strokes, edges, version, flags, bounds } = data;
        model = { fills, strokes, edges, version, flags, bounds };
        textures = data.textures;
        render();
        switchTool("select");
        requestAnimationFrame(fit);
        new ResizeObserver(() => { if (!view) fit(); else drawSelection(); }).observe(canvas);
    } else if (data.action === "state") {
        saving = data.saving;
        updateActions();
        notice(data.message, data.error);
    } else if (data.action === "request-close") close();
});
bridge?.postMessage({ action: "ready" });
