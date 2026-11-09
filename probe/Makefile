# NMAKE makefile for the editor render / file I/O probe (Visual C++ 6.0)
CFLAGS = /nologo /W3 /O2 /G6 /DWIN32

edprobe.exe: edprobe.cpp
	cl $(CFLAGS) edprobe.cpp user32.lib gdi32.lib kernel32.lib /link /subsystem:windows

clean:
	-del *.obj
	-del edprobe.exe
