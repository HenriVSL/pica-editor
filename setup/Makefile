# setup/Makefile - Pica installer (T19). NMAKE (VC6), run in M:\dev\editor\setup:
#   ~/Documents/86box/samba/w98 build editor\setup            -> setup.exe (needs ..\pica.exe)
#   ... build editor\setup shtest                             -> obj\shtest.exe (test helper, not shipped)
#   ... build editor\setup old                                -> obj\setup09.exe (payload obj\pica09.exe, see INSTALLER.md)
#   ... build editor\setup clean
# setup.exe embeds $(PICA), obj\uninst.exe and, when present, $(README) and $(LICENSE) as RCDATA.

CFLAGS = /nologo /W3 /O2 /G6 /MT /DWIN32 /D_WINDOWS /I..\res
LIBS = user32.lib gdi32.lib kernel32.lib comctl32.lib shell32.lib advapi32.lib ole32.lib version.lib
PICA = ..\pica.exe
README = ..\README.TXT
LICENSE = ..\LICENSE.TXT
OBJS = obj\setup.obj obj\inst.obj
HDRS = setup.h setup_ids.h ..\res\appinfo.h

!IF EXIST($(README))
RCREADME = /dHAVE_README
CPREADME = copy /y $(README) obj\readme.bin
!ELSE
RCREADME =
CPREADME = @echo setup: $(README) not found, setup.exe ships without it
!ENDIF
!IF EXIST($(LICENSE))
RCLICENSE = /dHAVE_LICENSE
CPLICENSE = copy /y $(LICENSE) obj\license.bin
!ELSE
RCLICENSE =
CPLICENSE = @echo setup: no $(LICENSE)
!ENDIF

all: dirs setup.exe

dirs:
	@if not exist obj\nul mkdir obj

obj\setup.obj: setup.cpp $(HDRS)
	cl $(CFLAGS) /c /Foobj\setup.obj setup.cpp

obj\inst.obj: inst.cpp $(HDRS)
	cl $(CFLAGS) /c /Foobj\inst.obj inst.cpp

obj\uninst.res: uninst.rc setupver.rc2 setup_ids.h ..\res\appinfo.h ..\res\pica.ico
	rc /r /i..\res /foobj\uninst.res uninst.rc

obj\uninst.exe: $(OBJS) obj\uninst.res
	link /nologo /subsystem:windows /out:obj\uninst.exe $(OBJS) obj\uninst.res $(LIBS)

obj\setup.res: setup.rc setupver.rc2 setup_ids.h ..\res\appinfo.h ..\res\pica.ico obj\uninst.exe $(PICA)
	copy /y $(PICA) obj\pica.bin
	$(CPREADME)
	$(CPLICENSE)
	rc /r /i..\res $(RCREADME) $(RCLICENSE) /foobj\setup.res setup.rc

setup.exe: $(OBJS) obj\setup.res
	link /nologo /subsystem:windows /out:setup.exe $(OBJS) obj\setup.res $(LIBS)

# test build with a lower version: obj\pica09.exe = pica.exe with version 0.9 (python3 tools/patch_ver.py)
old: dirs obj\setup09.exe

obj\setup09.exe: $(OBJS) obj\uninst.exe obj\pica09.exe setup.rc setupver.rc2
	copy /y obj\pica09.exe obj\pica.bin
	rc /r /i..\res $(RCREADME) $(RCLICENSE) /foobj\setup09.res setup.rc
	link /nologo /subsystem:windows /out:obj\setup09.exe $(OBJS) obj\setup09.res $(LIBS)
	copy /y $(PICA) obj\pica.bin

shtest: dirs obj\shtest.exe

obj\shtest.exe: shtest.cpp ..\res\appinfo.h
	cl $(CFLAGS) /Foobj\shtest.obj /Feobj\shtest.exe shtest.cpp user32.lib shell32.lib advapi32.lib version.lib /link /subsystem:console

clean:
	-del obj\*.obj
	-del obj\*.res
	-del obj\*.bin
	-del obj\uninst.exe
	-del obj\setup09.exe
	-del obj\shtest.exe
	-del setup.exe
