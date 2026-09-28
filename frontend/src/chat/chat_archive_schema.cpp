#include "chat_archive.hpp"

namespace Chat {

namespace {

// Chat schema v1: the multichat scrollback past the in-memory ring, one row per admitted
// message. A ladder of its own in a file of its own (chat.db, never history.db): a chat
// migration must not be able to make an older build refuse the session history.
//
// `seq` is the host's admission order, the order chat reads and pages in, so it is the key.
// `dest` is OAuth::DestinationKey of the destination the message came from, and
// (dest, msg_id) is the same identity the ring dedupes on, so a re-delivery the ring no
// longer remembers is still refused here. `platform` is stored normalized (trimmed,
// lowercased), so a platform filter is an exact match that can use its index. `body` is
// the frame as the ring holds it, less `seq` and `rx`, which the columns carry; `deleted`
// marks a message a moderator removed, whose text the body no longer holds.
constexpr const char *kChatMigration1 = R"SQL(
CREATE TABLE messages (
    seq          INTEGER PRIMARY KEY NOT NULL,
    dest         TEXT    NOT NULL,
    msg_id       TEXT    NOT NULL,
    platform     TEXT    NOT NULL,
    account_id   TEXT    NOT NULL,
    profile_uuid TEXT    NOT NULL DEFAULT '',
    author_id    TEXT    NOT NULL DEFAULT '',
    ts           INTEGER NOT NULL,
    rx           INTEGER NOT NULL,
    launch_id    TEXT    NOT NULL,
    deleted      TEXT    NOT NULL DEFAULT '',
    body         TEXT    NOT NULL
);

CREATE UNIQUE INDEX ux_messages_key ON messages (dest, msg_id);
CREATE INDEX ix_messages_rx ON messages (rx);
CREATE INDEX ix_messages_author ON messages (dest, author_id);
CREATE INDEX ix_messages_platform_seq ON messages (platform, seq);
CREATE INDEX ix_messages_account_seq ON messages (account_id, seq);
)SQL";

constexpr History::Migration kChatMigrations[] = {
	{1, kChatMigration1},
};

} // namespace

const History::Ladder kChatLadder{"chat", kChatMigrations};

} // namespace Chat
