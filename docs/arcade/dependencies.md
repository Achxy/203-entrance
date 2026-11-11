# Website dependency review — 12 November 2026

The initial starter lockfile audit reported 33 findings, including critical
findings for Next.js and React Server Components. This is an advisory inventory,
not proof that every advisory has an exploitable path in this application.

The website was updated to exact pins for React/React DOM/React Server DOM
19.3.1, Next.js 16.4.6, Vinext 1.4.5, Vite 8.3.8, Cloudflare's Vite plugin
1.69.0, Wrangler 4.160.1 and Zod 4.6.7. Compatible transitive fixes were applied
without `--force`, major-version overrides or dependency downgrades. The
connector-preview record schemas were adapted to Zod 4 and their strict
envelope behavior checked.

Primary upstream advisories reviewed:

- [React Server Components patch guidance](https://github.com/react/react/security/advisories/GHSA-xqc2-xf4c-x3h3).
- [Next.js image-optimization dependency patch](https://github.com/vercel/next.js/security/advisories/GHSA-2464-fhr4-w9xw).

After compatible fixes, the audit reports **zero high or critical findings**
and **15 moderate findings**, including transitive chains. The remaining
findings involve Drizzle's development loader/esbuild, TypeScript ESLint's
minimatch chain and the optional Satori/fflate image-generation chain. The
application exposes no untrusted image-generation input and does not run an
esbuild development server in production. These scope observations are not a
general exemption: update these chains when compatible upstream fixes exist,
and re-review before adding image generation or exposing development tools.
Do not use the audit's proposed major downgrades to hide the remaining findings.

The patched application passed its eight backend tests, TypeScript checking,
the Zod compatibility check and the production Worker build. A scan of the
built files found no matches for the configured Clerk secret, signing-key
private scalar or rate-limit salt. This focused check is not a comprehensive
security audit. Raw npm reports remain private under `.local/arcade/`.
