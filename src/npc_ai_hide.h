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
// that tile.  On the way it runs, never stops to fight (the order is the
// retreat: even mid-combat it turns and goes, shutting doors behind) and only
// fights if an enemy keeps it boxed in.  While hidden it only reacts to a
// hostile inside the room: at a distance it looks for another hideout, and
// with none it keeps away from the enemies without engaging (evading) until
// it is exhausted or boxed in, when it turns and fights, saying so; right
// next to it, it stands and fights at once, and goes back to hiding after ten
// quiet turns.  "Vengan conmigo" / "Quedense aqui"
// end the hideout and give the combat rules back.  No model involved.

enum class hide_phase : int {
    travelling,   // walking to the hideout
    hidden,       // holding the tile, engagement forbidden
    cornered,     // no other hideout: self-defence allowed until it is quiet
    evading,      // no hideout and enemies in sight: keeps its distance,
                  // never engages, until exhausted or boxed in (-> cornered)
    closing_door, // hidden and quiet, but a door of the room was left open:
                  // walks over, shuts it and comes back to its tile
    leaving       // follow order received inside the room: walking out to
                  // the door with door-opening still allowed, then done
};

struct hide_state {
    hide_phase phase = hide_phase::travelling;
    tripoint_abs_ms destination;
    // Tiles of the room chosen as hideout; empty for the fallback (an
    // interior tile away from the enemies, but not an enclosed room).
    std::set<tripoint_abs_ms> room;
    // The room's doors, watched while hidden: one left open is an invitation.
    std::set<tripoint_abs_ms> doors;
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
    // Travelling: turns without moving.  A companion boxed in by an enemy
    // it is not allowed to attack stops running and fights after a few.
    tripoint_abs_ms last_pos;
    int stuck_ticks = 0;
    // Evading: turns spent keeping away; a hideout is looked for again every
    // few of them.
    int evade_ticks = 0;
    // The hideout moved the companion itself this turn (see hide_took_turn).
    bool turn_taken = false;
    // Shutting a door: the door being closed and the tile to lie back down on.
    tripoint_abs_ms door_target;
    tripoint_abs_ms return_pos;
    int last_door_turn = -1;
    // A door seen open, and the turn it was first seen that way: the
    // companion gives whoever opened it a couple of turns to come through.
    tripoint_abs_ms door_seen;
    int door_seen_turn = -1;
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

// True when process_hide() already spent the companion's turn moving it away.
// With an enemy in reach the ordinary turn would end in a melee attack (the
// vanilla walk hits whatever stands on the next tile, and self-defence beats
// the "do not engage" rule), so while running to the hideout the order drives
// the step itself and npc::move() must stop there.  Clears the flag.
bool hide_took_turn( const npc &who );

bool is_hiding( const npc &who );
std::optional<hide_state> hide_state_for( const npc &who );

// End the hideout: restore the combat rules changed by the order.  Does not
// change the attitude or the mission; the caller (a tactical order) does.
// `step_out`: the companion is going back to following.  If it is inside the
// room it hid in, the hideout is not dropped yet: it turns into the `leaving`
// phase, a walk to the room's door with door-opening kept allowed (the
// player's "avoid doors" rule would otherwise leave it shut in), and ends when
// it stands in the doorway or out of the room.  The vanilla follow logic
// pauses whenever the path to the player is short, so without this a companion
// right behind a closed door stays put until the player opens it.
void cancel_hide( const npc &who, bool step_out = false );

void reset_all_hideouts();

} // namespace npc_ai

#endif // CATA_SRC_NPC_AI_HIDE_H
