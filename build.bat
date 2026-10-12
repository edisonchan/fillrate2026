@echo off
setlocal

echo Compiling gpu_rop_probe.c ...

cl /nologo /W4 /O2 /utf-8 /MD ^
   /I"vulkan_sdk\Include" ^
   gpu_rop_probe.c ^
   /Fe:gpu_rop_probe.exe ^
   /Fo:gpu_rop_probe.obj ^
   /link /LIBPATH:"vulkan_sdk\Lib" ^
   /ignore:4098,4217,4286 ^
   vulkan-1.lib ^
   shaderc_combined.lib

if %ERRORLEVEL% neq 0 (
    echo.
    echo Build failed.
    exit /b 1
)

echo.
echo Build succeeded: gpu_rop_probe.exe