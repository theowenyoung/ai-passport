// main/card_store.h — Flash access for Blog Cards.
//
// `cards` holds the pack written by tools/blog_cards.py; it is mapped read-only.
// `cardstat` holds reading progress in two alternating slots with checksums, so
// a power loss during a write leaves the previous copy intact. Call from one
// task only; writes briefly stall code running from Flash.
#pragma once

#include "card_pack.h"
#include "card_sched.h"
#include "esp_err.h"

// Map and validate the card pack. On failure `pack` is left empty.
card_pack_status_t card_store_open_pack(card_pack_t *pack);

// Apply the newest valid progress copy to `s` (already initialized for the pack).
// Returns false when no valid copy exists, which is normal on first use.
bool card_store_load(card_sched_t *s);

// Write the progress to the older slot. `fresh_after` is the newest publish time
// in the current pack, so the next pack can tell which posts are new.
esp_err_t card_store_save(const card_sched_t *s, uint32_t fresh_after);
