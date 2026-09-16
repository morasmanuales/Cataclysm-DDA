#include "npc_ai_order_menu.h"

#include <optional>
#include <string>
#include <vector>

#include "npc_ai_context.h"
#include "translations.h"

namespace npc_ai
{

namespace
{

std::string trim_target( const std::string &text )
{
    const std::size_t first = text.find_first_not_of( " \t\r\n" );
    if( first == std::string::npos ) {
        return std::string();
    }
    const std::size_t last = text.find_last_not_of( " \t\r\n." );
    return last < first ? std::string() : text.substr( first, last - first + 1 );
}

// `english` is already passed through _() at the call site so the gettext
// cache stays per-literal; the Spanish text is the runtime fallback used by
// every other NPC AI message.
std::string text( const std::string &english, const char *spanish )
{
    return localized_ai_message( english, spanish );
}

} // namespace

std::string menu_order_group_title( const menu_order_group group )
{
    switch( group ) {
        case menu_order_group::movement:
            return text( _( "— Movement —" ), "— Movimiento —" );
        case menu_order_group::items:
            return text( _( "— Items —" ), "— Objetos —" );
        case menu_order_group::tasks:
            return text( _( "— Tasks —" ), "— Tareas —" );
    }
    return std::string();
}

std::vector<menu_order_entry> order_menu_catalogue()
{
    using G = menu_order_group;
    std::vector<menu_order_entry> catalogue;
    catalogue.reserve( 17 );

    // Movement
    catalogue.push_back( { menu_order::follow, G::movement, menu_order_target::none, true, 'f',
                           text( _( "Follow me" ), "Síganme" ),
                           text( _( "Come with me and do not fall behind." ),
                                 "Vengan conmigo y no se queden atrás." ) } );
    catalogue.push_back( { menu_order::guard, G::movement, menu_order_target::none, true, 'g',
                           text( _( "Guard this position" ), "Vigilen esta posición" ),
                           text( _( "Stay here and hold the position." ),
                                 "Quédense aquí y mantengan la posición." ) } );
    catalogue.push_back( { menu_order::enter_interior, G::movement, menu_order_target::none, true, 'i',
                           text( _( "Get inside" ), "Entren al edificio" ),
                           text( _( "Take shelter in the nearest reachable safe building and hold there." ),
                                 "Refugiarse en el edificio seguro alcanzable más cercano y quedarse dentro." ) } );
    catalogue.push_back( { menu_order::drag_casualty, G::movement, menu_order_target::ally_name, true, 'a',
                           text( _( "Drag a casualty…" ), "Arrastrar a un herido…" ),
                           text( _( "Choose the injured companion; you then pick where to drag them." ),
                                 "Elige al compañero herido; después señalas adónde arrastrarlo." ) } );

    // Items
    catalogue.push_back( { menu_order::pickup, G::items, menu_order_target::item_words, true, 'p',
                           text( _( "Pick up an item…" ), "Recoger un objeto…" ),
                           text( _( "Name an object lying nearby.  Instant when exactly one visible item matches; otherwise the AI model picks among the real candidates." ),
                                 "Nombra un objeto cercano.  Inmediato si un solo objeto visible coincide; si no, el modelo de IA elige entre los candidatos reales." ) } );
    catalogue.push_back( { menu_order::pickup_all_food, G::items, menu_order_target::none, false, 'F',
                           text( _( "Pick up all the food" ), "Recoger toda la comida" ),
                           text( _( "Collect every edible item in sight, in batches." ),
                                 "Recoger por lotes toda la comida a la vista." ) } );
    catalogue.push_back( { menu_order::search_food, G::items, menu_order_target::none, false, 'b',
                           text( _( "Search for food" ), "Buscar comida" ),
                           text( _( "Walk the surroundings checking piles, fridges, cupboards and shelves within a short radius, and pick up the food found." ),
                                 "Recorrer los alrededores revisando pilas, neveras, armarios y estantes en un radio corto, y recoger la comida que encuentre." ) } );
    catalogue.push_back( { menu_order::search_item, G::items, menu_order_target::item_words, false, 'B',
                           text( _( "Search for an item…" ), "Buscar un objeto…" ),
                           text( _( "Name it; the companion walks the surroundings checking piles and containers, brings back every match and reports what it found." ),
                                 "Nómbralo; el compañero recorre los alrededores revisando pilas y contenedores, trae todo lo que coincida e informa de lo encontrado." ) } );
    catalogue.push_back( { menu_order::wield, G::items, menu_order_target::item_words, false, 'w',
                           text( _( "Wield an item…" ), "Empuñar un objeto…" ),
                           text( _( "Name a weapon or tool the companion carries or sees.  Instant with one candidate; the AI model decides when there are several." ),
                                 "Nombra un arma o herramienta que lleve o vea.  Inmediato con un candidato; con varios decide el modelo de IA." ) } );
    catalogue.push_back( { menu_order::drop, G::items, menu_order_target::item_words, true, 'd',
                           text( _( "Drop an item…" ), "Soltar un objeto…" ),
                           text( _( "Name something the companion carries." ),
                                 "Nombra algo que el compañero lleve encima." ) } );
    catalogue.push_back( { menu_order::wear, G::items, menu_order_target::item_words, true, 'e',
                           text( _( "Wear an item…" ), "Ponerse una prenda…" ),
                           text( _( "Name clothing, a backpack or gear to put on." ),
                                 "Nombra una prenda, mochila o equipo para ponerse." ) } );
    catalogue.push_back( { menu_order::take_off, G::items, menu_order_target::item_words, true, 't',
                           text( _( "Take off an item…" ), "Quitarse una prenda…" ),
                           text( _( "Name a worn item to remove." ),
                                 "Nombra una prenda puesta para quitársela." ) } );
    catalogue.push_back( { menu_order::store, G::items, menu_order_target::item_words, true, 's',
                           text( _( "Put away an item…" ), "Guardar un objeto…" ),
                           text( _( "Name the wielded item to holster or stow." ),
                                 "Nombra el objeto empuñado para enfundarlo o guardarlo." ) } );
    catalogue.push_back( { menu_order::recover, G::items, menu_order_target::item_words, true, 'r',
                           text( _( "Recover lost gear…" ), "Recuperar equipo perdido…" ),
                           text( _( "Say \"backpack\", \"weapon\" or \"helmet\"; the companion retrieves the one it dropped." ),
                                 "Di \"mochila\", \"arma\" o \"casco\"; el compañero recupera el que dejó caer." ) } );

    // Tasks
    catalogue.push_back( { menu_order::start_fire, G::tasks, menu_order_target::none, false, 'l',
                           text( _( "Light a fire" ), "Encender fuego" ),
                           text( _( "Light the stove or fireplace in sight." ),
                                 "Encender la cocina o chimenea a la vista." ) } );
    catalogue.push_back( { menu_order::unload_vehicle, G::tasks, menu_order_target::none, false, 'u',
                           text( _( "Unload the vehicle" ), "Descargar el vehículo" ),
                           text( _( "Carry the cargo out of the vehicle in sight." ),
                                 "Sacar la carga del vehículo a la vista." ) } );
    catalogue.push_back( { menu_order::watch, G::tasks, menu_order_target::item_words, false, 'v',
                           text( _( "Watch for an item…" ), "Vigilar un objeto…" ),
                           text( _( "The companion warns you, in magenta, when it sees the named object.  Matches by word against item names and ids, with the synonyms in data/npc_ai/watch_synonyms.txt; no AI model." ),
                                 "El compañero avisa, en magenta, cuando vea el objeto nombrado.  Casa por palabra con nombres e ids, con los sinónimos de data/npc_ai/watch_synonyms.txt; sin modelo de IA." ) } );
    return catalogue;
}

std::optional<menu_order_entry> find_order_menu_entry( const menu_order id )
{
    for( const menu_order_entry &entry : order_menu_catalogue() ) {
        if( entry.id == id ) {
            return entry;
        }
    }
    return std::nullopt;
}

std::string order_menu_phrase( const menu_order id, const std::string &raw_target )
{
    const std::string target = trim_target( raw_target );
    // The phrases are Spanish because every keyword list accepts the Spanish
    // forms regardless of the dialogue language (the model-backed intent path
    // in npc_ai_order_intent.cpp relies on the same fact).
    switch( id ) {
        case menu_order::follow:
            return "Vengan conmigo.";
        case menu_order::guard:
            return "Quedense aqui.";
        case menu_order::enter_interior:
            return "Todos adentro.";
        case menu_order::pickup_all_food:
            return "Recoge toda la comida.";
        case menu_order::search_food:
            return "Busca comida.";
        case menu_order::search_item:
            return target.empty() ? std::string() : "Busca y trae " + target + ".";
        case menu_order::start_fire:
            return "Haz fuego.";
        case menu_order::unload_vehicle:
            return "Descarga el vehiculo.";
        case menu_order::pickup:
            return target.empty() ? std::string() : "Recoge " + target + ".";
        case menu_order::wield:
            return target.empty() ? std::string() : "Empuna " + target + ".";
        case menu_order::drop:
            return target.empty() ? std::string() : "Suelta " + target + ".";
        case menu_order::wear:
            return target.empty() ? std::string() : "Ponte " + target + ".";
        case menu_order::take_off:
            return target.empty() ? std::string() : "Quitate " + target + ".";
        case menu_order::store:
            return target.empty() ? std::string() : "Guarda " + target + ".";
        case menu_order::recover:
            return target.empty() ? std::string() : "Recupera tu " + target + ".";
        case menu_order::drag_casualty:
            return target.empty() ? std::string() : "Arrastra a " + target + ".";
        case menu_order::watch:
            return target.empty() ? std::string() : "Avisame si ves " + target + ".";
    }
    return std::string();
}

std::string order_menu_target_prompt( const menu_order id )
{
    switch( id ) {
        case menu_order::pickup:
            return text( _( "Which object should they pick up?" ), "¿Qué objeto debe recoger?" );
        case menu_order::search_item:
            return text( _( "What should they search for?" ), "¿Qué objeto debe buscar?" );
        case menu_order::wield:
            return text( _( "Which item should they wield?" ), "¿Qué objeto debe empuñar?" );
        case menu_order::drop:
            return text( _( "Which item should they drop?" ), "¿Qué objeto debe soltar?" );
        case menu_order::wear:
            return text( _( "Which item should they put on?" ), "¿Qué prenda debe ponerse?" );
        case menu_order::take_off:
            return text( _( "Which item should they take off?" ), "¿Qué prenda debe quitarse?" );
        case menu_order::store:
            return text( _( "Which item should they put away?" ), "¿Qué objeto debe guardar?" );
        case menu_order::recover:
            return text( _( "Which gear should they recover? (backpack, weapon, helmet)" ),
                         "¿Qué equipo debe recuperar? (mochila, arma, casco)" );
        case menu_order::drag_casualty:
            return text( _( "Who should be dragged?" ), "¿A quién hay que arrastrar?" );
        case menu_order::watch:
            return text( _( "Which object should they watch for?" ), "¿Qué objeto debe vigilar?" );
        case menu_order::follow:
        case menu_order::guard:
        case menu_order::enter_interior:
        case menu_order::pickup_all_food:
        case menu_order::search_food:
        case menu_order::start_fire:
        case menu_order::unload_vehicle:
            break;
    }
    return std::string();
}

} // namespace npc_ai
