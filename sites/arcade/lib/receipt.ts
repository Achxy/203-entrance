import {
    config,
    ApiError,
    unb64,
    field,
    verifyPass,
    hash,
    jsonObject,
} from "./security";
function derToRaw(der: Uint8Array) {
    let at = 2;
    if (der[0] !== 48 || der[1] + 2 !== der.length)
        throw new ApiError(400, "Invalid device signature.");
    const out = new Uint8Array(64);
    for (let i = 0; i < 2; i++) {
        if (der[at++] !== 2)
            throw new ApiError(400, "Invalid device signature.");
        let n = der[at++];
        // DER integers must be positive and minimally encoded.
        if (
            !n ||
            at + n > der.length ||
            der[at] & 128 ||
            (n > 1 && der[at] === 0 && !(der[at + 1] & 128))
        )
            throw new ApiError(400, "Invalid device signature.");
        if (n === 33 && der[at] === 0) {
            at++;
            n--;
        }
        if (n > 32 || !n || at + n > der.length)
            throw new ApiError(400, "Invalid device signature.");
        out.set(der.slice(at, at + n), i * 32 + 32 - n);
        at += n;
    }
    if (at !== der.length) throw new ApiError(400, "Invalid device signature.");
    return out;
}
export async function receipt(value: unknown) {
    if (typeof value !== "string" || value.length > 2200)
        throw new ApiError(400, "Invalid visit receipt.");
    const e = config(),
        parts = value.split(".");
    if (parts.length !== 3 || parts[0] !== "R1" || !e.DEVICE_PUBLIC_KEY)
        throw new ApiError(400, "This device receipt is not supported.");
    const bytes = unb64(parts[1], 1500),
        sig = derToRaw(unb64(parts[2], 80));
    const der = Uint8Array.from(
        atob(e.DEVICE_PUBLIC_KEY.replace(/-----[^-]+-----|\s/g, "")),
        (c) => c.charCodeAt(0),
    );
    const key = await crypto.subtle.importKey(
        "spki",
        der,
        { name: "ECDSA", namedCurve: "P-256" },
        false,
        ["verify"],
    );
    const input = new TextEncoder().encode(
        "203-arcade-visit-v1\n" + new TextDecoder().decode(bytes),
    );
    if (
        !(await crypto.subtle.verify(
            { name: "ECDSA", hash: "SHA-256" },
            key,
            sig,
            input,
        ))
    )
        throw new ApiError(
            400,
            "This receipt was not signed by the Room 203 display.",
        );
    const r = jsonObject(bytes);
    if (
        r.v !== 1 ||
        r.device !== "entrance-c7f088" ||
        typeof r.visit !== "string" ||
        !/^[a-f0-9]{32}$/.test(r.visit) ||
        typeof r.profile !== "string"
    )
        throw new ApiError(400, "Invalid device visit.");
    let profile;
    if (r.profile.startsWith("E1.")) profile = await verifyPass(r.profile);
    else if (r.profile.startsWith("G1."))
        profile = jsonObject(unb64(r.profile.slice(3), 400));
    else throw new ApiError(400, "Invalid guest profile.");
    const google =
        r.profile.startsWith("E1.") &&
        profile.google === true &&
        typeof profile.sub === "string" &&
        profile.sub.startsWith("user_");
    return {
        id: r.visit,
        name: field(profile.name, 32, !google),
        room: field(profile.room, 16, !google),
        clerkUserId: google ? profile.sub : null,
        verification: google ? "google_account_pass" : "unverified",
        device: r.device,
        receiptHash: await hash(value),
        receipt: value,
        receivedAt: Math.floor(Date.now() / 1000),
    };
}
