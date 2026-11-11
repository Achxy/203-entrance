import {
    body,
    config,
    identity,
    response,
    failure,
    rate,
    ApiError,
} from "../../../lib/security";
import { receipt } from "../../../lib/receipt";
import {
    syncVisit,
    maintainVisits,
    retryVisits,
} from "../../../lib/visit-store";
export async function POST(request: Request) {
    try {
        const data = await body(request);
        await rate(request, "receipt", 60, 3600);
        const r = await receipt(data.receipt),
            e = config();
        const old = await e.DB.prepare(
            "SELECT receipt_hash FROM visits WHERE id=?",
        )
            .bind(r.id)
            .first<{ receipt_hash: string }>();
        if (old && old.receipt_hash !== r.receiptHash)
            throw new ApiError(
                409,
                "A different receipt already uses this visit ID.",
            );
        if (!old) {
            // Physical admission, not web traffic, gates this global creation budget.
            await rate(request, "new-visits", 240, 86400, true);
            const count = await e.DB.prepare(
                "SELECT COUNT(*) AS n FROM visits",
            ).first<{ n: number }>();
            if ((count?.n || 0) >= 2000)
                throw new ApiError(
                    503,
                    "The private log needs owner maintenance. Your receipt remains on your phone.",
                );
            await e.DB.prepare(
                "INSERT INTO visits(id,name,room,clerk_user_id,verification,device,received_at,receipt_hash,receipt) VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO NOTHING",
            )
                .bind(
                    r.id,
                    r.name,
                    r.room,
                    r.clerkUserId,
                    r.verification,
                    r.device,
                    r.receivedAt,
                    r.receiptHash,
                    r.receipt,
                )
                .run();
            const saved = await e.DB.prepare(
                "SELECT receipt_hash FROM visits WHERE id=?",
            )
                .bind(r.id)
                .first<{ receipt_hash: string }>();
            if (saved?.receipt_hash !== r.receiptHash)
                throw new ApiError(
                    409,
                    "A different receipt already uses this visit ID.",
                );
        }
        const synced = await syncVisit(r.id);
        return response({
            ok: true,
            visit_id: r.id,
            verification: r.verification,
            clerk_sync: synced ? "done" : "pending",
        });
    } catch (e) {
        return failure(e);
    }
}
export async function GET(request: Request) {
    try {
        const e = config(),
            user = await identity(request);
        if (
            !user ||
            !user.email_addresses?.some(
                (x) =>
                    x.verification?.status === "verified" &&
                    x.email_address === e.ARCADE_OWNER_EMAIL,
            )
        )
            throw new ApiError(
                403,
                "This log is private to the Room 203 owner.",
            );
        await maintainVisits();
        await retryVisits();
        const rows = await e.DB.prepare(
            "SELECT id,name,room,verification,received_at,device,clerk_sync FROM visits WHERE received_at>? ORDER BY received_at DESC LIMIT 100",
        )
            .bind(Math.floor(Date.now() / 1000) - 90 * 86400)
            .all();
        return response({ visits: rows.results });
    } catch (e) {
        return failure(e);
    }
}
