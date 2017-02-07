
@rem Creates a distribution of Notepad++ in the specified folder
@echo off

set /p DISTR="Enter distribution folder: "
set DISTR=%DISTR%\Notepad++
if not exist "%DISTR%" mkdir "%DISTR%"

pushd bin
copy "allowAppDataPlugins.xml" "%DISTR%"
copy "change.log"              "%DISTR%"
copy "contextMenu.xml"         "%DISTR%"
copy "doLocalConf.xml"         "%DISTR%"
copy "functionList.xml"        "%DISTR%"
copy "langs.model.xml"         "%DISTR%"
copy "license.txt"             "%DISTR%"
copy "notepad++.exe"           "%DISTR%"
copy "readme.txt"              "%DISTR%"
copy "SciLexer.dll"            "%DISTR%"
copy "shortcuts.xml"           "%DISTR%"
copy "stylers.model.xml"       "%DISTR%"
copy "userDefineLang.xml"      "%DISTR%"
popd
