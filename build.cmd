@echo off
setlocal
rem Builds ml1250.exe. Uses clang or gcc if on PATH, otherwise MSVC (cl).
if not exist dist mkdir dist

where clang >nul 2>&1 && set CC=clang
if "%CC%"=="" ( where gcc >nul 2>&1 && set CC=gcc )

if not "%CC%"=="" (
  echo Building with %CC%
  %CC% -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -o dist\ml1250.exe src\*.c -lsetupapi -lwinspool || exit /b 1
  %CC% -O1 -std=c11 -Wall -Wextra -o dist\infcheck.exe tests\infcheck.c -lsetupapi || exit /b 1
  goto done
)

where cl >nul 2>&1 || (
  echo No C compiler found. Install one of:
  echo   llvm-mingw   https://github.com/mstorsjo/llvm-mingw/releases
  echo   w64devkit    https://github.com/skeeto/w64devkit/releases
  echo   or open a "Developer Command Prompt for VS" and re-run this.
  exit /b 1
)
echo Building with MSVC
cl /nologo /O2 /W3 /Fe:dist\ml1250.exe src\*.c setupapi.lib winspool.lib || exit /b 1
cl /nologo /O2 /W3 /Fe:dist\infcheck.exe tests\infcheck.c setupapi.lib || exit /b 1

:done
echo.
echo Built dist\ml1250.exe
