"use client";
import { useEffect, useState } from "react";
import {
    loadClerk,
    type ClerkClient,
    type PublicConfig,
} from "../../lib/clerk-client";
type Visit = {
    id: string;
    name: string;
    room: string;
    verification: string;
    received_at: number;
    clerk_sync: string;
};
export default function OwnerLog() {
    const [clerk, setClerk] = useState<ClerkClient | null>(null),
        [signed, setSigned] = useState(false),
        [rows, setRows] = useState<Visit[]>([]),
        [busy, setBusy] = useState(false),
        [error, setError] = useState(""),
        [loaded, setLoaded] = useState(false);
    useEffect(() => {
        let cancel = false,
            remove: (() => void) | undefined;
        fetch("/api/config")
            .then((r) => r.json() as Promise<PublicConfig>)
            .then(loadClerk)
            .then((c) => {
                if (cancel) return;
                setClerk(c);
                setSigned(Boolean(c.user));
                remove = c.addListener(() => setSigned(Boolean(c.user)));
            })
            .catch(() =>
                setError(
                    "Sign-in is unavailable. Reconnect to the Internet and reload.",
                ),
            );
        return () => {
            cancel = true;
            remove?.();
        };
    }, []);
    async function refresh() {
        setBusy(true);
        setError("");
        try {
            const token = await clerk?.session?.getToken();
            const res = await fetch("/api/visits", {
                headers: { Authorization: "Bearer " + token },
            });
            const data = (await res.json()) as {
                message: string;
                visits: Visit[];
            };
            if (!res.ok) throw new Error(data.message);
            setRows(data.visits);
            setLoaded(true);
        } catch (e) {
            setError(
                e instanceof Error
                    ? e.message
                    : "Could not load the private log.",
            );
        } finally {
            setBusy(false);
        }
    }
    return (
        <main className="owner-log">
            <a href="/">← Back to the arcade</a>
            <p className="eyebrow">ROOM 203 · OWNER ACCESS</p>
            <h1>The guest book.</h1>
            <p>
                This page is private to Achyuth’s verified Google account.
                Refresh also retries pending Clerk records and removes a small
                batch of expired visits.
            </p>
            {!signed ? (
                <button
                    className="primary"
                    disabled={!clerk}
                    onClick={() =>
                        clerk?.openSignIn({
                            forceRedirectUrl: location.origin + "/log",
                        })
                    }
                >
                    Sign in with Google
                </button>
            ) : (
                <div className="log-actions">
                    <button
                        className="primary"
                        disabled={busy}
                        onClick={refresh}
                    >
                        {busy ? "Checking records…" : "Refresh guest book"}
                    </button>
                    <button
                        className="text-button"
                        onClick={() => void clerk?.signOut()}
                    >
                        Sign out
                    </button>
                </div>
            )}
            {error && (
                <p role="alert" className="error">
                    {error}
                </p>
            )}
            {loaded && (
                <>
                    <p>
                        {rows.length} recent visits · last 90 days · up to 100
                        shown
                    </p>
                    <div
                        className="log-table"
                        tabIndex={0}
                        role="region"
                        aria-label="Guest visits"
                    >
                        <table>
                            <thead>
                                <tr>
                                    <th>Received</th>
                                    <th>Name</th>
                                    <th>Room</th>
                                    <th>Identity</th>
                                    <th>Clerk</th>
                                    <th>Visit ID</th>
                                </tr>
                            </thead>
                            <tbody>
                                {rows.map((r) => (
                                    <tr key={r.id}>
                                        <td>
                                            {new Date(
                                                r.received_at * 1000,
                                            ).toLocaleString()}
                                        </td>
                                        <td>{r.name || "Not shared"}</td>
                                        <td>{r.room || "Not shared"}</td>
                                        <td>
                                            {r.verification === "unverified"
                                                ? "Unverified guest"
                                                : "Signed Google pass"}
                                        </td>
                                        <td>
                                            {r.clerk_sync === "done"
                                                ? "Saved"
                                                : "Retry pending"}
                                        </td>
                                        <td>
                                            <code>{r.id}</code>
                                        </td>
                                    </tr>
                                ))}
                            </tbody>
                        </table>
                        {!rows.length && <p>No visits have been synced yet.</p>}
                    </div>
                </>
            )}
            <p className="muted">
                Room numbers are self-reported. “Received” is when the phone
                synced its device-signed receipt, not a verified arrival time.
                Guests can play offline; their receipts may arrive later.
            </p>
        </main>
    );
}
