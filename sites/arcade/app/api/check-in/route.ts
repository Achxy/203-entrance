import {
    body,
    field,
    identity,
    signPass,
    response,
    failure,
    rate,
} from "../../../lib/security";
export async function POST(request: Request) {
    try {
        const data = await body(request);
        await rate(request, "check-in", 12, 3600);
        if (data.consent !== true)
            return response(
                { message: "Consent to the private visitor log is required." },
                400,
            );
        const user = await identity(request),
            name = field(data.name, 32, !user),
            room = field(data.room, 16, !user);
        const pass = {
            v: 1,
            id: crypto.randomUUID().replace(/-/g, ""),
            name,
            room,
            sub: user?.id || "",
            google: Boolean(user),
            iat: Math.floor(Date.now() / 1000),
            exp: Math.floor(Date.now() / 1000) + 28800,
        };
        return response({
            claim: await signPass(pass),
            verified: Boolean(user),
        });
    } catch (e) {
        return failure(e);
    }
}
