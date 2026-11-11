import { config } from "./security";

type VisitRow = {
    id: string;
    name: string;
    room: string;
    verification: string;
    clerk_user_id: string | null;
    received_at: number;
};
type ClerkUser = {
    id: string;
    external_id?: string;
    email_addresses?: unknown[];
    phone_numbers?: unknown[];
    external_accounts?: unknown[];
    private_metadata?: { room203?: { last_visit?: string } };
};
async function clerk(path: string, method = "GET", payload?: unknown) {
    const e = config();
    return fetch("https://api.clerk.com/v1/" + path, {
        method,
        headers: {
            Authorization: "Bearer " + e.CLERK_SECRET_KEY,
            "Content-Type": "application/json",
            "User-Agent": "203-Arcade/1.0",
        },
        body: payload === undefined ? undefined : JSON.stringify(payload),
        signal: AbortSignal.timeout(6000),
    });
}
export async function syncVisit(id: string) {
    const e = config();
    if (!e.CLERK_SECRET_KEY) return false;
    const now = Math.floor(Date.now() / 1000);
    const row = await e.DB.prepare(
        "UPDATE visits SET sync_started=? WHERE id=? AND clerk_sync='pending' AND sync_started<? RETURNING id,name,room,verification,clerk_user_id,received_at",
    )
        .bind(now, id, now - 30)
        .first<VisitRow>();
    if (!row)
        return (
            (
                await e.DB.prepare("SELECT clerk_sync FROM visits WHERE id=?")
                    .bind(id)
                    .first<{ clerk_sync: string }>()
            )?.clerk_sync === "done"
        );
    try {
        let userId = row.clerk_user_id;
        if (row.verification === "unverified") {
            const external = "203-guest-" + row.id;
            // The unique external ID makes a retry safe even if a prior create succeeded
            // but its network response was lost. Guest records have no sign-in identifiers.
            const found = await clerk(
                "users?external_id=" +
                    encodeURIComponent(external) +
                    "&limit=1",
            );
            if (!found.ok) return false;
            const list = (await found.json()) as ClerkUser[];
            userId = list.find((x) => x.external_id === external)?.id || null;
            if (!userId) {
                const created = await clerk("users", "POST", {
                    external_id: external,
                    first_name: row.name,
                    skip_password_requirement: true,
                    private_metadata: {
                        room203: {
                            kind: "guest_record",
                            visit: row.id,
                            name: row.name,
                            room: row.room,
                            verified: false,
                            received_at: row.received_at,
                        },
                    },
                });
                if (!created.ok) return false;
                userId = ((await created.json()) as ClerkUser).id;
            }
        } else {
            if (!userId) return false;
            const saved = await clerk(
                "users/" + encodeURIComponent(userId) + "/metadata",
                "PATCH",
                {
                    private_metadata: {
                        room203: {
                            last_visit: row.id,
                            last_received_at: row.received_at,
                            name: row.name,
                            room: row.room,
                            room_verified: false,
                        },
                    },
                },
            );
            if (!saved.ok) return false;
        }
        await e.DB.prepare(
            "UPDATE visits SET clerk_sync='done',clerk_user_id=? WHERE id=?",
        )
            .bind(userId, id)
            .run();
        return true;
    } catch {
        return false;
    } finally {
        await e.DB.prepare("UPDATE visits SET sync_started=0 WHERE id=?")
            .bind(id)
            .run();
    }
}
export async function maintainVisits() {
    const e = config(),
        cutoff = Math.floor(Date.now() / 1000) - 90 * 86400;
    // Bounded maintenance; stale rows are hidden immediately even if Clerk is offline.
    const old = await e.DB.prepare(
        "SELECT id,verification,clerk_user_id FROM visits WHERE received_at<? ORDER BY received_at LIMIT 3",
    )
        .bind(cutoff)
        .all<Pick<VisitRow, "id" | "verification" | "clerk_user_id">>();
    for (const row of old.results) {
        if (row.verification === "unverified" && !row.clerk_user_id) {
            if (!e.CLERK_SECRET_KEY) continue;
            // Recover a create whose successful response was lost before expiry.
            const found = await clerk(
                "users?external_id=" +
                    encodeURIComponent("203-guest-" + row.id) +
                    "&limit=1",
            );
            if (!found.ok) continue;
            const users = (await found.json()) as ClerkUser[];
            row.clerk_user_id =
                users.find((x) => x.external_id === "203-guest-" + row.id)
                    ?.id || null;
        }
        if (row.clerk_user_id) {
            if (!e.CLERK_SECRET_KEY) continue;
            const r = await clerk(
                "users/" + encodeURIComponent(row.clerk_user_id),
            );
            if (r.status !== 404) {
                if (!r.ok) continue;
                const user = (await r.json()) as ClerkUser;
                if (
                    row.verification === "unverified" &&
                    user.external_id === "203-guest-" + row.id &&
                    !user.email_addresses?.length &&
                    !user.phone_numbers?.length &&
                    !user.external_accounts?.length
                ) {
                    const deleted = await clerk(
                        "users/" + encodeURIComponent(user.id),
                        "DELETE",
                    );
                    if (!deleted.ok && deleted.status !== 404) continue;
                } else if (
                    user.private_metadata?.room203?.last_visit === row.id
                ) {
                    const cleared = await clerk(
                        "users/" + encodeURIComponent(user.id) + "/metadata",
                        "PATCH",
                        { private_metadata: { room203: null } },
                    );
                    if (!cleared.ok) continue;
                }
            }
        }
        await e.DB.prepare("DELETE FROM visits WHERE id=?").bind(row.id).run();
    }
}
export async function retryVisits() {
    const rows = await config()
        .DB.prepare(
            "SELECT id FROM visits WHERE clerk_sync='pending' AND received_at>? ORDER BY received_at LIMIT 3",
        )
        .bind(Math.floor(Date.now() / 1000) - 90 * 86400)
        .all<{ id: string }>();
    for (const row of rows.results) await syncVisit(row.id);
}
