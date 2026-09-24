# PowerShell wrapper for fpga/sweep.sh. In PowerShell, `bash` resolves to
# WSL's bash, which cannot see the Windows Vivado install, so this calls
# Git for Windows' bash explicitly. Usage (from the repository root):
#   .\fpga\sweep.ps1 period 10 8 7
#   .\fpga\sweep.ps1 all
$gitBash = Join-Path $env:ProgramFiles 'Git\bin\bash.exe'
if (-not (Test-Path $gitBash)) { throw "Git Bash not found at $gitBash" }
& $gitBash (Join-Path $PSScriptRoot 'sweep.sh') @args
exit $LASTEXITCODE
