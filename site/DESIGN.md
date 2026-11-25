# Documentation design — monochrome revision

## Direction

The reference direction is Vercel-like restraint: neutral black and white, thin layout rules, a large centered headline, compact controls, and generous negative space. This replaces the earlier cream-and-blue technical-manual direction. It is not a reproduction of Vercel's logo or a claim of affiliation.

Geometry comes from the page structure rather than colored ornaments. The homepage is English; existing Japanese articles and their navigation remain Japanese. Article translation is not part of this visual revision.

## Google Fonts

- [Geist](https://fonts.google.com/specimen/Geist), weights 400–700: the main Latin interface and headline family. Its neutral proportions, compact spacing, and clear small-size rendering support a typography-first developer site without the more expressive forms of Space Grotesk. Use 600 for the large hero, 550 for section headings, and 400–500 for supporting copy and controls.
- [Geist Mono](https://fonts.google.com/specimen/Geist+Mono), weights 400 and 500: commands, execution-node labels, and small metadata. It shares the main family's character while preserving alignment and differentiating code from prose.
- [Noto Sans JP](https://fonts.google.com/noto/specimen/Noto+Sans+JP), weights 400–700: Japanese article fallback. The homepage does not use Japanese display lettering; long Japanese documents retain a purpose-built reading font.

All three are loaded from Google Fonts with `display=swap`, preconnects to the stylesheet and font origins, and system fallbacks. No font package is added to the dependency tree.

## Layout decisions

| Element | Rule | Reason |
| --- | --- | --- |
| Global palette | Pure white / near-black backgrounds; neutral gray secondary text; no blue accents | Establish the requested monochrome character. Hierarchy comes from contrast and size, not unrelated colors. Dark mode uses the same neutral hierarchy. |
| Landing header | Brand at left, Docs / Benchmarks / GitHub next, search and actions at right; 64px height | Separate identity, destinations, and utilities. The compact header leaves the main visual emphasis to the headline. The custom x-shaped mark is specific to xyo, not Vercel's triangle. |
| Article header | Keep Starlight's existing functional header | Preserve article search, theme controls, and mobile navigation without introducing a second navigation implementation for long-form documents. |
| Outer frame | Maximum 75rem, centered; continuous 1px left and right rules | Align the hero, diagram, navigation cells, commands, and data to a single width. This is the primary geometric system. |
| Single header | No secondary xyo / Documentation strip under the global navigation | Remove the duplicated-header impression and give the hero the first content position. Brand and navigation already provide the necessary context. |
| Hero | Centered single-column headline, 48–96px responsive type; description and actions below | Lead with “Ultrafast.” at the strongest contrast, then identify the product as “Native Scratch.” Supporting copy names LLVM JIT, AOT, and parallel execution. The benchmark CTA supports the speed positioning without inventing a universal speedup or claiming every project beats TurboWarp. |
| Hero rails | Two very faint vertical lines at 25% and 75% | Continue the page's structural geometry through its negative space. They are nonsemantic decoration, never an alternative to meaningful content. |
| Engine badge | Small outlined pill above the headline | State the implementation context before the claim. It links to the actual execution guide instead of pretending to be a release announcement. |
| Primary action | Filled neutral button beneath the description | Make the next step obvious. A neighboring outlined benchmark link offers evidence for the speed positioning at deliberately lower emphasis. Small 6px radii distinguish controls from structural rules. |
| Hero footnote | Small text directly beneath the actions | Clarify that existing SB3 / SB2 files are the input, without lengthening the headline or implying universal compatibility. |
| Execution diagram | Wide horizontal flow below the hero; project left, xyo center, execution modes right | Let visitors read the main claim first, then inspect the technical relationship. Three outgoing branches show available paths, not an automatic sequence. The guide explains mode limitations. |
| Diagram nodes | Subtle outlined nodes and a high-contrast central xyo node | Keep the runtime as the visual focal point; the connecting lines encode actual relationships rather than decorative circuitry. Use equal-size execution nodes for peer choices. |
| Diagram caption | Small centered caption beneath the flow | Place the compatibility goal next to the technical model. SVG title and description provide the same relationship to assistive technology. |
| Platform row | A compact horizontal row after the execution diagram | Answer deployment questions in one glance. WebAssembly's interpreter limitation is adjacent to its name, not hidden in a distant note. |
| Documentation cells | Three equal columns with functional line icons, one title, description, and link | Organize the reading journey as getting started → using execution modes → understanding internals. Equal widths communicate peer navigation choices rather than performance rankings. Remove arbitrary square/circle/triangle category decorations. |
| Cell icons and numbers | Icon top left, sequence number top right | Offer quick recognition and reading order while leaving the heading and description as the actual accessible meaning. Icons are decorative. |
| Command section | Explanatory copy left, terminal right; equal columns | Pair an action-oriented explanation with real commands. A restrained 8px terminal radius and header identify it as executable syntax rather than another navigation card. |
| Terminal prompts | Nonselectable `$` with command text in monospace | Preserve familiar terminal conventions without adding shell prompts to copied command text. Comments identify the three use cases. |
| Tool strip | Compact row directly after the command section | Keep auxiliary CLI tools discoverable without giving each one a large marketing card. A single CLI reference link provides the details. |
| Benchmarks | Full-width section after the introductory content, within the same frame | Keep real measured results available without making first-time visitors parse a table before they understand the runtime. Do not introduce invented headline performance numbers. |
| Benchmark table | Existing data and calculations; English labels only when `locale="en"` | Preserve the CI data pipeline and Japanese article behavior. Unbroken cells scroll horizontally inside the table on narrow screens. |
| Bottom row | Identity left, implementation metadata right | Close the shared frame and provide a quiet endpoint, rather than an additional call-to-action competing with the primary one. |
| Cross marks | Small plus signs only at section/frame intersections | Mark shared alignment intersections. They do not appear as free-floating visual noise and are excluded from accessibility output. |
| Article sidebars | Left document navigation, central reading column (max 46rem), right page outline | Keep cross-document navigation distinct from within-page navigation. Long text must not inherit the homepage's wider measure. |
| Article hierarchy | Neutral headings, modest inline code backgrounds, understated selected navigation | Make the same design usable for actual documentation, not just the landing screen. Current location is indicated by weight, background, and an inset rule. |

Spacing uses an 8px scale for major sections (16 / 24 / 32 / 48 / 56 / 72). Fine adjustments in type and control padding are independent of the layout scale. Structural borders stay square; interactive controls and the terminal may use modest radii.

## Responsive and accessible behavior

- Below 50rem, documentation cells and command columns stack in their original reading order. The hero remains centered, with responsive type rather than forced desktop line lengths.
- On small screens, the landing header retains the brand, Docs link, and search. Secondary header destinations remain accessible through the content. Article pages retain Starlight's mobile menu and theme controls.
- The execution SVG retains an accessible title and description on desktop. Below 50rem it is replaced by a vertical HTML flow with readable labels, rather than shrinking a 1000px diagram into illegible text. Textual guide links provide the detailed alternative.
- Focus-visible outlines use 2px high-contrast rules. Hover never reveals essential information.
- Decorative icons, rails, plus signs, and shell prompts do not carry unique information.
- Reduced-motion preference disables entrance, hover, pulse, float, and pointer-tilt motion. No animation is required to understand the page.
- Existing search, page outline, theme switching, and article pagination remain functional.

## Animation and 3D

Motion follows the existing information structure rather than adding another competing visual layer:

| Motion | Timing and placement | Reason |
| --- | --- | --- |
| Hero entrance | 750ms ease-out; badge, headline lines, explanation, actions, footnote staggered over 420ms | Guide the eye in reading order without delaying the whole interface or changing layout. Only opacity and a small vertical transform animate. |
| Scroll entrance | 650ms opacity / 18px translation; each section reveals once at an 8% intersection threshold; navigation cards stagger by 70ms | Make section boundaries legible while preserving the page's shared geometry. Focus within a section immediately exposes its content. |
| Execution pulses | 5s repeating sequence along the existing SVG connections | Illustrate project → runtime → possible execution paths. This is a conceptual diagram, not live performance telemetry; pulses do not imply execution modes run simultaneously. |
| Runtime model | Six CSS 3D planes, centered on the existing runtime node; 6s float and 9s gentle turn | Make the runtime tangible as a block, connecting the Scratch input concept to the engine. It replaces the flat central node rather than becoming an unrelated hero ornament. |
| Pointer tilt | At most ±10° X and ±16° Y, on fine hover-capable pointers only | Let visitors inspect the model without taking over scrolling, requiring a drag, or interfering with links. Touch input never tilts it. |
| Button and card feedback | 2–4px transform, 200–250ms | Confirm interactive targets without moving the surrounding layout or creating large parallax effects. |

The model is procedural HTML/CSS geometry, not a downloaded GLB or an external branded asset. It requires no Three.js, model download, WebGL context, texture, or continuous JavaScript rendering loop. Six faces and subtle monochrome lighting retain the visual direction. Mobile uses a smaller instance in the vertical execution diagram.

The **Pause motion** button sits in the single global header, outside the animated areas. On mobile it becomes an accessible icon button; no second toolbar or documentation strip is introduced. It pauses the repeating loops, exposes all section content, disables pointer tilt, and stores the preference locally when storage is available. OS reduced-motion preference takes precedence and labels the disabled control **Reduced motion**. Hidden tabs and offscreen execution diagrams pause their continuous CSS animations. A disconnected controller releases observers and event listeners.

Without JavaScript, content stays visible, controls that cannot work stay hidden, and the original static runtime node remains. With reduced motion, a static 3D model is still available, but no motion is necessary. The decorative model is aria-hidden; the diagram's accessible title and description continue to explain the underlying relationship.

## Recorded LLVM IR stream

The execution section now pairs its conceptual 3D runtime with a real compiler artifact. The stream sits immediately below the diagram: a compact explanation on the left and the recorded IR on the right. This links the visual engine to its actual output without putting moving text behind the main headline or adding a second header.

The asset comes from running the existing LLVM-enabled `build/xyo` against `tests/bench/algos/out/micro_loop_1000000.sb3`, using `XYO_JIT_DUMP2=1`, `--jit=eager`, and `--no-mega`. `site/scripts/capture-llvm-ir.mjs` captures stderr and extracts a complete numeric-loop basic block verbatim. The full unmodified module is published at `ir/micro-loop.ll`; the adjacent provenance JSON records the command, time, project hash, binary hash, capture-time repository commit, IR hash, and excerpt line range. The binary hash identifies the actual executable; the repository revision alone is not a claim about its build provenance.

This is explicitly **captured output / replay**, not a live browser compiler or fabricated terminal session. Regenerate deliberately with `node site/scripts/capture-llvm-ir.mjs`; documentation builds use the committed snapshot and do not require the native runtime. A changed IR shape fails capture for review instead of silently substituting made-up code.

The visual copy loops upward slowly with a soft top/bottom mask. It reuses the global Pause motion control, stops outside the viewport or in hidden tabs, and also pauses on hover or keyboard focus. Reduced-motion preference displays a static excerpt. The duplicated scrolling copy is aria-hidden and excluded from search indexing; a separate expandable, keyboard-scrollable static excerpt preserves all original whitespace for reading and copying. Full IR and capture details remain linked next to the description. On mobile the explanation and stream stack within the same outer grid.

## Validation

Build with `cd site && ./node_modules/.bin/astro build`. For visual-only validation, do not run the sync script that deletes and regenerates article directories: preserve the current generated articles and other agents' changes. The normal package build script is unchanged.

Browser validation covers the homepage, getting-started article, and benchmark article at 320 / 390 / 768 / 1024 / 1440px in both themes; primary links, search, and the article mobile menu are checked separately. Browser screenshots are produced from the built static output without launching a replacement server or changing the DSH GUI.
