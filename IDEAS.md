# 3DS PICA200 graphics effect ideas (vertex & geometry shaders)

Effects that exploit the PICA200's **programmable vertex and geometry stages** (via citro3d or GLASS).
Grouped by stage, with the hard hardware constraints up front and a status table of what's already
built in this folder.

> The PICA200 has **no programmable fragment stage** — there are no GLSL pixel shaders on the 3DS.
> "Surface" detail comes from the fixed TexEnv combiners + whatever the vertex/geometry stages output.
> So every idea below lives in the **vertex** or **geometry** shader (+ animated uniforms + blending).

---

## Already built (in `1-graphicsProjects/`)

| Demo | Stage | Effect | Status |
|---|---|---|---|
| `citro3d-geoshader/` | geometry | 2D rainbow pinwheel; explode each spike along its primitive **centroid** | ✅ done |
| `citro3d-icosahedron/` | geometry | 3D solid blown apart along per-face **normals** (cross product) + lit + glass/dynamic opacity | ✅ done |
| `citro3d-icosahedron-stereo/` | geometry | the exploding icosahedron in **stereoscopic 3D** — per-eye off-axis projection, 3D slider drives pop-out | ✅ done |
| `citro3d-cubesphere/` | vertex | subdivided cube ↔ sphere morph via `normalize(pos)` (`rsq`) | ✅ done |
| `citro3d-fur/` | geometry | furry ball — each triangle sprouts a hair along its normal (1→2 tris), bristles + wind sway | ✅ done |
| `citro3d-starfield/` | geometry | GPU particle starfield — each **point** sprouts a camera-facing glowing quad (1→4 tris), additive bloom | ✅ done |
| `citro3d-starfield-stereo/` | geometry | the particle starfield in **stereoscopic 3D** — per-eye projection, stars at real depths | ✅ done |
| `GLASS-morph/` | vertex (GLASS/ES2) | triangle morphs between two shapes by a uniform | ✅ done |

---

## Geometry-shader effects (per-primitive / amplification)

The geometry stage uniquely sees a *whole primitive* and can *emit more* geometry.

- **3D exploding solid** — per face, compute the face normal (cross product of two edges → needs all 3
  verts) and fly the face out along it. ✅ *Built (`citro3d-icosahedron`).*
- **Centroid burst / pinwheel** — push each triangle out along its centroid `(v0+v2+v4)/3`. ✅ *Built
  (`citro3d-geoshader`).*
- **Fur / spikes / fins** — each triangle emits itself **plus** a tapered hair along its normal, so the
  shape grows fur; animate hair length + a wind vector. ✅ *Built (`citro3d-fur`): 1 input tri → base
  surface tri + hair spike; bristles and sways; **live, uncapped D-pad subdivision** spawns more/fewer
  hairs by reallocating + regenerating the sphere VBO (grows until linear memory runs out).*
- **Instant wireframe / neon outline** — emit each triangle's 3 edges as thin quads (PICA lines have no
  width); pair with additive blending for a glowing wireframe, or animate an edge "scan."
- **Tumbling debris field** — like the explode, but give each shard its **own** rotation from a
  per-primitive seed + time, so faces tumble independently instead of moving rigidly outward.
- **GPU particles from points** — feed a point cloud; GS emits a camera-facing quad per point → sparks,
  snow, a starfield. ✅ *Built (`citro3d-starfield`): point cloud (stride 2), GS emits each point as a
  camera-facing quad — a 4-triangle fan with a bright centre + zero-alpha rim, so additive blending makes
  a soft round glow; billboard right/up are the camera basis in model space (R^T uniforms, like the fur
  headlight); the field tumbles + Circle-Pad steers.*
- **Poor-man's tessellation** — the PICA has no tessellation stage, so GS amplification is the stand-in:
  subdivide a coarse grid and displace the new verts → animated water/terrain from a tiny input mesh.
- **Shatter-into-shards** — subdivide each face (like the stock geoshader's 1→3) *and* explode the
  pieces, so the surface fractures into smaller shards as it blows apart.

## Vertex-shader effects (per-vertex deformation)

- **Cube ↔ sphere morph** — blend each vertex toward `normalize(pos)` (uses `rsq`). ✅ *Built
  (`citro3d-cubesphere`).*
- **N-shape morph** — blend a vertex between two (or more) stored positions by uniform weights. ✅
  *Built for 2 shapes (`GLASS-morph`); extend to N shape keys.*
- **Wave / flag / jelly** — displace verts by a travelling wave from a `time` uniform (flag, water,
  wobble). Needs a polynomial `sin` approximation (~6 instructions; no native `sin` opcode).
- **Twist / bend** — rotate each vertex by an angle proportional to its height → twisting column, DNA
  helix. (Also needs a `sin`/`cos` approximation per vertex.)
- **Spherical "breathing"** — push verts along their normals by `amp·wave` (needs a normal attribute).
- **Plasma vertex colour** — compute colour from position + time via dot products → flowing gradients.

## Combiner & output tricks (no fragment shader, so lean on these)

- **Additive blending** → glow/bloom; emit a scaled-up dim copy of each shard for a halo. (Colours stay
  vivid over a dark background — additive adds light instead of fading toward black.)
- **Animated TexEnv constant colour** via a uniform → pulsing/cycling glow synced to the motion.
- **Vertex-colour × texture** multi-stage combiners → fake lighting, gradients, detail.

## 3DS-specific angles

- **Stereoscopic 3D** — render with a per-eye offset projection so exploding shards literally pop out of
  the screen. The genuinely-3DS "wow". ✅ *Built (`citro3d-icosahedron-stereo`): `gfxSet3D(true)`, two
  render targets (GFX_LEFT/GFX_RIGHT), `Mtx_PerspStereoTilt` per eye with the zero-parallax plane at the
  object distance, `osGet3DSliderState()` driving the interocular offset; right eye skipped when the
  slider is at 0.*
- **Both screens** + touch/circle-pad/slider to drive bloom, rotation, opacity, or eye separation live.
  (The icosahedron already uses the bottom screen for a text HUD — could go interactive.)

---

## Hardware constraints that shape all of the above

- **No fragment/pixel shaders.** Surface detail = combiners + vertex/geometry output + textures.
- **No `sin`/`cos` opcode.** For *global* rotation, compute the matrix on the CPU and pass it as a
  uniform (cheap — what every demo here does). For *per-vertex* angles (waves, twist), use a
  ~6-instruction polynomial sine approximation in the shader.
- **One input register per instruction.** Reading two attributes at once (e.g. `posB − posA`) needs one
  copied to a temp first.
- **Constant/uniform operand must be src1** for `mul`/`dp3`/`dp4`/`add`/`max`/`min`.
- **GS output budget is small** — keep amplification to a handful of triangles per input primitive.
- **Depth is reversed** — clear depth to 0 and use `GPU_GREATER` (not `GPU_LESS`, which gives a black
  screen). Geoshader uniforms live on `GPU_GEOMETRY_SHADER`.

## Suggested next builds (impact-per-effort)

1. ~~**Stereoscopic-3D explode**~~ — ✅ done (`citro3d-icosahedron-stereo`).
2. ~~**GPU particle starfield**~~ — ✅ done (`citro3d-starfield`).
3. **Wave/flag** — first vertex-shader effect that needs the polynomial-`sin` trick.
4. ~~**Stereo starfield**~~ — ✅ done (`citro3d-starfield-stereo`).

See `../3ds-opengl-landscape.md` (§7 examples, §9 gotchas) for build details and the PICA pitfalls.
