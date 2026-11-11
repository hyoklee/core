@echo off
rem Run the PoroTomo OMNI round trip on a Windows build made with oneAPI icx-cl
rem (see CI ci-windows.yml "icx" job) and CLIO_CORE_ENABLE_HDF5=ON.
rem Usage: run_porotomo_win.bat [build_dir]   (default C:\src\core\build-icx)
setlocal
set "B=%~1"
if "%B%"=="" set "B=C:\src\core\build-icx"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -property installationPath`) do set "VSINSTALL=%%i"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
rem icx-built binaries need libmmd.dll etc.; setvars.bat is not required.
call "%ProgramFiles(x86)%\Intel\oneAPI\compiler\latest\env\vars.bat" >nul 2>&1
set "PATH=%B%\vcpkg_installed\x64-windows\bin;%B%\bin;%PATH%"
set "CLIO_WITH_RUNTIME=1"
set "CLIO_SERVER_CONF=%~dp0clio_porotomo_config_win.yaml"
set "CLIO_BIND_ADDR=127.0.0.1"
set "CLIO_PORT=10600"
set "CAE_OMNI_FILE=%~dp0porotomo_dasv_omni_win.yaml"
set "CAE_OMNI_LAND_TIMEOUT_S=120"
"%B%\bin\cae_omni_hdf5_roundtrip.exe"
exit /b %ERRORLEVEL%
