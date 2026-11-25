import assert from "node:assert/strict";
import { bounds, components, fillPath, primitive, solid, transform } from "../src/FlashStudio/Editor/shape-geometry.js";

const paint = { ...solid(), type: 16 };
const makeBox = (x, y, width, height) => primitive("rectangle", [x, y], [x + width, y + height])
    .map(edge => [...edge, 1, 0, 0, 0]);
const model = { fills: [solid(), paint], strokes: [null], edges: [
    ...makeBox(0, 0, 100, 100), ...makeBox(25, 25, 25, 25), ...makeBox(200, 200, 100, 100)
] };
assert.deepEqual(components(model.edges).map(group => group.indices.length), [8, 4]);
transform(model, components(model.edges)[0], 1, 1, 30, 50);
assert.equal(model.edges[0][3], 2);
assert.deepEqual(model.fills[2].matrix.slice(4), [30, 50]);
assert.deepEqual(model.fills[1].matrix.slice(4), [0, 0]);
assert.deepEqual(bounds(model.edges.slice(0, 8)), [30, 50, 130, 150]);
assert.deepEqual(bounds(model.edges.slice(8)), [200, 200, 300, 300]);
const ellipse = primitive("ellipse", [0, 0], [40, 30]);
assert.equal(ellipse.length, 8);
assert.deepEqual(ellipse[0][0], ellipse.at(-1)[1]);
assert.equal((fillPath(ellipse).match(/Q/g) ?? []).length, 8);
assert.ok(fillPath(ellipse).endsWith("Z"));
console.log("OK — courbes, trous et déplacement des remplissages sans modifier les styles partagés");
