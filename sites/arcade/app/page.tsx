"use client";
import { useEffect, useRef, useState } from "react";

import {
    loadClerk,
    type ClerkClient,
    type PublicConfig,
} from "../lib/clerk-client";

const games = [
    {
        name: "Tetris",
        tag: "ONE PLAYER",
        color: "mint",
        copy: "Find your rhythm. Make a little space.",
        keys: "Move · rotate · drop",
    },
    {
        name: "Pong",
        tag: "ONE OR TWO",
        color: "red",
        copy: "Bring a friend. Or challenge the house.",
        keys: "Slide · rally · repeat",
    },
    {
        name: "Pac-Man",
        tag: "ONE PLAYER",
        color: "yellow",
        copy: "Small maze. Big appetite. Familiar company.",
        keys: "Turn · chomp · escape",
    },
];
const WIFI = { ssid: "203 IoTitty", password: "neko-203-play" };
export default function Home() {
    const [step, setStep] = useState(0),
        [name, setName] = useState(""),
        [room, setRoom] = useState(""),
        [error, setError] = useState(""),
        [busy, setBusy] = useState(false),
        [clerk, setClerk] = useState<ClerkClient | null>(null),
        [signed, setSigned] = useState(false),
        [claim, setClaim] = useState(""),
        [consent, setConsent] = useState(false),
        [copied, setCopied] = useState(false),
        [connected, setConnected] = useState(false),
        [ready, setReady] = useState(false);
    const panel = useRef<HTMLElement>(null);
    useEffect(() => {
        if (location.hash === "#join") setStep(1);
        let cancel = false,
            remove: (() => void) | undefined;
        let previousUser = "";
        fetch("/api/config")
            .then((r) => r.json() as Promise<PublicConfig>)
            .then(async (cfg) => {
                if (cancel) return;
                setReady(Boolean(cfg.device_ready));
                const c = await loadClerk(cfg);
                if (cancel) return;
                setClerk(c);
                const sync = () => {
                    setSigned(Boolean(c.user));
                    if (c.user && previousUser !== c.user.id)
                        setName(
                            (c.user.fullName || c.user.firstName || "").slice(
                                0,
                                32,
                            ),
                        );
                    previousUser = c.user?.id || "";
                };
                sync();
                remove = c.addListener(sync);
            })
            .catch(() => {});
        return () => {
            cancel = true;
            remove?.();
        };
    }, []);
    useEffect(() => {
        if (step)
            panel.current?.scrollIntoView({
                behavior: matchMedia("(prefers-reduced-motion: reduce)").matches
                    ? "instant"
                    : "smooth",
                block: "start",
            });
    }, [step]);
    async function prepare(e: React.FormEvent) {
        e.preventDefault();
        setError("");
        if (!signed && (!name.trim() || !room.trim()))
            return setError(
                "Add your name and room number to continue as a guest.",
            );
        if (!consent)
            return setError("Please agree to the private visitor log first.");
        if (!signed) {
            const json = JSON.stringify({
                name: name.trim(),
                room: room.trim(),
                verified: false,
            });
            if (
                new TextEncoder().encode(name.trim()).length > 32 ||
                new TextEncoder().encode(room.trim()).length > 16
            ) {
                setError("Please use a shorter name or room number.");
                return;
            }
            setClaim(
                "G1." +
                    btoa(unescape(encodeURIComponent(json)))
                        .replace(/\+/g, "-")
                        .replace(/\//g, "_")
                        .replace(/=+$/, ""),
            );
            setStep(2);
            return;
        }
        setBusy(true);
        try {
            const token = await clerk?.session?.getToken();
            const response = await fetch("/api/check-in", {
                method: "POST",
                headers: {
                    "Content-Type": "application/json",
                    ...(token ? { Authorization: `Bearer ${token}` } : {}),
                },
                body: JSON.stringify({
                    name: name.trim(),
                    room: room.trim(),
                    consent: true,
                }),
            });
            const data = (await response.json()) as {
                message?: string;
                claim: string;
            };
            if (!response.ok)
                throw new Error(
                    data.message || "Could not prepare your visit.",
                );
            setClaim(data.claim);
            setStep(2);
        } catch (e) {
            if (signed)
                setError(
                    e instanceof Error
                        ? e.message
                        : "Google check-in is unavailable. Try again when online.",
                );
            else {
                setClaim(
                    "G1." +
                        btoa(
                            unescape(
                                encodeURIComponent(
                                    JSON.stringify({
                                        name: name.trim(),
                                        room: room.trim(),
                                        verified: false,
                                    }),
                                ),
                            ),
                        )
                            .replace(/\+/g, "-")
                            .replace(/\//g, "_")
                            .replace(/=+$/, ""),
                );
                setStep(2);
            }
        } finally {
            setBusy(false);
        }
    }
    const local = `http://192.168.4.1/play${claim ? `#profile=${encodeURIComponent(claim)}` : ""}`;
    return (
        <>
            <header className="mast">
                <a className="wordmark" href="/" aria-label="203 Arcade home">
                    203<span>ARCADE</span>
                </a>
                <span className="room-label">BY ACHYUTH & JAIDEEP</span>
                <a className="small-link" href="#how">
                    How it works <span>↗</span>
                </a>
            </header>
            <main>
                <section className="hero">
                    <div className="hero-copy">
                        <p className="eyebrow">
                            <span className="tiny-square" /> A LITTLE ROOM FOR
                            PLAY
                        </p>
                        <h1>
                            Your phone.
                            <br />
                            Our <em>arcade.</em>
                        </h1>
                        <p className="intro">
                            Something fun at the door.
                            <br />
                            Pick a game, take a turn, make yourself at home.
                        </p>
                        <button className="primary" onClick={() => setStep(1)}>
                            Let’s play <span>↗</span>
                        </button>
                        <p className="micro">
                            No app. iPhone & Android. Just be nearby.
                        </p>
                    </div>
                    <div
                        className="cabinet"
                        aria-label="Preview of the Room 203 display"
                    >
                        <div className="cabinet-top">
                            <span>ROOM</span>
                            <strong>203</strong>
                            <span>
                                EST.
                                <br />
                                2026
                            </span>
                        </div>
                        <div className="bezel">
                            <div className="lcd">
                                <div className="lcd-caption">
                                    SHENANIGANS BY
                                    <br />
                                    THE 203 DUDES
                                </div>
                                <img
                                    src="/neko.gif"
                                    alt="Our lucky cat waving hello"
                                />
                            </div>
                        </div>
                        <div className="cabinet-bottom">
                            <span className="speaker" aria-hidden="true">
                                ▥▥▥
                            </span>
                            <span className="cabinet-light" />{" "}
                            <span>COME SAY HELLO</span>
                        </div>
                        <span className="sticker">
                            PLAY
                            <br />A LITTLE.
                        </span>
                    </div>
                </section>
                <section
                    ref={panel}
                    id="join"
                    className={`join-panel ${step ? "visible" : ""}`}
                    aria-label="Join the arcade"
                    hidden={!step}
                >
                    <div className="section-kicker">
                        <span>0{step} / 02</span>
                        <h2>
                            {step === 1
                                ? "Who’s taking a turn?"
                                : "Look up. You’re almost in."}
                        </h2>
                        <button
                            className="text-button"
                            onClick={() => {
                                setStep(step === 2 ? 1 : 0);
                                setError("");
                            }}
                        >
                            {step === 2 ? "← Back" : "Close ×"}
                        </button>
                    </div>
                    {step === 1 ? (
                        <div className="check-in">
                            <div>
                                <button
                                    className="google"
                                    disabled={!clerk || busy}
                                    onClick={() =>
                                        clerk?.openSignIn({
                                            forceRedirectUrl:
                                                location.origin + "/#join",
                                        })
                                    }
                                >
                                    <span className="google-g">G</span>
                                    {signed
                                        ? `Signed in${name ? " as " + name : ""}`
                                        : "Continue with Google"}
                                    {signed && (
                                        <span className="verified">
                                            VERIFIED ACCOUNT
                                        </span>
                                    )}
                                </button>
                                <p className="fine">
                                    {clerk
                                        ? "Optional. Google verifies your account; your room number is always self-reported."
                                        : "Google sign-in is being connected. Guest play is available without an account."}
                                </p>
                                <div className="or">OR JUST DROP IN</div>
                                <p className="guest-note">
                                    Guest names and room numbers are marked
                                    unverified. No password, email or account
                                    needed.
                                </p>
                            </div>
                            {signed && (
                                <button
                                    className="text-button"
                                    onClick={() => {
                                        setSigned(false);
                                        setName("");
                                        setError("");
                                        void clerk?.signOut().catch(() => {});
                                    }}
                                >
                                    Use an unverified guest name instead
                                </button>
                            )}
                            <form onSubmit={prepare}>
                                <div className="fields">
                                    <label>
                                        Your name{" "}
                                        {signed && <small>(optional)</small>}
                                        <input
                                            autoComplete="given-name"
                                            maxLength={32}
                                            value={name}
                                            onChange={(e) =>
                                                setName(e.target.value)
                                            }
                                            required={!signed}
                                            placeholder="What should we call you?"
                                        />
                                    </label>
                                    <label>
                                        Room number{" "}
                                        {signed && <small>(optional)</small>}
                                        <input
                                            autoComplete="off"
                                            maxLength={16}
                                            value={room}
                                            onChange={(e) =>
                                                setRoom(e.target.value)
                                            }
                                            required={!signed}
                                            placeholder="e.g. 307"
                                        />
                                    </label>
                                </div>
                                <label className="consent">
                                    <input
                                        type="checkbox"
                                        checked={consent}
                                        onChange={(e) =>
                                            setConsent(e.target.checked)
                                        }
                                    />
                                    <span>
                                        Save my visit in the private Room 203
                                        guest log.{" "}
                                        <a href="/privacy">What gets saved?</a>
                                    </span>
                                </label>
                                {error && (
                                    <p className="error" role="alert">
                                        {error}
                                    </p>
                                )}
                                <button className="primary" disabled={busy}>
                                    {busy
                                        ? "Getting your pass…"
                                        : "Next: connect to the arcade"}{" "}
                                    <span>→</span>
                                </button>
                            </form>
                        </div>
                    ) : (
                        <div className="handoff">
                            <div>
                                <ol className="steps">
                                    <li>
                                        <span>1</span>
                                        <div>
                                            <strong>
                                                Keep your usual connection.
                                            </strong>
                                            <p>
                                                Campus Wi-Fi or mobile data both
                                                work. Your phone and the display
                                                meet through our Internet relay.
                                            </p>
                                        </div>
                                    </li>
                                    <li>
                                        <span>2</span>
                                        <div>
                                            <strong>
                                                Open your controller.
                                            </strong>
                                            <p>
                                                Ask for a code, then enter the
                                                six digits on the physical
                                                display. The code is for one
                                                player and expires after a
                                                minute.
                                            </p>
                                        </div>
                                    </li>
                                    <li>
                                        <span>3</span>
                                        <div>
                                            <strong>
                                                Eyes on the display. Thumbs on
                                                your phone.
                                            </strong>
                                            <p>
                                                Choose a game and tap Ready. A
                                                friend can join you in the menu
                                                for two-player Pong.
                                            </p>
                                        </div>
                                    </li>
                                </ol>
                                <a
                                    className="primary"
                                    href={
                                        "/controller.html#profile=" +
                                        encodeURIComponent(claim)
                                    }
                                >
                                    Open controller <span>↗</span>
                                </a>
                                <p className="fine">
                                    No Bluetooth permissions or special browser.
                                    Internet interruptions release controls
                                    safely.
                                </p>
                                {!ready && (
                                    <p className="setup-note">
                                        The display’s Internet connection is
                                        being checked. The local connection
                                        below is also available.
                                    </p>
                                )}
                            </div>
                            <div className="wifi-card">
                                <span className="eyebrow">
                                    IF THE INTERNET TAKES A BREAK
                                </span>
                                <strong>Play nearby, offline.</strong>
                                <details>
                                    <summary>
                                        Use the display Wi-Fi instead
                                    </summary>
                                    <p>
                                        Join <b>{WIFI.ssid}</b> in your phone’s
                                        Wi-Fi settings. Choose “Without
                                        Internet” on iPhone, or “Stay connected”
                                        on Android.
                                    </p>
                                    <div className="wifi-password">
                                        <code>{WIFI.password}</code>
                                        <button
                                            className="text-button"
                                            onClick={async () => {
                                                try {
                                                    await navigator.clipboard.writeText(
                                                        WIFI.password,
                                                    );
                                                    setCopied(true);
                                                } catch {
                                                    setCopied(false);
                                                }
                                            }}
                                        >
                                            {copied ? "Copied ✓" : "Copy"}
                                        </button>
                                    </div>
                                    <label className="consent">
                                        <input
                                            type="checkbox"
                                            checked={connected}
                                            onChange={(e) =>
                                                setConnected(e.target.checked)
                                            }
                                        />
                                        <span>I’ve joined {WIFI.ssid}</span>
                                    </label>
                                    <a
                                        className={`primary ${!connected ? "disabled" : ""}`}
                                        aria-disabled={!connected}
                                        href={connected ? local : undefined}
                                        onClick={(e) => {
                                            if (!connected) e.preventDefault();
                                        }}
                                    >
                                        Open local controller <span>↗</span>
                                    </a>
                                    <p className="fine">
                                        Local address:{" "}
                                        <span className="address">
                                            http://192.168.4.1/play
                                        </span>
                                        . Use the full browser, rather than a
                                        Wi-Fi sign-in window.
                                    </p>
                                </details>
                                <p className="fine">
                                    The same games, turn limits and private
                                    visit log work through either connection.
                                </p>
                            </div>
                        </div>
                    )}
                </section>
                <section className="games-section">
                    <div className="section-kicker">
                        <span>ON THE DISPLAY</span>
                        <h2>
                            Three good ways
                            <br />
                            to lose track of a minute.
                        </h2>
                        <span className="edition">VOL. 01</span>
                    </div>
                    <div className="game-shelf">
                        {games.map((g, i) => (
                            <article className={`game ${g.color}`} key={g.name}>
                                <div className="game-art">
                                    <img
                                        src={`/games/game-${i}.png`}
                                        alt={`${g.name} on the 160 by 128 display`}
                                    />
                                    <span className="game-number">
                                        0{i + 1}
                                    </span>
                                </div>
                                <div className="game-title">
                                    <h3>{g.name}</h3>
                                    <span>{g.tag}</span>
                                </div>
                                <p>{g.copy}</p>
                                <div className="game-foot">
                                    {g.keys}
                                    <span>↗</span>
                                </div>
                            </article>
                        ))}
                    </div>
                    <p className="house-note">
                        <span>HOUSE RULE</span> Solo Pong is you versus our
                        unbeatable house opponent. Bring a second player for a
                        human-versus-human match.
                    </p>
                </section>
                <section className="how" id="how">
                    <div>
                        <p className="eyebrow">A SHARED LITTLE STAGE</p>
                        <h2>
                            Good games.
                            <br />
                            Good neighbours.
                        </h2>
                    </div>
                    <div className="rules">
                        <div>
                            <span>01</span>
                            <p>
                                <strong>Look up, not down.</strong>The menu and
                                game live on the display. Your phone is the
                                controller.
                            </p>
                        </div>
                        <div>
                            <span>02</span>
                            <p>
                                <strong>Two friends? Meet in the menu.</strong>
                                Player two joins before Pong starts. Both tap
                                Ready; neither paddle is AI.
                            </p>
                        </div>
                        <div>
                            <span>03</span>
                            <p>
                                <strong>A turn, not a takeover.</strong>Five
                                minutes per turn. Idle controllers are released
                                automatically. The cat comes back when nobody’s
                                playing.
                            </p>
                        </div>
                    </div>
                </section>
            </main>
            <footer>
                <a className="wordmark" href="/">
                    203<span>ARCADE</span>
                </a>
                <p>Shenanigans by the 203 dudes.</p>
                <nav>
                    <a href="/privacy">Privacy & guest log</a>
                    <a href="#how">Help connecting</a>
                </nav>
            </footer>
        </>
    );
}
