# GePatch + Adrenaline 2026.10.30

GePatch и Adrenaline для запуска PSP-игр в **960×544** на PS Vita.

Исправил разрывы изображения при затемнениях и переходах, доработал обработку
графики и вывод кадров. Проверял на своей Vita в Tekken 6.

GePatch предусматривает работу с обычным Adrenaline — обновление необязательно.
Исправления stencil-теней и tearing требуют модифицированного
`adrenaline_user.suprx` из релиза, предназначенного для **Adrenaline 8.0.2**.
Другие версии Adrenaline и совместимость с ABM пока не проверены.

Для установки обоих модулей скачайте
`GePatch-Adrenaline-2026.10.30-update.zip`, сохраните копии старых модулей
и замените файлы:

- `ge_patch.prx` → `ux0:/pspemu/seplugins/ge_patch.prx`
- `adrenaline_user.suprx` → `ux0:/app/PSPEMUCFW/sce_module/adrenaline_user.suprx`

Если оставляете обычный Adrenaline, замените только `ge_patch.prx`.

В `ux0:/pspemu/seplugins/game.txt` включите `ms0:/seplugins/ge_patch.prx 1`
и полностью перезапустите Adrenaline.

В некоторых играх возможны графические ошибки и проблемы с производительностью.
Продолжаю работать над совместимостью.

Спасибо TheOfficialFloW, isage и остальным авторам оригинальных проектов.
