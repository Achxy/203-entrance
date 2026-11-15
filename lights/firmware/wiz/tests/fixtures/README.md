# Reviewed model response

The ESP25_SHRGB_01 / firmware 1.38.0 model fixture was promoted from the
sanitized response obtained through hub 1.1.0 during the room's read-only
diagnostic session on 2026-11-16. It contains the model-configuration response,
not the private export or the full diagnostic. Inspection confirmed that it has
no device MAC, IP, home/room identity, credential, token, or signing key.

The unchanged C parser accepts the complete nested response within its existing
2048-byte and 192-token bounds. With the separately reported, exact known module
profile it permits 2200–6500 K and brightness 10–100. The test uses a synthetic
system envelope and an existing test MAC to combine those two pieces of evidence.
The same model body alone does not grant RGB/CCT features for an unknown module.

The original cctRange is [2200, 2700, 6500, 6500]. The reported nowc=1 and
hasGradient=1 are retained as model evidence; they do not introduce gradient,
zone, or additional white-channel control. Scenes remain the existing curated
class profile, not physically validated effects. env="pro" does not establish
response authenticity, and no successful bulb write is claimed by this fixture.
