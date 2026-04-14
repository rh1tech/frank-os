/*
 * FRANK OS — Localization / Language Support
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "lang.h"
#include "settings.h"

/* English string table */
static const char *str_en[] = {
    /* Common buttons */
    [STR_OK]             = "OK",
    [STR_CANCEL]         = "Cancel",
    [STR_CLOSE]          = "Close",
    [STR_YES]            = "Yes",
    [STR_NO]             = "No",

    /* Start menu */
    [STR_START]          = "Start",
    [STR_PROGRAMS]       = "Programs",
    [STR_SETTINGS]       = "Settings",
    [STR_FIRMWARE]       = "Firmware",
    [STR_RUN_DOTS]       = "Run...",
    [STR_REBOOT]         = "Reboot",

    /* Programs submenu */
    [STR_NAVIGATOR]      = "Navigator",
    [STR_TERMINAL]       = "Terminal",

    /* Settings submenu */
    [STR_CONTROL_PANEL]  = "Control Panel",
    [STR_NETWORK]        = "Network",
    [STR_LANGUAGE]       = "Language",

    /* System menu */
    [STR_RESTORE]        = "Restore",
    [STR_MOVE]           = "Move",
    [STR_MAXIMIZE]       = "Maximize",
    [STR_MINIMIZE]       = "Minimize",
    [STR_ENTER_FULLSCREEN] = "Enter Fullscreen",
    [STR_EXIT_FULLSCREEN]  = "Exit Fullscreen",

    /* Taskbar */
    [STR_VOLUME]         = "Volume",
    [STR_MUTE]           = "Mute",
    [STR_DISCONNECT]     = "Disconnect",

    /* Control Panel items */
    [STR_DESKTOP]        = "Desktop",
    [STR_SYSTEM]         = "System",
    [STR_MOUSE]          = "Mouse",
    [STR_FREQUENCIES]    = "Frequencies",

    /* Desktop Properties */
    [STR_BG_COLOR]       = "Background color:",
    [STR_PREVIEW]        = "Preview:",
    [STR_WINDOW_THEME]   = "Window theme:",
    [STR_DESKTOP_PROPS]  = "Desktop Properties",

    /* System Properties */
    [STR_SYSTEM_PROPS]   = "System Properties",

    /* Mouse Properties */
    [STR_DBLCLICK_SPEED] = "Double-click speed:",
    [STR_SLOW]           = "Slow",
    [STR_FAST]           = "Fast",
    [STR_TEST_AREA]      = "Test area:",
    [STR_MOUSE_PROPS]    = "Mouse Properties",

    /* Frequencies */
    [STR_CPU_FREQ]       = "CPU Frequency:",
    [STR_PSRAM_FREQ]     = "PSRAM Frequency:",
    [STR_REBOOT_NOTICE]  = "Changes take effect\nafter reboot.",

    /* Language */
    [STR_LANGUAGE_PROPS]   = "Language",
    [STR_SELECT_LANGUAGE]  = "Select language:",
    [STR_INPUT_TOGGLE]     = "Input toggle:",

    /* File dialog */
    [STR_LOOK_IN]        = "Look in:",
    [STR_FILE_NAME]      = "File name:",
    [STR_OPEN]           = "Open",
    [STR_SAVE]           = "Save",
    [STR_CONFIRM_SAVE_AS] = "Confirm Save As",

    /* Find/Replace */
    [STR_FIND]           = "Find",
    [STR_REPLACE]        = "Replace",
    [STR_FIND_NEXT]      = "Find Next",
    [STR_REPLACE_ALL]    = "Replace All",
    [STR_FIND_WHAT]      = "Find what:",
    [STR_REPLACE_WITH]   = "Replace with:",
    [STR_MATCH_CASE]     = "Match case",

    /* Run dialog */
    [STR_RUN]            = "Run",
    [STR_RUN_DESC]       = "Type the name of a program,\nfolder, or document, and\nFRANK OS will open it.",
    [STR_ERR_NO_SDCARD]  = "No SD card mounted.\nPlease insert an SD card\nand try again.",
    [STR_ERR_FILE_NOT_FOUND] = "Cannot find '%s'.\n\nMake sure the name is correct,\nthen try again.",

    /* Network */
    [STR_NET_CONN_FAILED]  = "Connection failed.\n\nCheck the password\nand try again.",
    [STR_WIFI_PASSWORD]    = "WiFi Password",
    [STR_ENTER_PASSWORD]   = "Enter password:",
    [STR_CONNECTING_TO]    = "Connecting to %s...",
    [STR_CONNECTING]       = "Connecting...",
    [STR_CONNECTED_TO]     = "Connected to: %s (%s)",
    [STR_NO_ADAPTER]       = "No network adapter detected",
    [STR_NOT_CONNECTED]    = "Not connected",
    [STR_SCANNING]         = "Scanning...",
    [STR_NO_NETWORKS]      = "No networks found. Click Scan.",
    [STR_SCAN]             = "Scan",
    [STR_CONNECT]          = "Connect",
    [STR_HDR_NETWORK]      = "Network",
    [STR_HDR_SIGNAL]       = "Signal",
    [STR_HDR_TYPE]         = "Type",
    [STR_ERR_NO_NET_ADAPTER] = "No network adapter detected.\n\nPlease connect a network\nadapter and reboot.",

    /* File manager - toolbar */
    [STR_FM_BACK]          = "Back",
    [STR_FM_UP]            = "Up",
    [STR_FM_CUT]           = "Cut",
    [STR_FM_COPY]          = "Copy",
    [STR_FM_PASTE]         = "Paste",
    [STR_FM_DELETE]        = "Delete",

    /* File manager - context menu */
    [STR_FM_OPEN_WITH]     = "Open with",
    [STR_FM_RENAME]        = "Rename",
    [STR_FM_NEW_FOLDER]    = "New Folder",
    [STR_FM_REFRESH]       = "Refresh",

    /* File manager - menu bar */
    [STR_FILE]             = "File",
    [STR_EDIT]             = "Edit",
    [STR_VIEW]             = "View",
    [STR_HELP]             = "Help",
    [STR_FM_NEW_FOLDER_MENU] = "New Folder",
    [STR_FM_DELETE_MENU]   = "Delete     Del",
    [STR_FM_RENAME_MENU]   = "Rename      F2",
    [STR_FM_CLOSE_MENU]    = "Close",
    [STR_FM_CUT_MENU]      = "Cut    Ctrl+X",
    [STR_FM_COPY_MENU]     = "Copy   Ctrl+C",
    [STR_FM_PASTE_MENU]    = "Paste  Ctrl+V",
    [STR_FM_SELALL_MENU]   = "SelAll Ctrl+A",
    [STR_FM_LARGE_ICONS]   = "Large Icons",
    [STR_FM_SMALL_ICONS]   = "Small Icons",
    [STR_FM_LIST]          = "List",
    [STR_FM_REFRESH_MENU]  = "Refresh     F5",
    [STR_FM_ABOUT_MENU]    = "About      F1",

    /* File manager - columns + types */
    [STR_FM_NAME]          = "Name",
    [STR_FM_SIZE]          = "Size",
    [STR_FM_TYPE]          = "Type",
    [STR_FM_FOLDER]        = "Folder",
    [STR_FM_APPLICATION]   = "Application",
    [STR_FM_FILE_TYPE]     = "File",
    [STR_FM_OBJECTS]       = "object(s)",

    /* File manager - dialogs */
    [STR_CONFIRM_DELETE]   = "Confirm Delete",
    [STR_FM_DELETE_ITEMS]  = "Delete selected item(s)?",
    [STR_FM_NO_APP_REGISTERED] = "No application is registered\nfor this file type.",
    [STR_ABOUT_NAVIGATOR]  = "About Navigator",
    [STR_FM_DELETING]      = "Deleting",
    [STR_FM_COPYING]       = "Copying",
    [STR_FM_NEW_FOLDER_DLG] = "New Folder",
    [STR_FM_RENAME_DLG]    = "Rename",
    [STR_FM_ENTER_NAME]    = "Name:",
    [STR_FM_NEW_NAME]      = "New name:",
    [STR_FM_ALL_FILES]     = "All Files (*.*)",

    /* Terminal */
    [STR_FM_EXIT]          = "Exit",
    [STR_ABOUT_TERMINAL]   = "About Terminal",

    /* Desktop context menu */
    [STR_SEND_TO_DESKTOP]  = "Send to Desktop",
    [STR_DT_REMOVE]        = "Remove",
    [STR_DT_SORT_BY_NAME]  = "Sort by Name",
    [STR_DT_REFRESH]       = "Refresh",

    /* Alt+Tab */
    [STR_ALTTAB_DESKTOP]   = "Desktop",

    /* Reboot/firmware dialogs */
    [STR_LAUNCH_FIRMWARE]  = "Launch Firmware",
    [STR_LAUNCH_FW_MSG]    = "Launch \"%s\"?...\n\nScreen will turn off...\n\nTo return to FRANK OS,\nhold Space and press Reset...",
    [STR_REBOOT_CONFIRM]   = "Are you sure you want to\nreboot the system?",
    [STR_FLASHING_FW]      = "Flashing firmware...",

    /* No UF2 */
    [STR_NO_UF2]           = "(no .uf2 files)",

    /* Run dialog extras */
    [STR_BROWSE]           = "Browse...",
    [STR_OPEN_LABEL]       = "Open:",
    [STR_FILES_OF_TYPE]    = "Files of type:",

    /* Dialog buttons */
    [STR_DLG_OK]           = "OK",
    [STR_DLG_CANCEL]       = "Cancel",
    [STR_DLG_YES]          = "Yes",
    [STR_DLG_NO]           = "No",
};

/* Russian string table (Win1251 encoded) */
static const char *str_ru[] = {
    /* Common buttons */
    [STR_OK]             = "OK",
    [STR_CANCEL]         = "Отмена",
    [STR_CLOSE]          = "Закрыть",
    [STR_YES]            = "Да",
    [STR_NO]             = "Нет",

    /* Start menu */
    [STR_START]          = "Пуск",
    [STR_PROGRAMS]       = "Программы",
    [STR_SETTINGS]       = "Настройки",
    [STR_FIRMWARE]       = "Прошивки",
    [STR_RUN_DOTS]       = "Выполнить...",
    [STR_REBOOT]         = "Перезагрузка",

    /* Programs submenu */
    [STR_NAVIGATOR]      = "Навигатор",
    [STR_TERMINAL]       = "Терминал",

    /* Settings submenu */
    [STR_CONTROL_PANEL]  = "Панель управления",
    [STR_NETWORK]        = "Сеть",
    [STR_LANGUAGE]       = "Язык",

    /* System menu */
    [STR_RESTORE]        = "Восстановить",
    [STR_MOVE]           = "Переместить",
    [STR_MAXIMIZE]       = "Развернуть",
    [STR_MINIMIZE]       = "Свернуть",
    [STR_ENTER_FULLSCREEN] = "Полный экран",
    [STR_EXIT_FULLSCREEN]  = "Оконный режим",

    /* Taskbar */
    [STR_VOLUME]         = "Громкость",
    [STR_MUTE]           = "Тишина",
    [STR_DISCONNECT]     = "Отключить",

    /* Control Panel items */
    [STR_DESKTOP]        = "Рабочий стол",
    [STR_SYSTEM]         = "Система",
    [STR_MOUSE]          = "Мышь",
    [STR_FREQUENCIES]    = "Частоты",

    /* Desktop Properties */
    [STR_BG_COLOR]       = "Цвет фона:",
    [STR_PREVIEW]        = "Предпросмотр:",
    [STR_WINDOW_THEME]   = "Тема окон:",
    [STR_DESKTOP_PROPS]  = "Рабочий стол",

    /* System Properties */
    [STR_SYSTEM_PROPS]   = "Система",

    /* Mouse Properties */
    [STR_DBLCLICK_SPEED] = "Скорость двойного клика:",
    [STR_SLOW]           = "Медленно",
    [STR_FAST]           = "Быстро",
    [STR_TEST_AREA]      = "Тест:",
    [STR_MOUSE_PROPS]    = "Мышь",

    /* Frequencies */
    [STR_CPU_FREQ]       = "Частота CPU:",
    [STR_PSRAM_FREQ]     = "Частота PSRAM:",
    [STR_REBOOT_NOTICE]  = "Изменения после\nперезагрузки.",

    /* Language */
    [STR_LANGUAGE_PROPS]   = "Язык",
    [STR_SELECT_LANGUAGE]  = "Выберите язык:",
    [STR_INPUT_TOGGLE]     = "Переключение ввода:",

    /* File dialog */
    [STR_LOOK_IN]        = "Папка:",
    [STR_FILE_NAME]      = "Имя файла:",
    [STR_OPEN]           = "Открыть",
    [STR_SAVE]           = "Сохранить",
    [STR_CONFIRM_SAVE_AS] = "Подтверждение",

    /* Find/Replace */
    [STR_FIND]           = "Поиск",
    [STR_REPLACE]        = "Замена",
    [STR_FIND_NEXT]      = "Найти далее",
    [STR_REPLACE_ALL]    = "Заменить всё",
    [STR_FIND_WHAT]      = "Найти:",
    [STR_REPLACE_WITH]   = "Заменить на:",
    [STR_MATCH_CASE]     = "С учётом регистра",

    /* Run dialog */
    [STR_RUN]            = "Запуск",
    [STR_RUN_DESC]       = "Введите имя программы,\nпапки или документа.",
    [STR_ERR_NO_SDCARD]  = "Нет SD-карты.\nВставьте SD-карту\nи попробуйте снова.",
    [STR_ERR_FILE_NOT_FOUND] = "Не удалось найти '%s'.\n\nПроверьте имя и\nпопробуйте снова.",

    /* Network */
    [STR_NET_CONN_FAILED]  = "Ошибка подключения.\n\nПроверьте пароль\nи попробуйте снова.",
    [STR_WIFI_PASSWORD]    = "Пароль WiFi",
    [STR_ENTER_PASSWORD]   = "Введите пароль:",
    [STR_CONNECTING_TO]    = "Подключение к %s...",
    [STR_CONNECTING]       = "Подключение...",
    [STR_CONNECTED_TO]     = "Подключено: %s (%s)",
    [STR_NO_ADAPTER]       = "Сетевой адаптер не найден",
    [STR_NOT_CONNECTED]    = "Не подключено",
    [STR_SCANNING]         = "Поиск сетей...",
    [STR_NO_NETWORKS]      = "Сети не найдены.",
    [STR_SCAN]             = "Поиск",
    [STR_CONNECT]          = "Подключить",
    [STR_HDR_NETWORK]      = "Сеть",
    [STR_HDR_SIGNAL]       = "Сигнал",
    [STR_HDR_TYPE]         = "Тип",
    [STR_ERR_NO_NET_ADAPTER] = "Сетевой адаптер не найден.\n\nВставьте сетевой адаптер\nи перезагрузите.",

    /* File manager - toolbar */
    [STR_FM_BACK]          = "Назад",
    [STR_FM_UP]            = "Вверх",
    [STR_FM_CUT]           = "Вырезать",
    [STR_FM_COPY]          = "Копировать",
    [STR_FM_PASTE]         = "Вставить",
    [STR_FM_DELETE]        = "Удалить",

    /* File manager - context menu */
    [STR_FM_OPEN_WITH]     = "Открыть в",
    [STR_FM_RENAME]        = "Переименовать",
    [STR_FM_NEW_FOLDER]    = "Новая папка",
    [STR_FM_REFRESH]       = "Обновить",

    /* File manager - menu bar */
    [STR_FILE]             = "Файл",
    [STR_EDIT]             = "Правка",
    [STR_VIEW]             = "Вид",
    [STR_HELP]             = "Справка",
    [STR_FM_NEW_FOLDER_MENU] = "Новая папка",
    [STR_FM_DELETE_MENU]   = "Удалить Del",
    [STR_FM_RENAME_MENU]   = "Переименовать F2",
    [STR_FM_CLOSE_MENU]    = "Закрыть",
    [STR_FM_CUT_MENU]      = "Вырезать Ctrl+X",
    [STR_FM_COPY_MENU]     = "Копировать Ctrl+C",
    [STR_FM_PASTE_MENU]    = "Вставить Ctrl+V",
    [STR_FM_SELALL_MENU]   = "Выделить Ctrl+A",
    [STR_FM_LARGE_ICONS]   = "Крупные значки",
    [STR_FM_SMALL_ICONS]   = "Мелкие значки",
    [STR_FM_LIST]          = "Список",
    [STR_FM_REFRESH_MENU]  = "Обновить F5",
    [STR_FM_ABOUT_MENU]    = "О программе  F1",

    /* File manager - columns + types */
    [STR_FM_NAME]          = "Имя",
    [STR_FM_SIZE]          = "Размер",
    [STR_FM_TYPE]          = "Тип",
    [STR_FM_FOLDER]        = "Папка",
    [STR_FM_APPLICATION]   = "Приложение",
    [STR_FM_FILE_TYPE]     = "Файл",
    [STR_FM_OBJECTS]       = "объектов",

    /* File manager - dialogs */
    [STR_CONFIRM_DELETE]   = "Подтверждение",
    [STR_FM_DELETE_ITEMS]  = "Удалить выбранные?",
    [STR_FM_NO_APP_REGISTERED] = "Нет приложения\nдля этого типа файлов.",
    [STR_ABOUT_NAVIGATOR]  = "О Навигаторе",
    [STR_FM_DELETING]      = "Удаление",
    [STR_FM_COPYING]       = "Копирование",
    [STR_FM_NEW_FOLDER_DLG] = "Новая папка",
    [STR_FM_RENAME_DLG]    = "Переименовать",
    [STR_FM_ENTER_NAME]    = "Имя:",
    [STR_FM_NEW_NAME]      = "Новое имя:",
    [STR_FM_ALL_FILES]     = "Все файлы (*.*)",

    /* Terminal */
    [STR_FM_EXIT]          = "Выход",
    [STR_ABOUT_TERMINAL]   = "О Терминале",

    /* Desktop context menu */
    [STR_SEND_TO_DESKTOP]  = "На рабочий стол",
    [STR_DT_REMOVE]        = "Удалить",
    [STR_DT_SORT_BY_NAME]  = "Сортировать",
    [STR_DT_REFRESH]       = "Обновить",

    /* Alt+Tab */
    [STR_ALTTAB_DESKTOP]   = "Рабочий стол",

    /* Reboot/firmware dialogs */
    [STR_LAUNCH_FIRMWARE]  = "Запуск прошивки",
    [STR_LAUNCH_FW_MSG]    = "Запустить \"%s\"?...\n\nЭкран отключится...\n\nДля возврата в FRANK OS,\nудерживайте Space\nи нажмите Reset...",
    [STR_REBOOT_CONFIRM]   = "Вы уверены, что хотите\nперезагрузить систему?",
    [STR_FLASHING_FW]      = "Запись прошивки...",

    /* No UF2 */
    [STR_NO_UF2]           = "(нет файлов .uf2)",

    /* Run dialog extras */
    [STR_BROWSE]           = "Обзор...",
    [STR_OPEN_LABEL]       = "Открыть:",
    [STR_FILES_OF_TYPE]    = "Тип файлов:",

    /* Dialog buttons */
    [STR_DLG_OK]           = "OK",
    [STR_DLG_CANCEL]       = "Отмена",
    [STR_DLG_YES]          = "Да",
    [STR_DLG_NO]           = "Нет",
};

uint8_t lang_get(void) {
    return settings_get()->language;
}

void lang_set(uint8_t lang) {
    settings_get()->language = lang;
}

const char *L(int str_id) {
    if (settings_get()->language == LANG_RU)
        return str_ru[str_id];
    return str_en[str_id];
}
