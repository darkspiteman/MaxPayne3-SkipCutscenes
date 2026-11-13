MP3SkipCutscenes - пропуск кат-сцен в Max Payne 3 (ПК)
======================================================

Подходит для версий Steam и Rockstar Games Launcher: в моде нет ничего привязанного к Steam, нужные места в
MaxPayne3.exe он находит по сигнатурам. Сделан и проверен на MaxPayne3.exe 1.0.0.276 (Steam).
На версии Rockstar Games Launcher автор мод НЕ проверял - после первого запуска посмотрите лог.

Что делает
  - Space пропускает кат-сцены, в том числе заблокированные игрой ("STILL LOADING"): сначала догружается
    уровень, потом сцена завершается, поэтому игра не вылетает.
  - Одно нажатие пропускает всю цепочку сцен до геймплея.
  - Space обрывает килкамы (кроме постановочных, где выстрел должен долететь).
  - Файлы игры на диске не изменяются.

Установка
  1. Закройте игру.
  2. Откройте папку, где лежит MaxPayne3.exe:
       Steam:     ...\steamapps\common\Max Payne 3\Max Payne 3
       Rockstar:  папка установки из Rockstar Games Launcher -> Настройки -> Мои установленные игры
                  -> Max Payne 3 -> Открыть (обычно C:\Program Files\Rockstar Games\Max Payne 3)
  3. Скопируйте туда:
       MP3SkipCutscenes.asi
       MP3SkipCutscenes.ini
       dinput8.dll            <- ASI-загрузчик. НЕ копируйте, если в папке уже есть dinput8.dll
                                 (Ultimate ASI Loader или другой загрузчик) - оставьте свой.
  4. Запустите игру как обычно (через Steam или Rockstar Games Launcher).
  5. Откройте MP3SkipCutscenes.log рядом с MaxPayne3.exe. При рабочей установке в начале лога есть строки
       hooks installed (... patches ...)
       ready
     Если там "NOT FOUND" / "unsupported executable" - у вас другой MaxPayne3.exe, мод ничего в игре не тронул;
     пришлите лог автору.

Использование
  Space (или штатная кнопка пропуска) во время кат-сцены. Пропуск начинает работать после первой секунды сцены.
  Настройки: MP3SkipCutscenes.ini (клавиша, длина "хвоста", окно цепочки, килкамы).

Удаление
  Удалите MP3SkipCutscenes.asi, MP3SkipCutscenes.ini, MP3SkipCutscenes.log.
  Если копировали dinput8.dll из этого архива и других .asi-модов нет: удалите dinput8.dll и MP3AsiLoader.log.
  Больше мод нигде ничего не меняет.

Примечания
  - Не используйте вместе с CutsceneSkip из MaxPayne3.FusionFix и со старым "SkipCutscenes.asi" (ScriptHook).
  - Ролики между главами по-прежнему идут свои ~6 секунд (их держат скрипты игры; изучается).
  - Только одиночная игра.
  - Нужна Windows 10 или 11.

Лицензия: GPL-3.0-or-later (файл LICENSE). Основано на пропуске кат-сцен из MaxPayne3.FusionFix (ThirteenAG,
https://github.com/ThirteenAG/MaxPayne3.FusionFix). Полные исходники - в source.zip.
