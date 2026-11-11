import { registerHooks } from "node:module";
import { readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import ts from "typescript";
registerHooks({
    resolve(specifier, context, next) {
        if (specifier === "cloudflare:workers")
            return {
                url: new URL("./worker-env.mjs", import.meta.url).href,
                shortCircuit: true,
            };
        if (specifier.startsWith(".") && context.parentURL) {
            const url = new URL(specifier, context.parentURL);
            if (
                !/\.[a-z]+$/.test(url.pathname) &&
                existsSync(fileURLToPath(url) + ".ts")
            )
                return { url: url.href + ".ts", shortCircuit: true };
        }
        return next(specifier, context);
    },
    load(url, context, next) {
        if (url.endsWith(".ts"))
            return {
                format: "module",
                shortCircuit: true,
                source: ts.transpileModule(
                    readFileSync(fileURLToPath(url), "utf8").replaceAll(
                        "import.meta.env.DEV",
                        "false",
                    ),
                    {
                        compilerOptions: {
                            target: ts.ScriptTarget.ES2022,
                            module: ts.ModuleKind.ESNext,
                        },
                    },
                ).outputText,
            };
        return next(url, context);
    },
});
