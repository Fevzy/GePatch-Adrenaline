@echo off
setlocal
cd /d "%~dp0.."
if not exist build-tools mkdir build-tools
python gepatch\tests\generate_reference.py
if errorlevel 1 exit /b 1
call scripts\msvc_env.cmd cl /nologo /TC /std:c11 /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /wd4311 /wd4312 /Igepatch\tests\stubs /Ibuild-tools /Fe:build-tools\test_gepatch.exe /Fo:build-tools\ gepatch\tests\test_gepatch.c gepatch\vertex_restore.c gepatch\uv_inset.c gepatch\guard_band.c > build-tools\gepatch_test_build.log 2>&1
if errorlevel 1 (
  type build-tools\gepatch_test_build.log
  exit /b 1
)
build-tools\test_gepatch.exe > build-tools\gepatch_test_results.log 2>&1
if errorlevel 1 (
  type build-tools\gepatch_test_results.log
  exit /b 1
)
type build-tools\gepatch_test_results.log
