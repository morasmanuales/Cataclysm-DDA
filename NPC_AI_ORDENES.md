# Órdenes habladas en dos etapas

Rama `feature/npc-ai-dynamic-orders` (03/09/2026). Resuelve que "Síganme" no
hiciera nada mientras "Vengan conmigo" sí, sin convertir cada nueva forma de
decirlo en una línea más de una lista.

## Cómo funciona

1. **Vía rápida, como siempre.** Las listas de frases de `npc_ai_tactical.cpp`
   (seguir, vigilar), `npc_ai_interior.cpp` (entrar), recogida, empuñar,
   equipo, rescate, vigilancia, fuego y descarga se consultan primero. Si una
   coincide, se ejecuta al instante sin tocar el modelo. Se ampliaron las
   listas de seguir y vigilar con las formas habituales en singular, plural y
   voseo ("siganme", "seguidme", "pegaos a mi", "esperadme aqui"...).
2. **Clasificador de intención, solo si nada coincidió.** Si la frase pasa una
   puerta barata (sin signo de interrogación, de 1 a 10 palabras, y el Context
   Router la clasifica como GENERAL, es decir no es saludo, salud, percepción
   ni memoria), se manda al modelo con un catálogo cerrado:

   | Id | Significa | Ejecuta |
   |---|---|---|
   | FOLLOW | venir con el jugador, no quedarse atrás | lo mismo que "vengan conmigo" |
   | GUARD | quedarse, esperar, vigilar la posición | lo mismo que "quédense aquí" |
   | ENTER_INTERIOR | entrar o refugiarse en un edificio | lo mismo que "todos adentro" |
   | PICKUP | recoger un objeto (TARGET) | `try_handle_pickup_command` con "Recoge <TARGET>." |
   | WIELD | empuñar o sacar algo (TARGET) | `try_handle_wield_command` |
   | DROP | soltar algo (TARGET) | `try_handle_equipment_command` con "Suelta <TARGET>." |
   | WEAR | ponerse algo (TARGET) | `try_handle_equipment_command` con "Ponte <TARGET>." |
   | NONE | charla, pregunta, opinión, plan futuro | la frase sigue al diálogo normal |

   El modelo devuelve exactamente `ORDER=<id>` y `TARGET=<palabras del
   jugador>`. C++ acepta solo ids del catálogo; cualquier otra cosa se trata
   como NONE. Si el id es una orden pero el manejador determinista no
   encuentra nada real (no hay tal objeto), tampoco se ejecuta nada y la
   frase pasa a diálogo, donde el NPC puede decir que no ve ese objeto.

## Lo que no cambia

- El modelo nunca decide qué acciones existen ni las ejecuta. Elige una
  entrada de una lista que C++ ya sabía ejecutar.
- Las preguntas nunca llegan al clasificador. "¿Me seguirías al fin del
  mundo?" es diálogo.
- Coste: una petición de unos 350 tokens solo cuando la vía rápida falla en una
  frase corta e imperativa. Con DeepInfra, alrededor de un segundo y una
  fracción de centavo.
- Con "todos" seleccionado, la orden se aplica al grupo igual que por la vía
  rápida; el clasificador recibe el número de compañeros como contexto.

## Archivos

- `src/npc_ai_order_intent.h/.cpp`: puerta, prompt, parser y ejecución.
- `src/npc_ai_async.*`: tipo de petición `order_resolution` y su despacho.
- `src/npc_ai_context.*`: propósito `order_resolution` con system prompt de
  clasificador estricto (es/en).
- `src/npc_ai_tactical.*`: `execute_tactical_order` acepta la orden ya
  clasificada; listas de seguir y vigilar ampliadas.
- `src/npctalk.cpp`: enganche antes del diálogo individual y del grupal.
- `tests/npc_ai_order_intent_test.cpp`: puerta, parser, ejecución con
  ejecutor falso (FOLLOW cambia la misión del NPC; NONE y salidas inválidas
  reenvían a diálogo sin ejecutar nada) y test oculto en vivo
  `[.npc_ai_live_orders]`.

## Medido

Clasificador en vivo con Qwen3-14B en DeepInfra, 16 frases (10 órdenes de
los 7 tipos y 6 frases de conversación que no deben ser órdenes): **16/16**.
Gate `[npc_ai] --rng-seed 1`: 215 casos / 4306 aserciones, PASS.

## Pendiente

- RESCUE no está en el catálogo: el manejador de rescate necesita el callback
  de la interfaz para elegir posición, que no existe al aplicar una
  completion asíncrona.
- Órdenes compuestas ("coge la mochila y sígueme") se resuelven como una sola.
- Medir en partida cuántas frases llegan al clasificador frente a la vía
  rápida, para ajustar la puerta.
