@if not exist bin mkdir bin
cl /Febin\NfsTester.exe /I. /Ox /D_MBCS /DLITTLE_ENDIAN ws2_32.lib Rpc.cpp Test.cpp
@if errorlevel 1 goto BUILD_FAILED

@echo BUILD SUCCESS
@goto EXIT

:BUILD_FAILED
@echo BUILD FAILED

:EXIT