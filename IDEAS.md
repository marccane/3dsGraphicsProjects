# 3DS PICA200 graphics effect ideas (vertex & geometry shaders)

Effects that exploit the PICA200's **programmable vertex and geometry stages** (via citro3d or GLASS).
Grouped by stage, with the hard hardware constraints up front and a status table of what's already
built in this folder.

> The PICA200 has **no programmable fragment stage** — there are no GLSL pixel shaders on the 3DS.
> "Surface" detail comes from the fixed TexEnv combiners + whatever the vertex/geometry stages output.
> So every idea below lives in the **vertex** or **geometry** shader (+ animated uniforms + blending).

---

## Already built (in `9-newGraphicsProjects/`)

| Demo | Stage | Effect | Status |
|---|---|---|---|
| `citro3d-geoshader/` | geometry | 2D rainbow pinwheel; explode each spike along its primitive **centroid** | ✅ done |
| `citro3d-icosahedron-stereo/` | geometry | **stereoscopic** 3D solid blown apart along per-face **normals** (cross product) + lit + glass/dynamic opacity; per-eye off-axis projection, 3D slider drives pop-out | ✅ done |
| `citro3d-cubesphere/` | vertex | subdivided cube ↔ sphere morph via `normalize(pos)` (`rsq`) | ✅ done |
| `citro3d-fur/` | geometry | furry ball — each triangle sprouts a hair along its normal (1→2 tris), bristles + wind sway | ✅ done |
| `citro3d-starfield-stereo/` | geometry | GPU particle **stereoscopic** starfield — each **point** sprouts a camera-facing glowing quad (1→4 tris); additive bloom, per-eye projection, stars at real depths | ✅ done |
| `citro3d-shadow-stereo/` | fixed-function lighting | **hardware shadow mapping** (stereo) — 2-pass: torus rendered to a depth texture from the light's POV, then shadow compare in the lighting unit; orbiting light sweeps a real shadow on a floor | ✅ done |
| `citro3d-shadow-explode-stereo/` | geometry + fixed-function lighting | **exploding icosahedron casting real shadows** (stereo) — GS caster renders the shards' depth from the light; the flat-shaded shards cast an animated shadow on a floor | ✅ done |
| `GLASS-morph/` | vertex (GLASS/ES2) | triangle morphs between two shapes by a uniform | ✅ done |

---

## Geometry-shader effects (per-primitive / amplification)

The geometry stage uniquely sees a *whole primitive* and can *emit more* geometry.

- **3D exploding solid** — per face, compute the face normal (cross product of two edges → needs all 3
  verts) and fly the face out along it. ✅ *Built (`citro3d-icosahedron-stereo`, in stereo).*
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
  snow, a starfield. ✅ *Built (`citro3d-starfield-stereo`): point cloud (stride 2), GS emits each point as
  a camera-facing quad — a 4-triangle fan with a bright centre + zero-alpha rim, so additive blending
  makes a soft round glow; billboard right/up are the camera basis in model space (R^T uniforms, like the
  fur headlight); the field tumbles + Circle-Pad steers; rendered in stereoscopic 3D (see below).*
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
2. ~~**GPU particle starfield**~~ — ✅ done (`citro3d-starfield-stereo`, in stereo).
3. **Wave/flag** — first vertex-shader effect that needs the polynomial-`sin` trick.

See `../8-opengl/3ds-opengl-landscape.md` (§7 examples, §9 gotchas) for build details and the PICA pitfalls.

---

# Appendix A — Full PICA200 hardware feature set (from `pica200.txt`)

The body of this doc is about the PICA200's **programmable** vertex & geometry stages, because that's
what we can freely drive from homebrew. For completeness, this appendix reproduces the **entire** advertised
capability list of the chip (DMP's "MAESTRO-2G" technology) from `pica200.txt`. Most of these are
**fixed-function** or **DMP-proprietary** features — they are *not* programmable pixel shaders (the PICA has
none). Some are reachable from `citro3d`/`libctru` today; some were only ever exposed through DMP's closed
driver and have no open homebrew path.

> **Headline (from the spec blurb):** MAESTRO-2G = OpenGL ES 1.1 + the ES 1.1 extension pack + DMP
> proprietary extensions for *hardware* shading: procedural texturing, BRDF, Cook-Torrance specular
> highlights, **polygon subdivision ("Geo Shader" ≈ tessellation)**, soft-shadow projection, and fake
> subsurface scattering (≈ two-sided lighting). The "Geo Shader" is the very geometry stage every `*geoshader`
> / explode / fur / starfield demo here is built on.

**Reachability legend** (best-effort — verify against the current `citro3d`/`libctru` API before building):
✅ reachable from homebrew today · ⚠️ partial / approximated via the fixed-function lighting LUTs or a
multi-pass trick · ❌ DMP-proprietary, no open path · (untagged = a raw hardware fact, not a feature to drive).

## A.1 Core specs

- 65 nm single core, max clock **400 MHz** · power **0.5–1.0 mW/MHz**.
- Pixel fill: **800 Mpixel/s** (400 Mpixel/s @100 MHz · 1600 Mpixel/s @400 MHz).
- Vertex rate: **15.3 Mpolygon/s @200 MHz** (40 Mtri/s @100 MHz · 160 Mtri/s @400 MHz).
- Frame buffer up to **4095×4095**. Pixel formats: **RGBA8888, RGBA5551, RGB565, RGBA4444**.
- Buffers: **8-bit stencil**, **24-bit depth**, single / double / triple buffering, **PICA-FBM** frame-buffer
  management. *(3DS top screen is 400×240, bottom 320×240.)*

## A.2 Fixed-function pipeline features

- **Vertex program** (ARB_vertex_program) — ✅ this is the PICA vertex shader (`*.v.pica`); used by every demo.
- **Hardware T&L** — ✅ (the vertex shader + matrix uniforms do transform; lighting see A.3).
- **Render to Texture** — ✅ (`C3D_RenderTarget` to a texture) → the basis for any post-process pass below.
- **Polygon subdivision / subdivision primitive** ("Geo Shader" / tessellation) — ✅ via the **geometry
  shader** amplification we already use (`citro3d-fur`, `citro3d-starfield-stereo`). No dedicated tessellator;
  GS is the stand-in.
- **MipMap** — ✅ · **Bilinear texture filtering** — ✅ · **Perspective-correct texture mapping** — ✅
  (automatic) · **Alpha blending** — ✅ (`C3D_AlphaBlend`; the additive glow demos use it).
- **Full-scene anti-aliasing (2×2)** — ⚠️ render at 2× and down-scale on the display transfer
  (`GX_TRANSFER_SCALE_XY`) — supersampling rather than a dedicated MSAA unit.
- **5-stage TEV pipeline** — ✅ (`C3D_TexEnv`, stages 0–5) · **TEV combiner buffer** (only the first four
  stages can *write* it) — ✅ · **Color / Alpha / Texture combiners** — ✅. The TEV is the closest thing to a
  pixel shader: chain combiners for fake lighting, gradients, detail.
- **Dot3 bump / normal mapping** — ⚠️ via a TEV `GPU_DOT3_RGB` stage and/or the lighting unit's bump mode
  (normal-map texture).
- **Phong shading** — ⚠️ · **Cel shading** — ⚠️ via a ramp/LUT — see A.3 (the hardware fragment-lighting unit).
- **Environment / reflection mapping** — ⚠️ cube maps + reflection texcoord generation from the lighting unit.
- **Lightmapping** — ⚠️ (a pre-baked light texture multiplied in via TEV — purely a content/combiner trick).
- **Shadow mapping** — ⚠️ (PICA shadow-texture mode; limited libctru exposure) · **Shadow volumes** — ⚠️
  (stencil-buffer technique) · **Self-shadowing** — ⚠️.
- **Volumetric fog** — ⚠️ (`C3D_FogLut` / fog density; "gaseous object rendering" is the `GPU_GAS` mode,
  rarely exposed).
- **Post-processing — motion blur, bloom, depth of field, HDR, gamma** — ⚠️ all are **multi-pass
  render-to-texture** techniques: bloom/blur/DoF are very doable (bright-pass → blur → composite); true HDR is
  limited (no float framebuffer — only LDR tone-map fakes); gamma via a final LUT pass.
- **Polygon offset** — ✅ (`C3D_DepthMap` z-offset) · **Depth / Stencil / Alpha test** — ✅
  (`C3D_DepthTest` / `C3D_StencilTest` / `C3D_AlphaTest`) · **Clipping, Culling** — ✅ (`C3D_CullFace`,
  automatic clip).

## A.3 DMP MAESTRO-2G proprietary shading

These are the marquee "hardware shading algorithms." On the 3DS they live in the **fixed-function fragment
lighting unit** (driven by lookup tables), not a pixel shader. `citro3d` exposes much of this unit via
`C3D_LightEnv` / `C3D_Light` / `C3D_LightLut` (Phong, specular, fresnel, distribution & reflection LUTs,
spotlights, attenuation) — so several of these are approximable by programming the right LUTs:

- **Per-pixel lighting** — ⚠️ (`C3D_LightEnv` fragment-lighting; the headline reachable one).
- **Bidirectional reflectance distribution function (BRDF)** — ⚠️ via the reflectance LUTs (RR/RG/RB).
- **Cook-Torrance model / specular highlights** — ⚠️ via the distribution LUT (D0/D1) + fresnel.
- **Fake sub-surface scattering** (≈ two-sided lighting) — ⚠️/❌ (fresnel/back-light LUT approximation).
- **Refraction mapping** — ⚠️/❌ (texcoord-perturbation trick; no open recipe).
- **Procedural texture** — ✅ (`C3D_ProcTex` — the PICA's procedural-texture unit *is* exposed by citro3d).
- **Soft shadowing / shadow** — ⚠️/❌ (shadow-texture mode; thin libctru support).
- **Gaseous object rendering** — ❌/⚠️ (`GPU_GAS` density-fog mode; essentially unused in homebrew).
- **Polygon subdivision** — ✅ (the geometry shader, as in A.2).

## A.4 The reachable ones, as buildable demos (next-build candidates)

Cross-referencing A.2/A.3 with what the open stack actually exposes, the standout *new* demos this list
surfaces (beyond the VS/GS ideas in the body) are — **and devkitPro ships working reference code for most
of them** (see A.5, which supersedes the best-effort ⚠️/❌ tags above for those features):

1. **Hardware fragment lighting** — a lit model using `C3D_LightEnv` + a Phong/specular LUT: real per-pixel
   specular highlights *without* a pixel shader. The single best showcase of the DMP lighting unit, and a
   different axis from every current demo (which fake all shading in the VS/GS). ✅ **Verified** —
   `/opt/devkitpro/examples/3ds/graphics/gpu/fragment_light`: `C3D_LightEnvInit/Bind/Material` +
   `C3D_LightInit/Color/Position` + `LightLut_Phong(&lut,30)` + `C3D_LightEnvLut(GPU_LUT_D0,
   GPU_LUTINPUT_LN,…)`; the **vertex shader outputs `normalquat`** (normal encoded as a quaternion) **+
   `view`**, and the TEV **adds** `GPU_FRAGMENT_PRIMARY_COLOR` (diffuse+ambient) + `GPU_FRAGMENT_SECONDARY_COLOR`
   (specular). Material is `{ambient, diffuse, specular0, specular1, emission}`.
2. **Shadow mapping** — real **two-pass hardware** shadow mapping (the genuinely-cool one). ✅ **Built**
   twice: `citro3d-shadow-stereo` (orbiting light, torus self-shadowing + casting onto a floor, tunable
   bias, stereo) and `citro3d-shadow-explode-stereo` (**the exploding icosahedron casting real shadows** —
   a geometry-shader *caster* renders the displaced shards' depth from the light, so the flat-shaded
   shards throw an animated shadow on the floor; the GS only outputs position/colour, so the unverified
   "GS → fragment-lighting" path is sidestepped — only the floor uses the lighting unit). Based on
   `/opt/devkitpro/examples/3ds/graphics/gpu/shadow_mapping`: **pass 1** renders the caster from the light's
   POV into a **depth texture** (`C3D_TexInitShadow(&tex,512,512)` + `C3D_RenderTargetCreateFromTex(&tex,
   GPU_TEXFACE_2D, 0, GPU_RB_DEPTH16)`, light uses an ortho `Mtx_LookAt`/`Mtx_Ortho`); **pass 2** renders
   from the camera with the shadow compare done **in the lighting unit** (`C3D_LightShadowEnable`,
   `C3D_LightEnvShadowMode(GPU_SHADOW_PRIMARY)`, `C3D_LightEnvShadowSel(0)`, `C3D_TexShadowParams(false,
   bias)`; shadow map bound on texunit 0, `GPU_CLAMP_TO_BORDER` + white border so off-map = lit). Receiver
   VS computes the shadow-map UV from a `light_viewproj` uniform. Tunable `bias` kills shadow acne. This
   **upgrades the A.3 "soft shadowing / shadow" tag from ⚠️/❌ to ✅.**
3. **Post-process bloom** — render the scene to a texture, bright-pass + separable blur passes, composite
   additively. Makes the existing glow demos (starfield, exploding solid) genuinely bloom. Pure
   render-to-texture; no new shader stage needed. (No single stock example, but `shadow_mapping` proves the
   render-to-texture path and `mipmap_fog` the multi-pass plumbing.)
4. **Procedural texture** (`C3D_ProcTex`) — animated noise/marble/wood from the hardware proctex unit. ✅
   example: `.../gpu/proctex`.
5. **Cube-map reflection / environment mapping** — a chrome object reflecting a cube map. ✅ example:
   `.../gpu/cubemap`.
6. **Normal / bump mapping** (Dot3) — ✅ example: `.../gpu/normal_mapping`.
7. **Toon / cel shading** — ✅ example: `.../gpu/toon_shading`.

Fragment lighting and shadow mapping are the standouts (and have the cleanest reference code). Note **every
`gpu/` example is stereoscopic** (`gfxSet3D(true)` + dual GFX_LEFT/GFX_RIGHT targets + `Mtx_PerspStereoTilt`)
— exactly our existing stereo pattern, so they slot straight into this project.

## A.5 devkitPro stock examples (`/opt/devkitpro/examples/3ds/graphics/gpu/`)

The SDK ships runnable examples that prove most of the appendix features are reachable — invaluable reference
code (and a reason **not** to reinvent any of these from scratch). The ones that map to this doc:

| Example | Proves (appendix feature) |
|---|---|
| `fragment_light` | hardware per-pixel/Phong/specular lighting (A.3 per-pixel lighting, Cook-Torrance-ish) |
| `shadow_mapping` | 2-pass shadow mapping via depth texture + lighting-unit compare (A.3 shadow / soft shadowing) |
| `cubemap` | environment / reflection mapping (A.2) |
| `normal_mapping` | Dot3 bump / normal mapping (A.2) |
| `toon_shading` | cel shading (A.2) |
| `proctex` | procedural texture (A.3) |
| `mipmap_fog` | mipmaps + volumetric fog (A.2) |
| `loop_subdivision` | geometry-shader polygon subdivision / tessellation stand-in (A.2 — same stage as our demos) |
| `particles`, `gpusprites` | point/sprite particle systems (cf. our `citro3d-starfield-stereo`) |
| `geoshader` | the stock geometry-shader example our `citro3d-geoshader` was modelled on |
| `stereoscopic_2d`, `wide_mode_3d` | stereo plumbing (cf. our `*-stereo` demos) |
| `composite_scene`, `both_screens`, `immediate`, `lenny` | multi-pass / both-screen / immediate-mode / classic refs |
