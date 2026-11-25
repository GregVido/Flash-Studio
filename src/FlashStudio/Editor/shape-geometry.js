export const pointKey = ([x, y]) => x + "," + y;
export const equalPoint = (a, b) => a && b && a[0] === b[0] && a[1] === b[1];

export function bounds(edges) {
    let left = Infinity, top = Infinity, right = -Infinity, bottom = -Infinity;
    for (const edge of edges) {
        for (const p of [edge[0], edge[1], edge[2]]) {
            if (!p) continue;
            left = Math.min(left, p[0]); top = Math.min(top, p[1]);
            right = Math.max(right, p[0]); bottom = Math.max(bottom, p[1]);
        }
    }
    return edges.length ? [left, top, right, bottom] : [0, 0, 1000, 700];
}

export function components(edges) {
    const parents = edges.map((_, i) => i);
    const root = i => {
        while (parents[i] !== i) {
            parents[i] = parents[parents[i]];
            i = parents[i];
        }
        return i;
    };
    const vertices = new Map();
    edges.forEach((edge, index) => {
        for (const endpoint of [edge[0], edge[1]]) {
            const token = edge[6] + ":" + pointKey(endpoint);
            if (vertices.has(token)) {
                const a = root(vertices.get(token)), b = root(index);
                parents[Math.max(a, b)] = Math.min(a, b);
            } else vertices.set(token, index);
        }
    });
    const byRoot = new Map();
    edges.forEach((edge, index) => {
        const id = root(index);
        if (!byRoot.has(id)) byRoot.set(id, { id, layer: edge[6], indices: [], fills: new Set() });
        const entry = byRoot.get(id);
        entry.indices.push(index);
        [edge[3], edge[4]].forEach(style => { if (style) entry.fills.add(style); });
    });
    const groups = [...byRoot.values()];
    for (const group of groups) group.bounds = bounds(group.indices.map(index => edges[index]));
    // Keep enclosed contours of the same paint together, including holes.
    if (groups.length <= 512) for (const inside of groups) {
        const [x0, y0, x1, y1] = inside.bounds;
        let container, area = Infinity;
        for (const outside of groups) {
            if (inside === outside || inside.layer !== outside.layer) continue;
            const [l, t, r, b] = outside.bounds;
            if (l > x0 || t > y0 || r < x1 || b < y1 || (l === x0 && t === y0 && r === x1 && b === y1) ||
                ![...inside.fills].some(fill => outside.fills.has(fill))) continue;
            const size = (r - l) * (b - t);
            if (size < area && encloses(outside, edges, edges[inside.id][0])) {
                container = outside;
                area = size;
            }
        }
        if (container) inside.container = container;
    }
    for (const inside of groups) {
        if (!inside.container) continue;
        let outside = inside.container;
        while (outside.container) outside = outside.container;
        outside.indices = outside.indices.concat(inside.indices);
    }
    return groups.filter(group => !group.container).map(group => {
        group.indices.sort((a, b) => a - b);
        return group;
    });
}

function encloses(group, edges, [x, y]) {
    let winding = 0;
    const cross = ([x0, y0], [x1, y1]) => {
        if ((y0 > y) !== (y1 > y) && x0 + (x1 - x0) * (y - y0) / (y1 - y0) > x)
            winding += y1 > y0 ? 1 : -1;
    };
    for (const index of group.indices) {
        const [from, to, control, a, b] = edges[index];
        if (a === b) continue;
        if (!control) cross(from, to);
        else {
            let previous = from;
            for (let s = 1; s <= 8; s++) {
                const t = s / 8, u = 1 - t;
                const next = [u * u * from[0] + 2 * u * t * control[0] + t * t * to[0],
                    u * u * from[1] + 2 * u * t * control[1] + t * t * to[1]];
                cross(previous, next);
                previous = next;
            }
        }
    }
    return winding !== 0;
}

function segment(edge) {
    return edge[2] ? `Q${edge[2].join(" ")} ${edge[1].join(" ")}` : `L${edge[1].join(" ")}`;
}

export function fillPath(edges) {
    const starts = new Map();
    const used = new Set();
    edges.forEach((edge, index) => {
        const key = pointKey(edge[0]);
        if (!starts.has(key)) starts.set(key, []);
        starts.get(key).push(index);
    });
    let path = "";
    edges.forEach(([from], index) => {
        if (used.has(index)) return;
        path += "M" + from.join(" ");
        for (let next = index; next !== undefined;) {
            const edge = edges[next];
            used.add(next);
            path += segment(edge);
            if (equalPoint(from, edge[1])) break;
            const pool = starts.get(pointKey(edge[1]));
            while (pool?.length && used.has(pool[0])) pool.shift();
            next = pool?.[0];
        }
        path += "Z";
    });
    return path;
}

export function strokePath(edges, noClose) {
    let path = "", start, end;
    for (const edge of edges) {
        if (!equalPoint(end, edge[0])) {
            if (!noClose && equalPoint(start, end)) path += "Z";
            start = edge[0];
            path += "M" + start.join(" ");
        }
        path += segment(edge);
        end = edge[1];
    }
    return !noClose && equalPoint(start, end) ? path + "Z" : path;
}

export function paintEdges(model, group) {
    const fills = new Map(), strokes = new Map();
    const add = (map, key, edge) => {
        if (!map.has(key)) map.set(key, []);
        map.get(key).push(edge);
    };
    for (const index of group.indices) {
        const edge = model.edges[index];
        if (edge[3] !== edge[4]) {
            if (edge[3]) add(fills, edge[3], edge);
            if (edge[4]) add(fills, edge[4], [edge[1], edge[0], edge[2]]);
        }
        if (edge[5]) add(strokes, edge[5], edge);
    }
    return { fills, strokes };
}

export const solid = (color = [38, 200, 174, 255]) => ({
    type: 0, color, matrix: [1, 0, 0, 1, 0, 0], flags: 0, focal: 0, bitmap: 0, stops: []
});

export function gradientMatrix([x0, y0, x1, y1], radial, angle) {
    const dx = Math.max(2, x1 - x0), dy = Math.max(2, y1 - y0);
    const x = Math.round((x0 + x1) / 2), y = Math.round((y0 + y1) / 2);
    if (radial) return [dx / 32768, 0, 0, dy / 32768, x, y];
    const rad = angle * Math.PI / 180, a = Math.cos(rad), b = Math.sin(rad);
    const length = (Math.abs(a) * dx + Math.abs(b) * dy) / 32768;
    return [a * length, b * length, -b * length, a * length, x, y];
}

export function replaceStyle(model, group, slot, old, value) {
    const indexes = slot === "fills" ? [3, 4] : [5];
    const included = new Set(group.indices);
    const reused = old && !model.edges.some((edge, index) =>
        !included.has(index) && indexes.some(field => edge[field] === old));
    const assigned = value ? (reused ? old : model[slot].length) : 0;
    if (value) model[slot][assigned] = value;
    for (const index of group.indices) {
        const edge = model.edges[index];
        if (!old) {
            if (indexes.every(field => !edge[field])) edge[indexes[0]] = assigned;
        } else for (const field of indexes) {
            if (edge[field] === old) edge[field] = assigned;
        }
    }
    return assigned;
}

export function transform(model, group, sx, sy, tx, ty) {
    const { fills, strokes } = paintEdges(model, group);
    const apply = ([x, y]) => [Math.round(x * sx + tx), Math.round(y * sy + ty)];
    const update = paint => {
        const m = paint.matrix, [x, y] = apply([m[4], m[5]]);
        return { ...paint, matrix: [m[0] * sx, m[1] * sy, m[2] * sx, m[3] * sy, x, y] };
    };
    // Paint follows the moved geometry without changing other elements that share its style.
    for (const index of fills.keys()) {
        const paint = model.fills[index];
        if (paint.type) replaceStyle(model, group, "fills", index, update(paint));
    }
    for (const index of strokes.keys()) {
        const stroke = model.strokes[index];
        if (stroke.paint.type) replaceStyle(model, group, "strokes", index, { ...stroke, paint: update(stroke.paint) });
    }
    for (const index of group.indices) {
        const edge = model.edges[index];
        for (const field of [0, 1, 2]) if (edge[field]) edge[field] = apply(edge[field]);
    }
}

export function primitive(tool, from, to, polygon = []) {
    if (tool === "line") return [[from, to, null]];
    if (tool === "ellipse") {
        const x = (from[0] + to[0]) / 2, y = (from[1] + to[1]) / 2;
        const a = Math.abs(to[0] - from[0]) / 2, b = Math.abs(to[1] - from[1]) / 2;
        const point = (theta, scale) => [Math.round(x + a * Math.cos(theta) * scale), Math.round(y + b * Math.sin(theta) * scale)];
        return Array.from({ length: 8 }, (_, i) => {
            const start = i * Math.PI / 4, end = (i + 1) * Math.PI / 4;
            return [point(start, 1), point(end, 1), point((start + end) / 2, 1 / Math.cos(Math.PI / 8))];
        });
    }
    const points = tool === "pen" ? polygon : [from, [to[0], from[1]], to, [from[0], to[1]]];
    return points.map((start, i) => [start, points[(i + 1) % points.length], null]);
}
