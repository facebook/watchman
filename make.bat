@rem if we don't have nmake in the path, we need to run setup
where nmake.exe 2> NUL
if %ERRORLEVEL% GTR 0 set need_setup=1

@rem if we don't have the include path set, we need to run setup
if not defined INCLUDE set need_setup=1

@rem run setup if we need to
if %need_setup% == 1 call "c:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" amd64

@rem Allow python build to succeed:
@rem http://stackoverflow.com/questions/2817869/error-unable-to-find-vcvarsall-bat
SET VS90COMNTOOLS=%VS120COMNTOOLS%

where python.exe 2> NUL
if %ERRORLEVEL% GTR 0 set PATH=c:\Python27;%PATH%

where php.exe 2> NUL
if %ERRORLEVEL% GTR 0 set PATH=c:\php;%PATH%

@rem finally, run make
nmake /nologo /s /f winbuild\Makefile %1
