#pragma once
#ifndef CATA_SRC_NPC_AI_HIDE_H
#define CATA_SRC_NPC_AI_HIDE_H

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "coordinates.h"

class npc;

namespace npc_ai
{

// "Escondanse": the companion walks to an enclosed room (walls, closed
// windows and at least one door), closes the door behind it, stops fighting,
// lies down (prone: behind a counter or a table it is not seen) and holds
// that tile.  While hidden it only reacts to a hostile inside the room: at a
// distance it looks for another hideout (and defends itself if there is
// none); right next to it, it stands and fights at once, and goes back to
// hiding after ten quiet turns.  "Vengan conmigo" / "Quedense aqui"
// end the hideout and give the combat rules back.  No model involved.

enum class hide_phase : int {
    travelling,   // walking to the hideout
    hidden,       // holding the tile, engagement forbidden
    cornered      // no other hideout: self-defence allowed until it is quiet
};

struct hide_state {
    hide_phase phase = hide_phase::travelling;
    tripoint_abs_ms destination;
    // Tiles of the room chosen as hideout; empty for the fallback (an
    // interior tile away from the enemies, but not an enclosed room).
    std::set<tripoint_abs_ms> room;
    bool enclosed = false;
    // Ally rule overrides as they were before the order (rule bit, override
    // enabled, override value), restored exactly on cancel.  While the order
    // lasts the hideout's own values are re-applied every turn, so the vanilla
    // "relax (clear overrides)" cannot silently leave the companion fighting.
    struct rule_backup {
        int rule = 0;
        bool was_enabled = false;
        bool was_set = false;
    };
    std::vector<rule_backup> rule_backups;
    int quiet_ticks = 0;
    int relocations = 0;
    int last_relocation_turn = -1;
};

struct hide_order_result {
    bool handled = false;
    bool success = false;
    // npc id -> destination.  `enclosed` false means the fallback was used.
    std::vector<std::pair<int, tripoint_abs_ms>> assignments;
    std::size_t fallback_count = 0;
    std::string message;
};

// Deterministic keyword parser: "escondanse", "escondete", "ocultense", with
// an optional direct address in front ("Liam, escondete") and never after a
// negation.
bool parse_hide_order( const std::string &spoken );

// Each companion goes to ITS nearest hideout (they may share a room but never
// a tile).  Followers and guards alike; a pending interior hold or search is
// cancelled by the new order.
hide_order_result execute_hide_order( const std::vector<npc *> &targets );

// Per-turn hook (npc::move): arrival, threat check, relocation.  Never
// consumes the turn; returns true while the companion has an active hideout.
bool process_hide( npc &who );

bool is_hiding( const npc &who );
std::optional<hide_state> hide_state_for( const npc &who );

// End the hideout: restore the combat rules changed by the order.  Does not
// change the attitude or the mission; the caller (a tactical order) does.
// `step_out`: the companion is going back to following.  If it is inside the
// room it hid in, it first walks to the room's door (opening it): the vanilla
// follow logic pauses whenever the path to the player is short, so a
// companion right behind a closed door would otherwise stay put until the
// player walks away or opens the door.
void cancel_hide( const npc &who, bool step_out = false );

void reset_all_hideouts();

} // namespace npc_ai

#endif // CATA_SRC_NPC_AI_HIDE_H
