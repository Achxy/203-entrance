CREATE TABLE `rate_limits` (
	`key` text PRIMARY KEY NOT NULL,
	`count` integer NOT NULL,
	`expires_at` integer NOT NULL
);
--> statement-breakpoint
CREATE TABLE `visits` (
	`id` text PRIMARY KEY NOT NULL,
	`name` text NOT NULL,
	`room` text NOT NULL,
	`clerk_user_id` text,
	`verification` text NOT NULL,
	`device` text NOT NULL,
	`received_at` integer NOT NULL,
	`receipt_hash` text NOT NULL,
	`receipt` text NOT NULL,
	`clerk_sync` text DEFAULT 'pending' NOT NULL
);
--> statement-breakpoint
CREATE UNIQUE INDEX `visits_receipt_hash_unique` ON `visits` (`receipt_hash`);