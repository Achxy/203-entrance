import type { Metadata } from "next";
import "./globals.css";
export const metadata: Metadata = {
    title: "203 Arcade — a little room for play",
    description:
        "Your phone is the controller. Our door is the arcade. Tetris, Pong and Pac-Man at Room 203.",
    metadataBase: new URL("https://203.at.achyuth.dev"),
};
export default function Layout({ children }: { children: React.ReactNode }) {
    return (
        <html lang="en">
            <body>{children}</body>
        </html>
    );
}
