
@rem Creates a distribution of Notepad++ in the specified folder
@echo off

set /p DISTR="Enter distribution folder: "
set DISTR=%DISTR%\Notepad++
if not exist "%DISTR%" mkdir "%DISTR%"

copy "bin\change.log"                "%DISTR%"
copy "bin64\contextMenu.xml"         "%DISTR%"
copy "bin\doLocalConf.xml"           "%DISTR%"
copy "bin64\functionList.xml"        "%DISTR%"
copy "bin64\langs.model.xml"         "%DISTR%"
copy "bin\license.txt"               "%DISTR%"
copy "bin64\notepad++.exe"           "%DISTR%"
copy "bin\readme.txt"                "%DISTR%"
copy "bin64\SciLexer.dll"            "%DISTR%"
copy "bin64\shortcuts.xml"           "%DISTR%"
copy "bin64\stylers.model.xml"       "%DISTR%"
copy "bin64\userDefineLang.xml"      "%DISTR%"

if not exist "%DISTR%\plugins" mkdir "%DISTR%\plugins"
xcopy "bin64\plugins" "%DISTR%\plugins" /e
