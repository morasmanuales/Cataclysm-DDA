# Reglas locales del proyecto (no se comparten)

- **No compilar sin que se pida.** Nunca lanzar MSBuild ni ninguna compilacion
  del juego o de los tests (`Cataclysm-vcpkg-static.sln`, `*.vcxproj`) salvo
  que el usuario lo pida explicitamente en el mensaje actual. Escribir codigo,
  tests y documentacion esta bien; al terminar, decir que queda sin compilar y
  ofrecer compilar. Si un paso requiere el binario (tests, escenarios en vivo),
  pedir permiso antes.
- **No commit ni push sin consultar**, salvo que el usuario lo pida en el turno.
- Claves de API: nunca en codigo, opciones ni logs. Solo variables de entorno o
  archivos en `config\`.
