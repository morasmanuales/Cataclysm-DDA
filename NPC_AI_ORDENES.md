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

## Menú de órdenes (sin escribir)

Rama `feature/action-menu-miguel-actions` (07/09/2026). El menú **Charlar
con PNJ** (tecla `C`) empieza con un bloque **Compañeros (IA)**: "Hablar con
naturalidad" (`I`, lo mismo que la tecla `q` de Miguel) y **Dar órdenes a los
compañeros…** (`O`). Debajo, bajo la cabecera "Otros", sigue el menú vanilla.
Mientras haya compañeros IA a la vista, las entradas vanilla "que siga" (`f`)
y "que vigile" (`g`) se ocultan porque las cubren las órdenes IA; "mover a
posición" (`G`) se mantiene. El submenú de órdenes se agrupa en Movimiento,
Objetos y Tareas con cabeceras. Las órdenes que C++ ya sabe ejecutar:

| Entrada | Frase canónica que se envía | Destinatario |
|---|---|---|
| Síganme | `Vengan conmigo.` | uno o todos |
| Vigilen esta posición | `Quedense aqui.` | uno o todos |
| Entren al edificio | `Todos adentro.` (al llegar dejan de seguirte y vigilan esa casilla; "Síganme" los recupera) | uno o todos |
| Esconderse | `Escondanse.` (cada compañero va a SU habitación cerrada más cercana: muros, ventanas cerradas y al menos una puerta; dentro elige una casilla a cubierto de las puertas (mostrador, mesa o esquina entre ella y la puerta) y, si no hay, la más lejos de las puertas; cierra la puerta al pasar y al llegar; deja de atacar, ignora ruidos, se tumba (tumbado tras un mueble con cobertura mayor que 33 nadie lo ve; de pie sí) y vigila esa casilla. Se levanta para reubicarse, para defenderse y al cancelar. Elige la casilla a cubierto tanto de las puertas como de las ventanas intactas. Si un enemigo entra en la habitación a distancia, busca otra habitación cerrada (5 turnos de enfriamiento entre saltos); si no queda ninguna, o si el enemigo ya está pegado a él (también de camino al escondite), se levanta y se defiende donde está; tras 10 turnos sin enemigos a la vista vuelve a esconderse, o retoma la marcha si aún no había llegado. Mientras está escondido sin nadie en la habitación se le quita el pánico vanilla (`npc_run_away`), así lo que vea por la ventana no lo saca del escondite. Sin ninguna habitación cerrada alcanzable: "No encuentro dónde encerrarme, me escondo aquí" y usa la casilla interior más lejos de los enemigos a la vista. Reglas forzadas mientras dura: no enfrentar, cerrar puertas, ignorar ruidos, solo armas silenciosas, y se le permite abrir puertas aunque el jugador lo tuviera prohibido (lo avisa). Se reafirman cada turno, así "Relajarse (limpiar anulaciones)" no lo deja peleando sin querer; al terminar se restauran exactamente las anulaciones que tenía antes, incluidas las de "Prepararse para el peligro". "Síganme" o "Quédense aquí" terminan el escondite; con "Síganme", si sigue dentro de la habitación, primero camina hasta la puerta más cercana al jugador y la abre, porque el seguimiento vanilla se queda quieto cuando el camino hasta el jugador es corto y un compañero tras una puerta cerrada no saldría solo; sin modelo) | uno o todos |
| Recoger un objeto… | `Recoge <objeto>.` | uno o todos |
| Recoger toda la comida | `Recoge toda la comida.` | uno o todos (uno por uno) |
| Buscar comida | `Busca comida.` (recorre un radio de 10 casillas, atravesando puertas, revisa pilas y contenedores no sellados como neveras, armarios y estantes, y recoge la comida que encuentra; sin modelo; al terminar informa de sitios revisados y de lo recogido por nombre y cantidad) | uno o todos (uno por uno) |
| Buscar un objeto… | `Busca y trae <objeto>.` (mismo recorrido; el nombre se normaliza a minúsculas sin acentos ni artículos y se exige que todas sus palabras aparezcan en el nombre o id del objeto; se detiene en cuanto tiene el objeto en la mano; informe final igual) | uno o todos (uno por uno) |

Durante el recorrido el compañero deja de seguir al jugador (actitud neutral,
sin seguir ni vigilar: el seguimiento lo devolvería al jugador y un guardia es
estacionario y no recoge nada); recupera el modo seguir al emprender la vuelta
o si otra orden cancela la búsqueda.

Distancia de ambas búsquedas: al elegirlas, el menú pregunta hasta dónde
buscar, y la frase hablada admite el mismo sufijo:

| Opción | Frase | Radio | Sitios máx. |
|---|---|---|---|
| Cerca | `... cerca.` | 6 casillas | 30 |
| Mediano (por defecto) | `... a media distancia.` o sin sufijo | 10 casillas | 40 |
| Lejos | `... lejos.` | 16 casillas | 80 |
| Solo dentro de casa | `... dentro de casa.` | el edificio donde está el compañero: casillas con techo alcanzables sin pisar el exterior, hasta 20 | 80 |

Con "dentro de casa" y el compañero en la calle, la orden se rechaza ("Ahora
mismo no estoy dentro de un edificio").

Recorrido de ambas búsquedas: antes de dar un paso, el compañero examina todo
lo que puede desde donde está (pilas a la vista y el contenedor que tenga al
lado) y recoge lo que coincida; solo camina hacia lo que no puede ver desde
ahí, eligiendo siempre el sitio pendiente más cercano a su posición actual.
Cada sitio revisado, inalcanzable o en el que se atascó queda anotado y no se
vuelve a visitar. Al terminar, el compañero vuelve a seguir al jugador (si
estaba de guardia deja de estarlo), camina hasta la posición que el jugador
ocupa en ese momento y da el informe al llegar junto a él (o al acabar esa
marcha; si tarda más de 600 turnos lo dice donde esté). En "Buscar un
objeto…" con "Todos", el primer compañero que tiene el objeto en la mano
cierra la búsqueda de los demás que buscaban lo mismo: todos vuelven al
jugador y los otros informan de quién lo encontró.
| Empuñar un objeto… | `Empuna <objeto>.` | uno o todos (uno por uno) |
| Soltar un objeto… | `Suelta <objeto>.` | uno o todos |
| Ponerse una prenda… | `Ponte <objeto>.` | uno o todos |
| Quitarse una prenda… | `Quitate <objeto>.` | uno o todos |
| Guardar un objeto… | `Guarda <objeto>.` | uno o todos |
| Recuperar equipo perdido… | `Recupera tu <mochila/arma/casco>.` | uno o todos |
| Arrastrar a un herido… | `Arrastra a <nombre>.` (el herido se elige de una lista) | uno o todos |
| Encender fuego | `Haz fuego.` | uno o todos (uno por uno) |
| Descargar el vehículo | `Descarga el vehiculo.` | uno o todos (uno por uno) |
| Vigilar un objeto… | `Avisame si ves <objeto>.` | uno o todos (uno por uno) |

Flujo: orden → destinatario (el selector es el mismo de la conversación IA y
siempre ofrece "todos" cuando hay más de un compañero) → objetivo si hace
falta (texto libre o lista de compañeros). Las órdenes marcadas "uno por uno"
no tienen manejador grupal: con "todos", el menú envía la misma frase a cada
compañero por separado, así que cada uno se comporta igual que si se la
hubieras dicho a él solo (varios pueden ir al mismo objeto o a la misma
cocina; el primero que llega lo hace y los demás informan). La
frase resultante entra por `game::ai_dispatch_player_line`, la misma función
por la que pasa el texto escrito en la conversación IA, así que el
comportamiento, los mensajes y el registro son idénticos a haber tecleado la
frase. El modelo sigue sin decidir nada: para recoger/empuñar puede seguir
eligiendo el objeto concreto entre candidatos reales, como hoy.

### Qué entradas siguen usando el modelo

Las marcadas "(IA)" en el menú pueden hacer una petición al modelo, igual que
cuando se escriben:

- **Recoger un objeto…**: si las palabras del jugador nombran exactamente un
  objeto visible (nombre completo, o una palabra de la orden dentro del nombre
  o id del objeto), se ejecuta al instante sin petición
  (`RESOLVER=deterministic_unique_match` en el log de pickup). Con cero o
  varias coincidencias el modelo elige entre los candidatos reales, como
  antes. Los objetos con etiqueta de propietario del NPC no usan el atajo:
  ese flujo pertenece a la recuperación de equipo.
- **Empuñar un objeto…**: con un solo candidato es determinista; con varios
  decide el modelo (comportamiento previo, sin cambios).
- **Vigilar un objeto…**: ya no usa el modelo. Las palabras del jugador,
  normalizadas (minúsculas, sin acentos, sin relleno), forman un selector
  `@idlike:` que casa por palabra completa (con tolerancia de plural) contra
  el nombre visible y el id de cada objeto a la vista; se amplía con los
  grupos de sinónimos de `data/npc_ai/watch_synonyms.txt` (editable sin
  recompilar) y con las categorías fijas: "cargador" → `@MAGAZINE`,
  "munición/balas/cartuchos" → `@AMMO`, "arma de fuego/pistola/rifle/escopeta"
  → `@GUN`. Una petición de varias palabras ("panel solar") exige todas las
  palabras en el mismo objeto, en cualquier orden, así "panel de madera" no
  dispara el aviso. La vigilancia cubre objetos sueltos y también cosas
  instaladas: piezas de vehículo (un panel solar montado en un coche, una
  rueda) y muebles (una nevera, un banco de trabajo). El aviso al encontrarlo
  se muestra en magenta; una sola vez por encargo.

Todas las demás entradas no crean ninguna petición: sus manejadores no llaman
a `enqueue_*`.

Archivos: `src/npc_ai_order_menu.h/.cpp` (catálogo, frases y textos),
`src/npctalk.cpp` (`game::ai_orders_menu`, y `ai_talk` refactorizado para
compartir `ai_dispatch_player_line`; entrada `NPC_CHAT_AI_ORDERS` en
`game::chat`), `src/game.h`,
`tests/npc_ai_order_menu_test.cpp` (catálogo cerrado, frases reconocidas por
cada parser, manejadores de tarea que reclaman su frase),
`src/npc_ai_pickup.cpp` (atajo determinista por coincidencia única de nombre y
cola compartida `begin_directed_pickup` para ambas rutas) y
`tests/npc_ai_equipment_test.cpp` (`npc_ai_pickup_unique_name_match_skips_the_model`).

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
- `src/npc_ai_hide.*`: orden "Escondanse": búsqueda de habitación cerrada
  (inundación sin cruzar puertas; fuga hacia fuera = no cerrada), anulaciones
  temporales de `forbid_engage`, `close_doors` e `ignore_noise` (solo las que
  el jugador no tenía forzadas), llegada como puesto de guardia, reubicación al
  ser descubierto y defensa propia cuando no hay otro escondite. Enganches en
  `npcmove.cpp` (`process_hide` por turno), `npctalk.cpp` (despacho) y
  `npc_ai_tactical.cpp` (`cancel_hide`).
  Traza en tiempo real con `CDDA_NPC_AI_DEBUG=1` en
  `npc_ai_hide_v1_runtime.txt` (carpeta del usuario): ORDER, GOTO, TICK de
  cada turno en marcha (destino, distancia, si conserva la orden de ir, si
  sigue, movimientos), ARRIVED, CORNERED y CANCEL.
- `tests/npc_ai_hide_test.cpp`: parser, habitación cerrada frente a techo sin
  muros, casilla más profunda, cierre de puerta al llegar, reubicación,
  acorralado y vuelta a la calma, grupo con casillas distintas, anulaciones
  propias del jugador intactas.
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
