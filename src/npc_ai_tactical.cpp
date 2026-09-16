#include "npc_ai_tactical.h"

#include <array>
#include <string_view>

#include "cata_utility.h"
#include "npc.h"
#include "npc_ai_batch_pickup.h"
#include "npc_ai_interior.h"
#include "npctalk.h"

namespace
{

template<std::size_t N>
bool contains_any_phrase( const std::string &line,
                          const std::array<std::string_view, N> &phrases )
{
    for( const std::string_view phrase : phrases ) {
        if( lcmatch( line, phrase ) ) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace npc_ai
{

tactical_order parse_tactical_order( const std::string &player_line )
{
    // lcmatch is case-insensitive and removes accents from the subject, so the
    // ASCII Spanish forms below also recognize their correctly accented forms.
    // Fast path.  Anything not listed here can still become an order through
    // the model-backed intent classifier in npc_ai_order_intent.cpp.
    static constexpr std::array follow_phrases = {
        std::string_view( "sigueme" ),
        std::string_view( "siganme" ),
        std::string_view( "seguidme" ),
        std::string_view( "ven conmigo" ),
        std::string_view( "venid conmigo" ),
        std::string_view( "vengan conmigo" ),
        std::string_view( "vamos conmigo" ),
        std::string_view( "acompaname" ),
        std::string_view( "acompanenme" ),
        std::string_view( "acompanadme" ),
        std::string_view( "todos siganme" ),
        std::string_view( "todos conmigo" ),
        std::string_view( "pegate a mi" ),
        std::string_view( "pegaos a mi" ),
        std::string_view( "peguense a mi" ),
        std::string_view( "no te quedes atras" ),
        std::string_view( "no se queden atras" ),
        std::string_view( "follow me" ),
        std::string_view( "everyone follow me" ),
        std::string_view( "come with me" ),
        std::string_view( "stay with me" ),
        std::string_view( "stick with me" )
    };
    static constexpr std::array guard_phrases = {
        std::string_view( "quedate aqui" ),
        std::string_view( "quedense aqui" ),
        std::string_view( "quedaos aqui" ),
        std::string_view( "espera aqui" ),
        std::string_view( "esperen aqui" ),
        std::string_view( "esperad aqui" ),
        std::string_view( "esperame aqui" ),
        std::string_view( "quieto" ),
        std::string_view( "quietos" ),
        std::string_view( "no te muevas" ),
        std::string_view( "no se muevan" ),
        std::string_view( "no os movais" ),
        std::string_view( "protejan esta posicion" ),
        std::string_view( "mantengan esta posicion" ),
        std::string_view( "vigila esta posicion" ),
        std::string_view( "vigilen esta posicion" ),
        std::string_view( "hold position" ),
        std::string_view( "hold here" ),
        std::string_view( "stay here" ),
        std::string_view( "wait here" ),
        std::string_view( "guard this position" ),
        std::string_view( "guard here" )
    };

    if( contains_any_phrase( player_line, guard_phrases ) ) {
        return tactical_order::guard;
    }
    if( contains_any_phrase( player_line, follow_phrases ) ) {
        return tactical_order::follow;
    }
    return tactical_order::none;
}

tactical_order_result execute_tactical_order( const std::vector<npc *> &targets,
        const std::string &player_line )
{
    return execute_tactical_order( targets, parse_tactical_order( player_line ) );
}

tactical_order_result execute_tactical_order( const std::vector<npc *> &targets,
        const tactical_order order )
{
    tactical_order_result result;
    result.order = order;
    result.handled = result.order != tactical_order::none;
    if( !result.handled ) {
        return result;
    }

    for( npc *target : targets ) {
        if( target == nullptr || !target->is_active() || !target->is_player_ally() ) {
            continue;
        }
        // A newer tactical order cancels a pending "get inside" walk, so the
        // companion never turns into a guard later by surprise.
        clear_interior_hold( *target );
        cancel_food_search( *target );
        if( result.order == tactical_order::guard ) {
            talk_function::assign_guard( *target );
        } else {
            talk_function::stop_guard( *target );
        }
        result.affected.push_back( target );
    }
    return result;
}

} // namespace npc_ai
