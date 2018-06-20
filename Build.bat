@echo off
set NAME=100kDrawCalls
set FXC=fxc.exe /Ges /O3 /WX /nologo /Qstrip_reflect /Qstrip_debug /Qstrip_priv

if exist *.cso del *.cso
%FXC% /D VS_TRANSFORM /E VsTransform /Fo VsTransform.cso /T vs_5_1 100kDrawCalls.hlsl & if errorlevel 1 goto :end
%FXC% /D PS_SHADE /E PsShade /Fo PsShade.cso /T ps_5_1 100kDrawCalls.hlsl & if errorlevel 1 goto :end

if exist %NAME%.exe del %NAME%.exe
cl /Zi /O2 /std:c++17 /EHsc %NAME%.cpp /link kernel32.lib user32.lib gdi32.lib /incremental:no /opt:ref
if exist %NAME%.obj del %NAME%.obj
if "%1" == "run" if exist %NAME%.exe (.\%NAME%.exe)

:end
