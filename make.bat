set need_setup=0

@rem if we don't have msbuild in the path, we need to run setup
where msbuild.exe 2> NUL
if %ERRORLEVEL% GTR 0 set need_setup=1

@rem if we don't have the include path set, we need to run setup
if not defined INCLUDE set need_setup=1

@rem backup of the path, otherwise we end up with an "input line is too long"
@rem error since the path env var is growing up at every call to vsvarsall.bat
@set PATH_BACKUP=%PATH%

@rem run setup if we need to
if %need_setup% == 1 call "c:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64

@rem Allow python build to succeed:
@rem http://stackoverflow.com/questions/2817869/error-unable-to-find-vcvarsall-bat
SET VS90COMNTOOLS=%VS140COMNTOOLS%

where python.exe 2> NUL
if %ERRORLEVEL% GTR 0 set PATH=c:\Python27;%PATH%

@rem finally, run make
msbuild.exe ALL_BUILD.vcxproj /nologo /p:Configuration=Release

SET RETURN_CODE=%ERRORLEVEL%

@rem restore the original path value
@set PATH=%PATH_BACKUP%

@rem "input line is too long" problem occur also with the INCLUDE environment variable
@rem We just wipe it since next call to the batch file will set back the right value
@set INCLUDE=
@set LIB=
@set LIBPATH=

EXIT /B %RETURN_CODE%
