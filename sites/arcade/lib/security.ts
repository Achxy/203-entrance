import { env } from "cloudflare:workers";
import { createRemoteJWKSet, jwtVerify } from "jose";
export function config() {
    return env as unknown as Record<string, string> & { DB: D1Database };
}
export const origin = "https://203.at.achyuth.dev";
export function allowedOrigin(value: string | null) {
    return (
        value === origin ||
        value === "https://room-203-arcade.sid381788.chatgpt.site" ||
        (import.meta.env.DEV &&
            /^http:\/\/(localhost|127\.0\.0\.1):\d+$/.test(value || ""))
    );
}
export function response(data: unknown, status = 200) {
    return Response.json(data, {
        status,
        headers: {
            "Cache-Control": "no-store",
            "X-Content-Type-Options": "nosniff",
            "Referrer-Policy": "no-referrer",
        },
    });
}
export class ApiError extends Error {
    constructor(
        public status: number,
        message: string,
    ) {
        super(message);
    }
}
export async function body(request: Request) {
    if (!allowedOrigin(request.headers.get("Origin")))
        throw new ApiError(403, "Open this form from the arcade website.");
    if (!request.headers.get("Content-Type")?.startsWith("application/json"))
        throw new ApiError(415, "Send a JSON request.");
    const reader = request.body?.getReader();
    if (!reader) throw new ApiError(400, "Missing request.");
    const parts: Uint8Array[] = [];
    let size = 0;
    try {
        while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            size += value.length;
            if (size > 4096) {
                await reader.cancel();
                throw new ApiError(413, "Request is too large.");
            }
            parts.push(value);
        }
    } finally {
        reader.releaseLock();
    }
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const p of parts) {
        bytes.set(p, offset);
        offset += p.length;
    }
    try {
        const data = JSON.parse(new TextDecoder().decode(bytes));
        if (!data || Array.isArray(data) || typeof data !== "object") throw 0;
        return data;
    } catch {
        throw new ApiError(400, "Invalid request.");
    }
}
export function field(value: unknown, max: number, required = false) {
    if (
        typeof value !== "string" ||
        new TextEncoder().encode(value.trim()).length > max ||
        /[\x00-\x1f\x7f]/.test(value)
    )
        throw new ApiError(
            400,
            "Name or room is too long or contains unsupported characters.",
        );
    const s = value.trim();
    if (required && !s)
        throw new ApiError(400, "Please enter both your name and room number.");
    return s;
}
export function b64(bytes: Uint8Array) {
    let s = "";
    for (const b of bytes) s += String.fromCharCode(b);
    return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
export function jsonObject(bytes: Uint8Array) {
    try {
        const value = JSON.parse(
            new TextDecoder("utf-8", { fatal: true }).decode(bytes),
        );
        if (!value || Array.isArray(value) || typeof value !== "object")
            throw 0;
        return value;
    } catch {
        throw new ApiError(400, "Invalid signed payload.");
    }
}
export function unb64(s: string, max = 2048) {
    if (
        typeof s !== "string" ||
        s.length > max * 2 ||
        !/^[A-Za-z0-9_-]+$/.test(s)
    )
        throw new ApiError(400, "Invalid receipt.");
    try {
        const bytes = Uint8Array.from(
            atob(s.replace(/-/g, "+").replace(/_/g, "/")),
            (c) => c.charCodeAt(0),
        );
        if (bytes.length > max) throw new Error("Oversized payload");
        return bytes;
    } catch {
        throw new ApiError(400, "Invalid receipt.");
    }
}
export async function hash(s: string) {
    return b64(
        new Uint8Array(
            await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s)),
        ),
    );
}
export async function rate(
    request: Request,
    scope: string,
    limit: number,
    seconds: number,
    global = false,
) {
    const e = config();
    if (!e.DB)
        throw new ApiError(
            503,
            "The visitor log is temporarily unavailable. You can still use guest check-in at the display.",
        );
    const now = Math.floor(Date.now() / 1000),
        bucket = Math.floor(now / seconds);
    const ip = global
        ? "all"
        : request.headers.get("cf-connecting-ip") || "unknown";
    const key =
        scope +
        ":" +
        (await hash((e.RATE_LIMIT_SALT || "203-arcade") + ip + bucket));
    const row = await e.DB.prepare(
        "INSERT INTO rate_limits(key,count,expires_at) VALUES (?,1,?) ON CONFLICT(key) DO UPDATE SET count=count+1 RETURNING count",
    )
        .bind(key, now + seconds * 2)
        .first<{ count: number }>();
    if (!row || row.count > limit)
        throw new ApiError(
            429,
            "Too many requests. Please wait before trying again.",
        );
    await e.DB.prepare("DELETE FROM rate_limits WHERE expires_at < ?")
        .bind(now)
        .run();
}
let keyset: ReturnType<typeof createRemoteJWKSet> | undefined,
    issuer = "";
export async function identity(request: Request) {
    const bearer = request.headers.get("Authorization");
    if (!bearer) return null;
    if (!bearer.startsWith("Bearer ") || bearer.length > 4096)
        throw new ApiError(401, "Please sign in again.");
    const e = config();
    if (!e.CLERK_SECRET_KEY || !e.CLERK_PUBLISHABLE_KEY)
        throw new ApiError(503, "Google sign-in is not ready yet.");
    let host = "";
    try {
        host = atob(
            e.CLERK_PUBLISHABLE_KEY.split("_").slice(2).join("_"),
        ).replace(/\$$/, "");
    } catch {}
    if (!/^[a-z0-9.-]+$/.test(host))
        throw new ApiError(503, "Identity configuration is unavailable.");
    const expected = "https://" + host;
    if (issuer !== expected) {
        issuer = expected;
        keyset = createRemoteJWKSet(
            new URL(expected + "/.well-known/jwks.json"),
        );
    }
    try {
        const { payload } = await jwtVerify(bearer.slice(7), keyset!, {
            issuer: expected,
            algorithms: ["RS256"],
            clockTolerance: 5,
        });
        if (!payload.sub || !allowedOrigin(String(payload.azp || ""))) throw 0;
        const r = await fetch(
            "https://api.clerk.com/v1/users/" + encodeURIComponent(payload.sub),
            {
                headers: {
                    Authorization: "Bearer " + e.CLERK_SECRET_KEY,
                    "User-Agent": "203-Arcade/1.0",
                },
                signal: AbortSignal.timeout(8000),
            },
        );
        if (!r.ok) throw 0;
        const user = (await r.json()) as {
            id: string;
            external_accounts?: Array<{
                provider: string;
                verification?: { status: string };
            }>;
            email_addresses?: Array<{
                email_address: string;
                verification?: { status: string };
            }>;
            first_name?: string;
            last_name?: string;
        };
        if (!user.external_accounts?.some((x) => x.provider === "oauth_google"))
            throw 0;
        return user;
    } catch {
        throw new ApiError(
            401,
            "Your Google session could not be verified. Please sign in again.",
        );
    }
}
export async function signPass(data: unknown) {
    const e = config();
    if (!e.ARCADE_PASS_KEY)
        throw new ApiError(503, "Visit passes are not configured yet.");
    const bytes = new TextEncoder().encode(JSON.stringify(data));
    const key = await crypto.subtle.importKey(
        "jwk",
        JSON.parse(e.ARCADE_PASS_KEY),
        { name: "ECDSA", namedCurve: "P-256" },
        false,
        ["sign"],
    );
    const input = new TextEncoder().encode(
        "203-arcade-pass-v1\n" + new TextDecoder().decode(bytes),
    );
    const sig = await crypto.subtle.sign(
        { name: "ECDSA", hash: "SHA-256" },
        key,
        input,
    );
    return "E1." + b64(bytes) + "." + b64(new Uint8Array(sig));
}
export async function verifyPass(pass: string) {
    const e = config();
    const parts = pass.split(".");
    if (parts.length !== 3 || parts[0] !== "E1")
        throw new ApiError(400, "Invalid signed pass.");
    const bytes = unb64(parts[1], 512),
        sig = unb64(parts[2], 64);
    const jwk = JSON.parse(e.ARCADE_PASS_KEY);
    delete jwk.d;
    jwk.key_ops = ["verify"];
    const key = await crypto.subtle.importKey(
        "jwk",
        jwk,
        { name: "ECDSA", namedCurve: "P-256" },
        false,
        ["verify"],
    );
    const input = new TextEncoder().encode(
        "203-arcade-pass-v1\n" + new TextDecoder().decode(bytes),
    );
    if (
        !(await crypto.subtle.verify(
            { name: "ECDSA", hash: "SHA-256" },
            key,
            sig,
            input,
        ))
    )
        throw new ApiError(400, "The identity pass is not authentic.");
    const p = jsonObject(bytes);
    if (
        p.v !== 1 ||
        typeof p.id !== "string" ||
        !/^[a-f0-9]{32}$/.test(p.id) ||
        !Number.isSafeInteger(p.iat) ||
        !Number.isSafeInteger(p.exp) ||
        p.iat < 0 ||
        p.exp <= p.iat ||
        p.exp - p.iat > 8 * 3600
    )
        throw new ApiError(400, "Invalid identity pass.");
    return p;
}
export function failure(e: unknown) {
    return response(
        {
            message:
                e instanceof ApiError
                    ? e.message
                    : "This service is temporarily unavailable. Please try again.",
        },
        e instanceof ApiError ? e.status : 503,
    );
}
