@echo off
set NAME=100kDrawCalls
if exist %NAME%.exe del %NAME%.exe
cl /Zi /O2 /std:c++17 /EHsc %NAME%.cpp /link kernel32.lib user32.lib gdi32.lib /incremental:no /opt:ref
if exist %NAME%.obj del %NAME%.obj
if "%1" == "run" if exist %NAME%.exe (.\%NAME%.exe)
