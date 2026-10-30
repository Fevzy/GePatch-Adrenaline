# GePatch + Adrenaline 2026.10.30

Моя доработка GePatch и Adrenaline для PSP-игр в **960×544 на PS Vita**.

Исправил разрывы изображения при затемнениях и переходах, доработал обработку
графики и вывод кадров. Проверял на своей Vita в Tekken 6.

Для установки нужен **Adrenaline 8.0.2**. Скачайте
`GePatch-Adrenaline-2026.10.30-update.zip`, сохраните копии старых модулей
и замените файлы:

- `ge_patch.prx` → `ux0:/pspemu/seplugins/ge_patch.prx`
- `adrenaline_user.suprx` → `ux0:/app/PSPEMUCFW/sce_module/adrenaline_user.suprx`

В `ux0:/pspemu/seplugins/game.txt` включите `ms0:/seplugins/ge_patch.prx 1`
и полностью перезапустите Adrenaline.

В Dynasty Warriors пока остаются серые участки земли — над этим ещё работаю.

Спасибо TheOfficialFloW, isage и остальным авторам оригинальных проектов.
