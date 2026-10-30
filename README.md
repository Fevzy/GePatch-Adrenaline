# GePatch + Adrenaline

[Русский](README.md) | [English](README_EN.md)

GePatch и Adrenaline для запуска PSP-игр в **960×544** на PS Vita.

Это независимая экспериментальная модификация. Авторы исходных проектов:
[TheOfficialFloW/GePatch](https://github.com/TheOfficialFloW/GePatch) и
[isage/Adrenaline](https://github.com/isage/Adrenaline), основанный на Adrenaline TheFloW.

## Релиз 2026.10.30

В релизе два модуля:

| Файл | SHA256 (начало) | Назначение |
|---|---|---|
| `ge_patch.prx` | `091579af` | PSP kernel plugin |
| `adrenaline_user.suprx` | `476689be` | Vita-side user module |

Полные хеши: [release/SHA256SUMS](release/SHA256SUMS).

Исправил разрывы изображения при затемнениях и переходах, доработал обработку
графики и вывод кадров. Проверял на своей Vita в Tekken 6.

## Совместимость с Adrenaline

GePatch предусматривает работу с обычным Adrenaline — для использования самого
плагина обновлять Adrenaline необязательно. Исправление теней, использующих
stencil-маску, и устранение tearing через два буфера требуют модифицированного
`adrenaline_user.suprx` из релиза. Этот модуль предназначен для **Adrenaline 8.0.2**.

Другие версии и форки Adrenaline, а также совместимость с ABM пока не проверены.

## Установка обновления

Для установки обоих модулей нужен **Adrenaline 8.0.2**. Комплект обновляет два модуля;
полный установочный VPK и прошивка PSP в него не входят.

1. Полностью закройте Adrenaline.
2. Сохраните копии существующих файлов `ge_patch.prx`, `adrenaline_user.suprx`
   и `ux0:/pspemu/seplugins/game.txt` для отката.
3. Скопируйте `ge_patch.prx` в `ux0:/pspemu/seplugins/ge_patch.prx`.
4. Скопируйте `adrenaline_user.suprx` в
   `ux0:/app/PSPEMUCFW/sce_module/adrenaline_user.suprx`.
5. В `ux0:/pspemu/seplugins/game.txt` добавьте или включите строку
   `ms0:/seplugins/ge_patch.prx 1`. Существующие записи сохраните.
6. Полностью перезапустите Adrenaline. Для отката закройте его и верните
   сохранённые модули и конфигурацию.

Если оставляете обычный Adrenaline, пропустите замену `adrenaline_user.suprx`.
Для полного набора исправлений установите оба модуля из одного релиза.
При передаче файлов проверьте SHA256 копий.

## Изменения

- Рендеринг в 960×544.
- Два display buffers с публикацией номера готового буфера командами GE.
- Длинные display lists обрабатываются на месте; короткие копируются
  и поэтапно отпускаются GE с удержанием потенциально изменяемых inset-команд.
- Сохранение и восстановление изменяемых вершин и команд, коррекция UV inset,
  обработка stencil/формата framebuffer.
- Ожидание VBlank не удерживает игру, отстающую от расписания.

## Известные проблемы

В некоторых играх возможны графические ошибки и проблемы с производительностью.
Продолжаю работать над совместимостью.

## Сборка и тесты

Инструкции: [BUILD.md](BUILD.md). Тесты используют синтетические GE lists,
не запускают игры. Итоговую картинку и скорость проверяет человек на Vita;
для Tekken 6 скорость оценивается по таймеру раунда, а не числу нарисованных кадров.

## Лицензии и происхождение

Сохранены исходные уведомления и лицензии:
[GePatch](gepatch/LICENSE.TXT), [Adrenaline GPLv3](adrenaline/src/LICENSE),
а также лицензии вложенных библиотек. Сведения о базовых исходниках и бинарниках:
[release/manifest.json](release/manifest.json),
[GePatch upstream manifest](gepatch/UPSTREAM_MANIFEST.json).
Исходный commit Adrenaline: `6c2ab8c79679b021de230f28da8278518c410bea`.
Игры, образы дисков, сохранения и прошивка Sony в репозиторий не включены.
