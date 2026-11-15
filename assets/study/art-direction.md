# Study paw artwork

Tool: OpenAI built-in imagegen, followed by deterministic source-pixel
conversion and animation in `tools/build_study.mjs`. The owner requested
maximum retention of the supplied cat's expression, with pixelation only.

`reference.png` is the owner's supplied reference. `clean-chest.png` is the
selected generated inpainting output. Only its pixels behind the original
foreground paw are used. The face, eyes, ears, body outside that patch and
moving paw all come from the original reference. No generated face is used.
Original and generated input hashes, palette, exact transform parameters and
every RGB565 frame hash are in `provenance.json`.

The initial photographic and redesigned pixel-art atlases were rejected
because they changed the expression. They are not production assets.

## Final built-in tool prompt

Use case: precise-object-edit. This is a strict edit of the supplied image, NOT a new character design. Keep the exact original cat, its expression, facial proportions, gaze, eye sizes, markings, ears, mouth, and photographic texture UNCHANGED. Keep original framing, background, size and position. Change ONLY the bottom foreground paw: remove the large raised paw that occupies the bottom center and fill that small area naturally with matching chest fur. Keep the head and all the upper two-thirds exactly as in the reference, absolutely no cuteness or stylization changes, no enlarged eyes, no changed ears, no smile. No new pose. This image will be used as a static background underneath a separately animated copy of the original paw; the original face pixels will be retained in software. Do not pixelate in this step. No text, no new objects.

## Pixel and motion review

All 36 frames were inspected in the contact sheet, including the reach,
reversal, attachment at the bottom edge and the first/last seam. The fixed
body and original face do not drift. All three final LCD caption layouts,
including the four Japanese glyphs, were inspected. Actual physical LCD
appearance remains a separate check.

The final motion places the paw eight native landscape LCD pixels to the viewer's
right of center, with a small periodic sideways component during the reach.
