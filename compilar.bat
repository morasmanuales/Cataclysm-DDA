@echo off
setlocal EnableExtensions

rem ============================================================================
rem  compilar.bat  -  compilacion Release x64 de Cataclysm-DDA (NPC AI)
rem
rem  Uso:
rem    compilar.bat                 -> compila solo los tests (por defecto)
rem    compilar.bat tests           -> idem
rem    compilar.bat juego           -> compila solo el juego (cataclysm-tiles.exe)
rem    compilar.bat todo            -> compila tests y juego
rem    compilar.bat tests run       -> compila tests y corre el gate [npc_ai]
rem    compilar.bat tests run ordenes
rem                                 -> compila tests y corre solo [npc_ai_orders]
rem                                    y [npc_ai_pickup]
rem
rem  Salidas (en la raiz del repo):
rem    Cataclysm-test-vcpkg-static-Release-x64.exe
rem    cataclysm-tiles.exe
rem
rem  Aviso benigno esperado del enlazador: LNK4315 /DEBUG:FASTLINK
rem ============================================================================

set "REPO=%~dp0"
if "%REPO:~-1%"=="\" set "REPO=%REPO:~0,-1%"
set "SLN=%REPO%\msvc-full-features\Cataclysm-vcpkg-static.sln"
set "TEST_EXE=%REPO%\Cataclysm-test-vcpkg-static-Release-x64.exe"

set "MODO=%~1"
if "%MODO%"=="" set "MODO=tests"
set "RUN=%~2"
set "FILTRO=%~3"

rem ---------------------------------------------------------------- MSBuild --
rem Las rutas con "(x86)" no pueden expandirse dentro de bloques entre
rem parentesis, por eso la busqueda va en una subrutina sin bloques.
set "MSBUILD="
set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" call :buscar_msbuild
if not defined MSBUILD if exist "E:\Visual Studio\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=E:\Visual Studio\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD if exist "%PF86%\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%PF86%\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
if not defined MSBUILD (
    echo [ERROR] No se encontro MSBuild.exe. Instala las Build Tools de Visual Studio o edita la ruta en este .bat.
    exit /b 1
)
if not exist "%SLN%" (
    echo [ERROR] No existe la solucion: "%SLN%"
    exit /b 1
)

echo MSBuild : %MSBUILD%
echo Solucion: %SLN%
echo Modo    : %MODO%
echo.

rem /nr:false: no dejar nodos de MSBuild vivos entre ejecuciones. Un nodo
rem residual de otra sesion puede reutilizarse con estado viejo y provocar
rem fallos que no se reproducen en una compilacion limpia.
set "COMUN=/p:Configuration=Release /p:Platform=x64 /m /nr:false /v:minimal /nologo"

rem ------------------------------------------------------------------ Tests --
if /i "%MODO%"=="tests" call :build "Cataclysm-test-vcpkg-static" || goto :fallo
if /i "%MODO%"=="todo"  call :build "Cataclysm-test-vcpkg-static" || goto :fallo

rem ------------------------------------------------------------------ Juego --
if /i "%MODO%"=="juego" call :build "Cataclysm-vcpkg-static" || goto :fallo
if /i "%MODO%"=="todo"  call :build "Cataclysm-vcpkg-static" || goto :fallo

if /i not "%MODO%"=="tests" if /i not "%MODO%"=="juego" if /i not "%MODO%"=="todo" (
    echo [ERROR] Modo desconocido "%MODO%". Usa: tests ^| juego ^| todo
    exit /b 1
)

rem ------------------------------------------------------------- Ejecutar ---
if /i not "%RUN%"=="run" goto :ok
if /i "%MODO%"=="juego" (
    echo [AVISO] "run" solo aplica a los tests.
    goto :ok
)
if not exist "%TEST_EXE%" (
    echo [ERROR] No existe "%TEST_EXE%"
    goto :fallo
)
pushd "%REPO%"
if /i "%FILTRO%"=="ordenes" (
    echo.
    echo === Tests: [npc_ai_orders] ===
    "%TEST_EXE%" "[npc_ai_orders]" --rng-seed 1 || set "TESTFALLO=1"
    echo.
    echo === Tests: [npc_ai_pickup] ===
    "%TEST_EXE%" "[npc_ai_pickup]" --rng-seed 1 || set "TESTFALLO=1"
) else (
    echo.
    echo === Gate: [npc_ai] ===
    "%TEST_EXE%" "[npc_ai]" --rng-seed 1 || set "TESTFALLO=1"
)
popd
if defined TESTFALLO (
    echo.
    echo [ERROR] Hay tests fallidos.
    exit /b 2
)

:ok
echo.
echo [OK] Terminado sin errores.
exit /b 0

:fallo
echo.
echo [ERROR] La compilacion fallo.
exit /b 1

rem ----------------------------------------------------------- subrutinas ---
:buscar_msbuild
rem vswhere se invoca por nombre desde su carpeta: su ruta contiene "(x86)" y
rem ese parentesis rompe el "in (...)" del for.
pushd "%PF86%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%I in (`.\vswhere.exe -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe"`) do if not defined MSBUILD set "MSBUILD=%%I"
popd
exit /b 0

:build
echo === Compilando %~1 ===
"%MSBUILD%" "%SLN%" /t:"%~1" %COMUN%
exit /b %ERRORLEVEL%
