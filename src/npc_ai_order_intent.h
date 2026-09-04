#pragma once
#ifndef CATA_SRC_NPC_AI_ORDER_INTENT_H
#define CATA_SRC_NPC_AI_ORDER_INTENT_H

#include <cstddef>
#include <string>

class npc;

namespace npc_ai
{

struct ai_request_completion;

// Second stage of order recognition.  The keyword parsers in
// npc_ai_tactical / npc_ai_interior / npc_ai_pickup are the fast path; when
// none of them matches a line that still looks like an order, the model is
// asked to classify it into this closed catalogue.  C++ keeps every authority:
// the model may only pick an entry (and quote the object it refers to), and
// each entry maps to the same execution code the keyword path already uses.
enum class order_intent : int {
    none,
    follow,
    guard,
    enter_interior,
    pickup,
    wield,
    drop,
    wear
};

const char *order_intent_name( order_intent intent );

// Cheap heuristic gate so ordinary conversation never pays a classifier round
// trip: short, non-interrogative, routed as GENERAL by the context router.
bool order_intent_candidate( const std::string &player_line );

std::string build_order_intent_prompt( const std::string &player_line, bool everyone,
                                       std::size_t ally_count );

struct order_intent_parse {
    order_intent intent = order_intent::none;
    std::string target;
    bool valid = false;
};

// Accepts only "ORDER=<catalogue id>" (+ optional "TARGET=<text>").  Anything
// else is invalid and treated as plain dialogue by the caller.
order_intent_parse parse_order_intent_response( const std::string &text );

// Enqueues the classifier request.  Returns false when the queue refused it
// (the caller then falls back to ordinary dialogue immediately).
bool enqueue_order_intent_resolution( const npc &who, const std::string &player_line,
                                      bool everyone, std::size_t ally_count );

// Main-thread completion handler: executes the classified order through the
// existing deterministic functions, or forwards the line to normal dialogue.
void apply_order_intent_completion( npc &who, const ai_request_completion &completion );

} // namespace npc_ai

#endif // CATA_SRC_NPC_AI_ORDER_INTENT_H
