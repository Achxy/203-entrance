import { env } from "cloudflare:workers";
export function GET() {
    const e = env as unknown as Record<string, string>;
    const key = e.CLERK_PUBLISHABLE_KEY || "";
    let host = "";
    try {
        host = atob(key.split("_").slice(2).join("_")).replace(/\$$/, "");
    } catch {}
    if (!/^[a-z0-9.-]+$/.test(host)) host = "";
    return Response.json(
        {
            clerk_key: host ? key : "",
            clerk_script: host
                ? `https://${host}/npm/@clerk/clerk-js@6.42.3/dist/clerk.browser.js`
                : "",
            clerk_ui: host
                ? `https://${host}/npm/@clerk/ui@1.42.4/dist/ui.browser.js`
                : "",
            relay_url: /^wss:\/\/[a-z0-9.-]+\/v1\/player$/.test(
                e.ARCADE_RELAY_URL || "",
            )
                ? e.ARCADE_RELAY_URL
                : "",
            device_ready: e.ARCADE_DEVICE_READY === "true",
        },
        { headers: { "Cache-Control": "no-store" } },
    );
}
