export type ClerkClient = {
    load: (options?: unknown) => Promise<void>;
    user?: { id: string; firstName?: string; fullName?: string };
    session?: { getToken: () => Promise<string | null> };
    openSignIn: (options?: unknown) => void;
    addListener: (fn: () => void) => () => void;
    signOut: () => Promise<void>;
};
declare global {
    interface Window {
        Clerk?: ClerkClient;
        __internal_ClerkUICtor?: unknown;
    }
}
export type PublicConfig = {
    device_ready: boolean;
    relay_url?: string;
    clerk_key?: string;
    clerk_script: string;
    clerk_ui: string;
};
let loading: Promise<ClerkClient> | undefined;
function script(src: string, key?: string) {
    return new Promise<void>((resolve, reject) => {
        const el = document.createElement("script");
        el.src = src;
        el.crossOrigin = "anonymous";
        el.async = true;
        if (key) el.setAttribute("data-clerk-publishable-key", key);
        const timer = setTimeout(() => {
            el.remove();
            reject(
                new Error(
                    "Google sign-in took too long. Try again when online.",
                ),
            );
        }, 15000);
        el.onload = () => {
            clearTimeout(timer);
            resolve();
        };
        el.onerror = () => {
            clearTimeout(timer);
            el.remove();
            reject(
                new Error(
                    "Google sign-in is unavailable. Guest play still works.",
                ),
            );
        };
        document.head.appendChild(el);
    });
}
export function loadClerk(cfg: PublicConfig) {
    if (!cfg.clerk_key)
        return Promise.reject(new Error("Google sign-in is being connected."));
    if (!loading)
        loading = (async () => {
            if (!window.__internal_ClerkUICtor) await script(cfg.clerk_ui);
            if (!window.Clerk) await script(cfg.clerk_script, cfg.clerk_key);
            await window.Clerk!.load({
                ui: { ClerkUI: window.__internal_ClerkUICtor },
            });
            return window.Clerk!;
        })().catch((e) => {
            loading = undefined;
            throw e;
        });
    return loading;
}
