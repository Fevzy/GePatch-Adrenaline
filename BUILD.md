# Сборка

## GePatch

Нужен PSPDEV с PSPSDK и psp-cfw-sdk (SystemCtrl kernel stubs).
В Linux/WSL, из каталога `gepatch`:

```sh
PSPDEV=/path/to/pspdev GP_LOG=0 GP_PROFILE=0 ./build_wsl.sh
```

Добавьте `$PSPDEV/bin` в PATH при самостоятельном запуске make.
Debug: `GP_LOG=1 GP_PROFILE=0`; profile: `GP_LOG=1 GP_PROFILE=1`.
Выходной файл: `gepatch/ge_patch.prx`. Релиз собран PSPDEV из рабочего окружения
и имеет SHA256 из `release/SHA256SUMS`.

Host-тесты в Windows x64: Python 3, Visual Studio с MSVC C/C++.

```bat
scripts\test_gepatch.cmd
```

Скрипт `scripts/msvc_env.cmd` находит Visual Studio через vswhere;
установленная компонента MSVC обязательна. Ожидается 16 PASS.
Указатели на host имеют размер 8 байт. Результат пишется в `build-tools`.

## Adrenaline

Исходники находятся в `adrenaline/src`; базовый проект — isage/Adrenaline 8.0.2.
Нужны VitaSDK, PSPDEV/psp-cfw-sdk, CMake >= 3.28, make, Python 3 и SDK-библиотеки
из `adrenaline/src/user/CMakeLists.txt` и `kernel/CMakeLists.txt`.
Исходная система сборки загружает libvita2d (fbo) и vita-shader-collection.

Типовая сборка в окружении с обоими SDK:

```sh
export VITASDK=/path/to/vitasdk
export PSPDEV=/path/to/pspdev
export PATH="$VITASDK/bin:$PSPDEV/bin:$PATH"
cd adrenaline/src
make
```

Полный проект сначала собирает PSP CEF resources, затем Vita-модули и VPK.
Сохранённые CFW resources исходной рабочей сборки приложены в `user/flash0`;
их исходники находятся в `cef`. Модифицированный user module использует
`gerender.c`, `getap.c` и соответствующие заголовки, включённые в репозиторий.

`adrenaline/build_vita.sh` сохранён как локальный рецепт Windows/Git Bash
для VitaSDK в `C:/vitasdk`. Он рассчитан на подготовленные CEF resources.
Для создания kernel stub libraries в Windows нужен GNU make; проект публикации
не использует временные пути исходного рабочего окружения.

Бинарник `release/adrenaline_user.suprx` взят из замороженной, подтверждённой
на устройстве сборки. Его повторная сборка в чистом окружении ещё не проверена.
Собранные заново файлы нельзя считать проверенными на Vita по одному факту сборки.

## Упаковка сохранённого релиза

Из чистого Git checkout: `python scripts/package_release.py`.
Скрипт проверяет хеши сохранённых модулей, создаёт update/source ZIP и SHA256SUMS
в `dist`, проверяет CRC архивов и хеши вложенных бинарников. Он не собирает
новые модули и не обращается к Vita.
