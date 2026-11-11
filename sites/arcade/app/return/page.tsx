"use client";
import { useEffect, useState } from "react";
export default function Return() {
    const [status, setStatus] = useState("Saving your visit…"),
        [failed, setFailed] = useState(false);
    async function save() {
        setFailed(false);
        try {
            const receipt = sessionStorage.getItem("203-receipt");
            if (!receipt) {
                setStatus("Thanks for dropping by.");
                return;
            }
            const r = await fetch("/api/visits", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ receipt }),
            });
            const body = (await r.json()) as { message: string };
            if (!r.ok) throw new Error(body.message);
            sessionStorage.removeItem("203-receipt");
            setStatus("Your visit is saved. Thanks for playing!");
        } catch {
            setStatus(
                "Your visit is still on the display. Reconnect to the Internet and try again.",
            );
            setFailed(true);
        }
    }
    useEffect(() => {
        const p = new URLSearchParams(location.hash.slice(1));
        const receipt = p.get("receipt");
        if (receipt && receipt.length <= 2200)
            sessionStorage.setItem("203-receipt", receipt);
        history.replaceState(null, "", location.pathname);
        save();
    }, []);
    return (
        <main className="prose">
            <a href="/">← 203 Arcade</a>
            <h1>
                See you
                <br />
                next round.
            </h1>
            <p role="status">{status}</p>
            {failed && (
                <button className="primary" onClick={save}>
                    Try saving again ↗
                </button>
            )}
            <p>The lucky cat has the door covered.</p>
        </main>
    );
}
