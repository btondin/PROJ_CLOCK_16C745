@echo off
rem =====================================================================
rem  configurar.bat - Menu para configurar o relogio VFD (PIC16C745)
rem =====================================================================
rem  Front-end simples para o dtc_sync.py: sincronizar hora, ver status
rem  e programar/ligar/desligar o alarme, sem precisar decorar os
rem  parametros de linha de comando.
rem
rem  Duplo-clique funciona de qualquer lugar: o script entra na propria
rem  pasta (%~dp0) antes de chamar o Python.
rem =====================================================================
chcp 65001 >nul
setlocal EnableExtensions
title VFDCLK - Configuracao do relogio
cd /d "%~dp0"

rem --- verifica se o Python esta acessivel ------------------------------
python --version >nul 2>&1
if errorlevel 1 (
    echo.
    echo ERRO: o comando "python" nao foi encontrado.
    echo.
    echo Instale o Python em https://www.python.org/downloads/ e marque a
    echo opcao "Add Python to PATH" durante a instalacao.
    echo.
    echo Se ao digitar "python" o Windows abrir a Microsoft Store, va em
    echo Configuracoes - Aplicativos - Aliases de execucao de aplicativos
    echo e desative os aliases "python.exe" e "python3.exe".
    echo.
    pause
    exit /b 1
)

rem --- verifica se a biblioteca hidapi esta instalada -------------------
python -c "import hid" >nul 2>&1
if errorlevel 1 (
    echo.
    echo A biblioteca "hidapi" ainda nao esta instalada.
    set /p "INSTALAR=Instalar agora? (S/N): "
    if /i not "%INSTALAR%"=="S" (
        echo Cancelado. Rode manualmente: python -m pip install -r requirements.txt
        pause
        exit /b 1
    )
    echo.
    python -m pip install -r requirements.txt
    if errorlevel 1 (
        echo.
        echo ERRO: falha ao instalar a hidapi. Verifique a conexao com a internet.
        pause
        exit /b 1
    )
)

:MENU
cls
echo ===================================================
echo   VFDCLK - Configuracao do relogio via USB
echo ===================================================
echo.
echo   1. Sincronizar hora do PC com o relogio
echo   2. Ver status (hora, temperatura, umidade, alarme)
echo   3. Programar horario do alarme (programa e liga)
echo   4. Ligar alarme (mantem o horario ja gravado)
echo   5. Desligar alarme
echo   0. Sair
echo.
set "OPCAO="
set /p "OPCAO=Escolha uma opcao: "

if "%OPCAO%"=="1" goto SINCRONIZAR
if "%OPCAO%"=="2" goto STATUS
if "%OPCAO%"=="3" goto PROGRAMAR
if "%OPCAO%"=="4" goto LIGAR
if "%OPCAO%"=="5" goto DESLIGAR
if "%OPCAO%"=="0" goto FIM
echo.
echo Opcao invalida.
pause
goto MENU

:SINCRONIZAR
echo.
python dtc_sync.py
goto FIM_ACAO

:STATUS
echo.
python dtc_sync.py --status
goto FIM_ACAO

:PROGRAMAR
echo.
set "HORA="
set /p "HORA=Horario do alarme (formato HH:MM, ex.: 07:30): "
if "%HORA%"=="" (
    echo Nenhum horario informado.
    goto FIM_ACAO
)
python dtc_sync.py --alarme "%HORA%"
goto FIM_ACAO

:LIGAR
echo.
python dtc_sync.py --alarme on
goto FIM_ACAO

:DESLIGAR
echo.
python dtc_sync.py --alarme off
goto FIM_ACAO

:FIM_ACAO
echo.
pause
goto MENU

:FIM
endlocal
exit /b 0
