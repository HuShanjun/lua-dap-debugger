@echo off
rem 参数1 lua版本: 5.1 5.2 5.3 5.4
if "%1" == "" (
    echo 请输入lua版本: 5.1 5.2 5.3 5.4
    exit /b 1
)
cmake -S . -B build/ninja -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DLUA_VERSION='%1'