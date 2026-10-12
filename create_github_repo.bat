@echo off
setlocal enabledelayedexpansion

:: ============================================================================
::  create_github_repo.bat
::  在 GitHub 上创建 fillrate2026 仓库，并推送当前目录内容。
::  如果缺少 gh CLI，则通过 winget 自动安装。
:: ============================================================================

set "REPO_NAME=fillrate2026"
set "REPO_VIS=public"
set "PROJECT_DIR=%~dp0"

echo ============================================================
echo   Create GitHub repo: %REPO_NAME%
echo ============================================================
echo(

:: ---------------------------------------------------------------------------
:: 1. 确保 gh CLI 可用
:: ---------------------------------------------------------------------------
where gh >nul 2>&1
if %ERRORLEVEL% equ 0 goto :gh_ready

echo [INFO] gh CLI not found. Installing via winget...
echo(

winget install --id GitHub.cli -e --accept-source-agreements --accept-package-agreements
if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] winget install failed. Install gh manually:
    echo         https://cli.github.com/
    exit /b 1
)

:: 当前 shell 里刷新 PATH，让刚安装的 gh 立即可用
set "PATH=%PATH%;%ProgramFiles%\GitHub CLI"
where gh >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo.
    echo [WARN] gh installed but not visible in this shell.
    echo        Close this window, open a new one, and re-run this script.
    exit /b 1
)

:gh_ready
echo [INFO] gh CLI is available.
echo(

:: ---------------------------------------------------------------------------
:: 2. 确保已登录 GitHub
:: ---------------------------------------------------------------------------
gh auth status >nul 2>&1
if %ERRORLEVEL% equ 0 goto :auth_ready

echo [INFO] Not logged in to GitHub. Launching interactive login...
echo        Choose: GitHub.com -^> HTTPS -^> Login with a web browser
echo(
gh auth login
if %ERRORLEVEL% neq 0 (
    echo [ERROR] gh auth login failed.
    exit /b 1
)

:auth_ready
echo [INFO] GitHub authentication OK.
echo(

:: ---------------------------------------------------------------------------
:: 3. 确保 git 身份已配置
:: ---------------------------------------------------------------------------
git config user.email >nul 2>&1
if %ERRORLEVEL% equ 0 goto :git_identity_ready

for /f "tokens=*" %%u in ('gh api user -q .login') do set "GH_LOGIN=%%u"
for /f "tokens=*" %%e in ('gh api user -q .email 2^>nul') do set "GH_EMAIL=%%e"
if "!GH_EMAIL!"=="" set "GH_EMAIL=!GH_LOGIN!@users.noreply.github.com"

git config user.name  "!GH_LOGIN!"
git config user.email "!GH_EMAIL!"
echo [INFO] Configured git identity: !GH_LOGIN! ^<!GH_EMAIL!^>

:git_identity_ready

:: ---------------------------------------------------------------------------
:: 4. 初始化 / 准备本地仓库
:: ---------------------------------------------------------------------------
cd /d "%PROJECT_DIR%"

if not exist ".git" (
    echo [INFO] Initializing local git repository...
    git init >nul 2>&1
    git symbolic-ref HEAD refs/heads/main >nul 2>&1
)

if not exist "README.md" (
    > README.md echo # %REPO_NAME%
    echo [INFO] Created placeholder README.md
)

:: ---------------------------------------------------------------------------
:: 5. 暂存并提交
:: ---------------------------------------------------------------------------
git add -A

git diff --cached --quiet
if %ERRORLEVEL% equ 0 (
    echo [INFO] No changes to commit.
) else (
    echo [INFO] Committing...
    git commit -m "Initial commit"
)

:: ---------------------------------------------------------------------------
:: 6. 在 GitHub 上创建仓库（或连接已有仓库）并推送
:: ---------------------------------------------------------------------------
gh repo view "%REPO_NAME%" >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo [INFO] Repo %REPO_NAME% already exists on GitHub.
) else (
    echo [INFO] Creating repo %REPO_NAME% (%REPO_VIS%)...
    gh repo create "%REPO_NAME%" --%REPO_VIS%
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] gh repo create failed.
        exit /b 1
    )
)

for /f "tokens=*" %%u in ('gh api user -q .login') do set "GH_USER=%%u"
set "REMOTE_URL=https://github.com/!GH_USER!/%REPO_NAME%.git"

git remote remove origin >nul 2>&1
git remote add origin "!REMOTE_URL!"

echo [INFO] Pushing to !REMOTE_URL! ...
git push -u origin main
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Push failed. If remote has older commits, try:
    echo         git push -u origin main --force
    exit /b 1
)

:: ---------------------------------------------------------------------------
:: 7. 打开浏览器
:: ---------------------------------------------------------------------------
echo(
echo ============================================================
echo   Done: https://github.com/!GH_USER!/%REPO_NAME%
echo ============================================================
echo(

start "" "https://github.com/!GH_USER!/%REPO_NAME%"

endlocal