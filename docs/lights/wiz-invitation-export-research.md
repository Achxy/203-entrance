# WiZ owner invitation, local-key export and commissioning boundary

Research checked on 2026-11-16. This is a source review, not a successful account join, export or bulb commissioning result. The invitation code was neither inspected by this research task nor submitted anywhere. No WiZ account, light, gateway firmware or Mac network settings were changed.

## What an owner invitation establishes

WiZ documents accepting a home invitation in its mobile app, using a QR code, invitation link or code entry. A new app user can skip linking a third-party sign-in, create an app profile, then choose **Join a home → Use invitation code → Join**. For an existing profile, the documented route is **Settings → current home name → Others → Join a home**. The article calls the code “6 digits”; this note does not impose that wording as a parser constraint on an observed code. [Official invitation acceptance](https://wizconnected.helpshift.com/hc/en/7-wiz-v2/faq/693-accepting-an-invite-to-a-wiz-home/)

The inviter selects the role. Owners can manage members and integrations; Users and Guests cannot edit settings. Therefore an **owner** invite can provide the app permissions needed to reach the integration settings after acceptance. It is not the home UDP signing key, and it does not prove that an unauthenticated HTTP request containing only the invite can retrieve that key. [Official invitation creation](https://wizconnected.helpshift.com/hc/en/7-wiz-v2/faq/692-inviting-new-members-in-a-home/), [official roles](https://wizconnected.helpshift.com/hc/en/7-wiz-v2/faq/691-user-roles-and-access-rights/)

## Supported export route

1. In the official WiZ app, select the existing home with Owner access.
2. Open its settings, then **Integrations → Local integrations → Get the link**.
3. Open the generated link in a browser and save the JSON privately.
4. Import only the selected home's `udp_signing_key` into trusted gateway owner setup; validate exactly 32 hexadecimal characters (16 bytes). Do not distribute the key with the guest app.

WiZ officially confirms that the integration page produces a browser-downloadable JSON containing the home security key, home structure and device capabilities. Links expire after **15 minutes**; exported home data refreshes every **12 hours**, so recent structural changes may not yet appear. The exact navigation/button labels above are corroborated by a first-hand protocol researcher's walkthrough, not by a live inspection of this user's app. [Official local integrations](https://wizconnected.helpshift.com/hc/en/7-wiz-v2/faq/1178-local-integrations/), [first-hand export walkthrough](https://gist.github.com/rmcolber/ccb5d804c36270efd1e1dfd239efe740), [pinned pywizlight key format](https://github.com/sbidy/pywizlight/blob/19e607afe0eb6db1473602639e194d691234d4fd/README.md#signed-requests-newer-firmware)

Keep local communication enabled with **Only verified controls**. WiZ states that this setting accepts verified WiZ/integration messages and that rotating the home security key removes third-party access. Exporting a key does not demonstrate that the actual bulbs reject unsigned writes; that remains a hardware check. [Official local security](https://wizconnected.helpshift.com/hc/en/7-wiz-v2/faq/548-secure-or-disable-local-network-communication/)

## Mac, browser and public protocol evidence

The current official India and US App Store listings say **Only for iPhone**. Their compatibility sections list iPhone/iOS 16+ and Apple Vision, not Mac. No supported native Mac installation is established. The official invitation guide redirects to the mobile app. The export guide permits a browser to download an already generated link; it does not document a browser-only home join or export-generation interface. This is a bounded search result, not proof that no private endpoint exists. [India App Store](https://apps.apple.com/in/app/wiz-connected/id1587655962), [US App Store](https://apps.apple.com/us/app/wiz-connected/id1587655962)

Public source review found this consumer-cloud implementation:

| Source | What it actually establishes |
| --- | --- |
| `dbqp01/wiz-tray`, commit `63952b93d21b451353fef6ee802fbbcefc9fee7b` | Code uses `POST https://api.wiz.world/oauth/v2/token`, Basic client authentication, scope `wiz_android_v2`, Google-token grant `urn:google_v2`, and refresh tokens. Its README requires separately obtained WiZ client credentials and says its build checks do not verify live WiZ login. It has no invitation redemption or export generation implementation. |
| Official WiZ Pro API | Separately documents OAuth/PKCE with a vendor-issued client ID and `https://api.pro.wizconnected.com/api/oauth/token`. It is not evidence for redeeming consumer home invites or downloading their UDP key. |

[Consumer implementation](https://github.com/dbqp01/wiz-tray/blob/63952b93d21b451353fef6ee802fbbcefc9fee7b/src/cloud_auth.rs), [its prerequisites and validation limits](https://github.com/dbqp01/wiz-tray/blob/63952b93d21b451353fef6ee802fbbcefc9fee7b/README.md), [official Pro API](https://docs.pro.wizconnected.com/)

No verified public consumer invite-acceptance or local-export-generation endpoint was found. A separate search hit tried a list of guessed authentication URLs; it was excluded as protocol evidence. Do not substitute those guesses, borrowed client credentials, or WiZ Pro authorization for the official owner flow. Further reverse engineering would require a legitimate current official-app artifact or an owner-authorized trace of that exact operation; neither was obtained in this review.

## Commissioning bulbs onto the installed ESP32

The current gateway AP does not forward Internet traffic. The ESP's own campus STA connection and outbound relay do not give its AP clients Internet access. That architecture supports local control of an already provisioned bulb, but does not establish complete official WiZ commissioning.

WiZ's network guide describes devices reaching its cloud through their Wi-Fi router. It lists local UDP 18266 for commissioning, local UDP 38899/38900 for control, and outbound DNS, NTP, MQTT/TLS, HTTPS and firmware-download traffic. Its current app guide expects the phone on the target Wi-Fi and may update firmware after pairing. [Official network requirements, pp. 4–7](https://assets.wizconnected.com/manuals/WiZ-Network-Configuration-v2-01162024.pdf), [official installation flow](https://wizconnected.helpshift.com/hc/en/7-wiz-v2/faq/527-adding-wiz-lights-in-your-home/)

Practical options still requiring implementation or hardware qualification:

- **Final ESP AP:** provide bounded, owner-enabled Internet forwarding/NAPT and DNS for commissioning, without changing the Mac's connection. Qualify resource use, campus routing and actual bulb cloud registration before calling this onboarding complete.
- **Temporary commissioning network:** a separate phone/router can briefly use the final AP's SSID/password while the ESP AP is disabled, then hand those credentials back to the ESP AP. This is a proposed migration method, not tested here. Keep the same WiZ home. Current app release notes also advertise Bluetooth reinstallation while retaining settings, but do not establish Internet-free reinstallation. [Official app release notes, version 1.31.0](https://apps.apple.com/us/app/wiz-connected/id1587655962)

After migration, test bulb power loss and recovery of time needed by signed commands. Gateway time alone does not demonstrate that an isolated bulb has recovered its own clock.

The public `50ck/wiz` offline-pairing implementation is not an equivalent supported path: at pinned commit `80624dce3cd381e08f986d13df83b1dce2828fc1` it assigns local home ID `420`, and its author reports that reaching WiZ MQTT can cause unpairing. It does not establish importing this owner's home signing key or preserving the existing cloud home. No code from it was executed. [Pairing source](https://github.com/50ck/wiz/blob/80624dce3cd381e08f986d13df83b1dce2828fc1/src/wiz/pairing.py), [author's limitation](https://github.com/50ck/wiz/blob/80624dce3cd381e08f986d13df83b1dce2828fc1/README.md)

The supported conclusion is: an owner invite can lead to the export through the official app, but invite-only key retrieval by a standalone client is unverified. The reliable immediate artifact is the private JSON export; verified official commissioning onto the ESP AP additionally needs an actual bulb Internet path or a qualified migration route.
