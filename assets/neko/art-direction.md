# Neko sprite generation

Tool: OpenAI built-in `imagegen` (not the CLI fallback), 11 November 2026.
Reference: the project owner's attached pixel-art maneki-neko image.
Generated source: `source-atlas.png`; its hash is recorded in `provenance.json`.

## Generation prompt

Use case: precise-object-edit. Asset type: a production pixel-art animation sprite sheet for a tiny ESP32 LCD. Reference/edit target: the attached maneki-neko. Create a clean, precisely aligned 4-column by 2-row sprite sheet, EIGHT equal square cells, transparent background. In each cell reproduce this SAME front-facing seated white calico beckoning cat: huge black eyes with yellow crescent highlights, pink nose, red inner ears and red collar, olive bib, gold bell, gold oval coin with the original black glyph-like markings, orange shoulder patches, crisp black stepped pixel outlines. Preserve the recognizable proportions and expression. Render as meticulous low-resolution pixel art, approximately 72 by 84 logical pixels per cat, enlarged with square nearest-neighbor pixel edges, using a restrained palette of no more than 16 flat colors. Center and align the body identically in EVERY cell; feet, face, ears, coin, tail, collar and all body pixels must stay exactly fixed. ONLY the raised forepaw at viewer-right changes: frames read left-to-right top-to-bottom and show eight progressive poses from fully upright open paw (matching the reference) to a gentle inward/downward beckon, the wrist/forearm folding toward the viewer and lower, ending with its top about 12 logical pixels lower. Keep the elbow/shoulder attachment fixed, anatomically connected and smoothly outlined in every pose. Do not swing the whole cat, do not wave the paw sideways, do not move the other paw. The final frames keep the same three tiny black claw marks, foreshortened naturally. No extra limbs, no blur, no antialiasing, no gradients, no drop shadows, no framing boxes, no titles, no labels, no numerals, no ornamental additions. All eight cells have identical scale, baseline and generous identical clear padding, with no sprite crossing its cell. The output is a clean new sprite rendition derived from the supplied design, without the faint repeated background lettering. Transparent background is real alpha.

## Assembly corrections

The generated sheet is an artwork source, not a trusted animation. It contains
small body variations and faint alpha-edge residue. `tools/build_neko.mjs`
normalizes its cells, freezes the first pose's body outside the forearm region,
removes near-black residue, and maps all poses to one explicit RGB565 palette.
The eight poses are reused with symmetric timing and endpoint holds. No optical
flow, cross-fades, lossy frame interpolation or frame-dependent palettes are used.
