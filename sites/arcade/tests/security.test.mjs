import { test, beforeEach, afterEach } from "node:test";
import assert from "node:assert/strict";
import { DatabaseSync } from "node:sqlite";
import { readFileSync } from "node:fs";
import { env } from "./worker-env.mjs";
import {
    body,
    field,
    b64,
    unb64,
    rate,
    signPass,
    verifyPass,
} from "../lib/security.ts";
import { receipt } from "../lib/receipt.ts";
import { syncVisit, maintainVisits } from "../lib/visit-store.ts";
import { POST, GET } from "../app/api/visits/route.ts";
let sql,
    device,
    originalFetch = globalThis.fetch;
const id = "a".repeat(32),
    other = "b".repeat(32);
const request = (payload, origin = "https://203.at.achyuth.dev") =>
    new Request("https://203.at.achyuth.dev/api/visits", {
        method: "POST",
        headers: {
            Origin: origin,
            "Content-Type": "application/json",
            "cf-connecting-ip": "192.0.2.1",
        },
        body: JSON.stringify(payload),
    });
function rawDer(raw) {
    const ints = [];
    for (let i = 0; i < 64; i += 32) {
        let n = Buffer.from(raw.slice(i, i + 32));
        while (n.length > 1 && n[0] === 0) n = n.subarray(1);
        if (n[0] & 128) n = Buffer.concat([Buffer.from([0]), n]);
        ints.push(Buffer.concat([Buffer.from([2, n.length]), n]));
    }
    const content = Buffer.concat(ints);
    return Buffer.concat([Buffer.from([48, content.length]), content]);
}
async function signedReceipt(profile, extra = {}) {
    const payload = JSON.stringify({
        v: 1,
        device: "entrance-c7f088",
        visit: id,
        profile,
        ...extra,
    });
    const sig = await crypto.subtle.sign(
        { name: "ECDSA", hash: "SHA-256" },
        device.privateKey,
        new TextEncoder().encode("203-arcade-visit-v1\n" + payload),
    );
    return (
        "R1." +
        b64(new TextEncoder().encode(payload)) +
        "." +
        b64(rawDer(new Uint8Array(sig)))
    );
}
const guest = (fields = {}) =>
    "G1." +
    b64(
        new TextEncoder().encode(
            JSON.stringify({ name: "Test visitor", room: "203", ...fields }),
        ),
    );
function insert(
    visit = id,
    verification = "unverified",
    user = null,
    received = Math.floor(Date.now() / 1000),
) {
    sql.prepare(
        "INSERT INTO visits(id,name,room,clerk_user_id,verification,device,received_at,receipt_hash,receipt) VALUES(?,?,?,?,?,?,?,?,?)",
    ).run(
        visit,
        "Test visitor",
        "203",
        user,
        verification,
        "entrance-c7f088",
        received,
        visit,
        "test",
    );
}
beforeEach(async () => {
    sql = new DatabaseSync(":memory:");
    for (const f of ["0000_loose_leper_queen.sql", "0001_sleepy_the_order.sql"])
        sql.exec(
            readFileSync(new URL("../drizzle/" + f, import.meta.url), "utf8"),
        );
    Object.keys(env).forEach((k) => delete env[k]);
    env.DB = {
        prepare(query) {
            return {
                bind(...args) {
                    const s = sql.prepare(query);
                    return {
                        first: async () => s.get(...args) || null,
                        all: async () => ({ results: s.all(...args) }),
                        run: async () => s.run(...args),
                    };
                },
                first: async () => sql.prepare(query).get() || null,
                all: async () => ({ results: sql.prepare(query).all() }),
                run: async () => sql.prepare(query).run(),
            };
        },
    };
    env.RATE_LIMIT_SALT = "disposable-test-salt";
    const key = await crypto.subtle.generateKey(
        { name: "ECDSA", namedCurve: "P-256" },
        true,
        ["sign", "verify"],
    );
    env.ARCADE_PASS_KEY = JSON.stringify(
        await crypto.subtle.exportKey("jwk", key.privateKey),
    );
    device = await crypto.subtle.generateKey(
        { name: "ECDSA", namedCurve: "P-256" },
        true,
        ["sign", "verify"],
    );
    env.DEVICE_PUBLIC_KEY =
        "-----BEGIN PUBLIC KEY-----\n" +
        Buffer.from(
            await crypto.subtle.exportKey("spki", device.publicKey),
        ).toString("base64") +
        "\n-----END PUBLIC KEY-----";
});
afterEach(() => {
    globalThis.fetch = originalFetch;
    sql.close();
});
test("cross-origin, malformed JSON, Unicode limits and oversized requests are rejected", async () => {
    await assert.rejects(
        body(request({}, "https://evil.example")),
        (e) => e.status === 403,
    );
    await assert.rejects(
        body(request({ x: "a".repeat(4096) })),
        (e) => e.status === 413,
    );
    assert.throws(
        () => field("é".repeat(17), 32),
        (e) => e.status === 400,
    );
    assert.throws(() => field("  ", 32, true));
    assert.throws(() => field("a\nb", 32));
    assert.throws(() => unb64(b64(new Uint8Array(65)), 64));
    assert.equal(field(" Alice ", 32, true), "Alice");
});
test("signed identity passes reject tampering and unreasonable lifetimes", async () => {
    const pass = await signPass({
        v: 1,
        id,
        iat: 100,
        exp: 100 + 28800,
        google: true,
        sub: "user_test",
        name: "",
        room: "",
    });
    assert.equal((await verifyPass(pass)).sub, "user_test");
    const parts = pass.split(".");
    parts[1] = b64(
        new TextEncoder().encode(
            JSON.stringify({
                v: 1,
                id,
                iat: 100,
                exp: 200,
                google: true,
                sub: "user_attacker",
            }),
        ),
    );
    await assert.rejects(verifyPass(parts.join(".")));
    await assert.rejects(
        verifyPass(await signPass({ v: 1, id, iat: 100, exp: 100 + 28801 })),
    );
});
test("device receipts authenticate the device and never trust guest verification claims", async () => {
    const r = await receipt(
        await signedReceipt(
            guest({ google: true, sub: "user_owner", verified: true }),
        ),
    );
    assert.equal(r.verification, "unverified");
    assert.equal(r.clerkUserId, null);
    await assert.rejects(
        receipt(await signedReceipt(guest(), { device: "some-other-board" })),
    );
    await assert.rejects(
        receipt(
            await signedReceipt("G1." + b64(new TextEncoder().encode("null"))),
        ),
        (e) => e.status === 400,
    );
    const good = await signedReceipt(guest());
    const parts = good.split(".");
    const der = unb64(parts[2]);
    der[5] ^= 1;
    parts[2] = b64(der);
    await assert.rejects(receipt(parts.join(".")));
    const pass = await signPass({
        v: 1,
        id,
        iat: 100,
        exp: 200,
        google: true,
        sub: "user_test",
        name: "",
        room: "",
    });
    assert.equal(
        (await receipt(await signedReceipt(pass))).verification,
        "google_account_pass",
    );
});
test("rate budgets are atomic and have a separate global creation cap", async () => {
    const results = await Promise.allSettled(
        Array.from({ length: 9 }, () => rate(request({}), "check", 3, 60)),
    );
    assert.equal(results.filter((x) => x.status === "fulfilled").length, 3);
    await rate(request({}), "new", 1, 60, true);
    await assert.rejects(
        rate(request({}), "new", 1, 60, true),
        (e) => e.status === 429,
    );
});
test("repeat receipts are idempotent and owner log is private", async () => {
    const value = await signedReceipt(guest());
    for (let i = 0; i < 2; i++) {
        const r = await POST(request({ receipt: value }));
        assert.equal(r.status, 200);
        assert.equal((await r.json()).clerk_sync, "pending");
    }
    assert.equal(sql.prepare("SELECT COUNT(*) n FROM visits").get().n, 1);
    assert.equal(
        (
            await POST(
                request({
                    receipt: await signedReceipt(guest({ name: "Changed" })),
                }),
            )
        ).status,
        409,
    );
    assert.equal(
        (await GET(new Request("https://203.at.achyuth.dev/api/visits")))
            .status,
        403,
    );
    assert.equal((await POST(request({ receipt: "forged" }))).status, 400);
});
test("Clerk sync recovers a lost create response without duplicate users", async () => {
    env.CLERK_SECRET_KEY = "test";
    insert();
    let stored = null,
        creates = 0;
    globalThis.fetch = async (url, opts) => {
        if (opts.method === "GET") return Response.json(stored ? [stored] : []);
        assert.equal(opts.method, "POST");
        creates++;
        const payload = JSON.parse(opts.body);
        assert.equal(payload.external_id, "203-guest-" + id);
        assert.equal(payload.password, undefined);
        assert.equal(payload.email_address, undefined);
        assert.equal(payload.banned, undefined);
        stored = {
            id: "user_guest",
            external_id: payload.external_id,
            email_addresses: [],
            phone_numbers: [],
            external_accounts: [],
        };
        throw new Error("response lost");
    };
    assert.equal(await syncVisit(id), false);
    assert.equal(await syncVisit(id), true);
    assert.equal(creates, 1);
    assert.equal(
        sql.prepare("SELECT clerk_user_id FROM visits").get().clerk_user_id,
        "user_guest",
    );
});
test("concurrent Clerk sync only acquires one row lease", async () => {
    env.CLERK_SECRET_KEY = "test";
    insert();
    let creates = 0;
    globalThis.fetch = async (url, opts) => {
        if (opts.method === "GET") return Response.json([]);
        creates++;
        await new Promise((resolve) => setTimeout(resolve, 5));
        return Response.json({ id: "user_guest" });
    };
    await Promise.all(Array.from({ length: 8 }, () => syncVisit(id)));
    assert.equal(creates, 1);
});
test("retention removes only identity-free guest records and preserves Google accounts", async () => {
    env.CLERK_SECRET_KEY = "test";
    const old = Math.floor(Date.now() / 1000) - 91 * 86400;
    insert(id, "unverified", null, old);
    insert(other, "google_account_pass", "user_real", old);
    const deleted = [],
        patched = [];
    globalThis.fetch = async (url, opts) => {
        if (url.includes("users?"))
            return Response.json([
                { id: "user_guest", external_id: "203-guest-" + id },
            ]);
        if (opts.method === "DELETE") {
            deleted.push(url);
            return Response.json({ deleted: true });
        }
        if (opts.method === "PATCH") {
            patched.push(url);
            return Response.json({});
        }
        return Response.json(
            url.endsWith("user_guest")
                ? {
                      id: "user_guest",
                      external_id: "203-guest-" + id,
                      email_addresses: [],
                      phone_numbers: [],
                      external_accounts: [],
                  }
                : {
                      id: "user_real",
                      email_addresses: [{}],
                      private_metadata: { room203: { last_visit: other } },
                  },
        );
    };
    await maintainVisits();
    assert.equal(deleted.length, 1);
    assert.ok(deleted[0].endsWith("user_guest"));
    assert.equal(patched.length, 1);
    assert.equal(sql.prepare("SELECT COUNT(*) n FROM visits").get().n, 0);
});
