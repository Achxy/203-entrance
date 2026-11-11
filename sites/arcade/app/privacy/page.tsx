export default function Privacy() {
    return (
        <main className="prose">
            <a href="/">← Back to the arcade</a>
            <h1>
                A guest log.
                <br />
                Not a public profile.
            </h1>
            <p>
                Room 203 keeps a private record of visits: the name and room
                number you choose to share, a random visit ID, the device, and
                whether the visit used a signed Google identity pass or an
                unverified guest name. A Google pass proves account ownership
                when it was issued; an offline display cannot recheck account
                status. Room numbers are always self-reported.
            </p>
            <p>
                Google sign-in is optional and is handled by Clerk. We never
                receive your Google password. The public site can prepare a pass
                while you have Internet; the local controller does not ask for a
                Google password or an administrator password.
            </p>
            <p>
                Games run locally on the display. Internet play uses an
                encrypted Cloudflare relay. Controller input is used to move
                your player and is not recorded as a movement history. A
                device-signed visit receipt distinguishes an actual
                physical-screen check-in from someone merely opening this
                website.
            </p>
            <p>
                The display keeps a bounded local visit queue so guests can play
                without Internet. Cloud visits expire after 90 days. Expired
                records are hidden immediately and removed from the database and
                Clerk during owner log maintenance; Google sign-in accounts are
                retained separately until you request deletion. Names, rooms,
                account identifiers and private guest records are never public
                leaderboards.
            </p>
            <p>
                You can decline by closing the check-in form. For a copy or
                deletion of your visit record, contact Achyuth or Jaideep at
                Room 203 and provide your visit ID if available. Clearing
                browser storage only removes your local copy.
            </p>
            <h2>Sharing the screen</h2>
            <p>
                Each turn lasts at most five minutes. The menu releases idle
                players after 25 seconds; games release inactive controllers
                after 45 seconds, and missing connections after 10 seconds. A
                brief cooldown follows a released turn. Keep the controller
                visible while you play. These limits run on the display and
                cannot be extended by leaving a tab open.
            </p>
            <h2>Connection safety</h2>
            <p>
                Internet controls travel over HTTPS and WSS. The relay retains
                bounded, hashed network-address rate counters to limit abuse; it
                does not keep a movement history. The arcade Wi-Fi fallback is a
                shared local network with no Internet uplink required. Only game
                controls and a check-in receipt pass through the local HTTP
                controller. Administrator settings and firmware updates use the
                separately authenticated HTTPS console.
            </p>
        </main>
    );
}
