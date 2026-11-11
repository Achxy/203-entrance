import { sqliteTable, text, integer } from "drizzle-orm/sqlite-core";
export const visits = sqliteTable("visits", {
    id: text("id").primaryKey(),
    name: text("name").notNull(),
    room: text("room").notNull(),
    clerkUserId: text("clerk_user_id"),
    verification: text("verification").notNull(),
    device: text("device").notNull(),
    receivedAt: integer("received_at").notNull(),
    receiptHash: text("receipt_hash").notNull().unique(),
    receipt: text("receipt").notNull(),
    syncStarted: integer("sync_started").notNull().default(0),
    clerkSync: text("clerk_sync").notNull().default("pending"),
});
export const rateLimits = sqliteTable("rate_limits", {
    key: text("key").primaryKey(),
    count: integer("count").notNull(),
    expiresAt: integer("expires_at").notNull(),
});
