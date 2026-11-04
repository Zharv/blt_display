# Activates ESP-IDF for this project in the current PowerShell session:
#   . .\idf-env.ps1
# Defaults match the install on D: (IDF in D:\Espressif\esp-idf-v5.5, tools in D:\Espressif\tools).
param(
    [string]$IdfPath = "D:\Espressif\esp-idf-v5.5",
    [string]$ToolsPath = "D:\Espressif\tools"
)

$env:IDF_TOOLS_PATH = $ToolsPath
# use the venv that was created for the IDF, whatever "python" on PATH is
$venv = Get-ChildItem -Directory (Join-Path $ToolsPath "python_env") -Filter "idf5.5_py*_env" | Select-Object -First 1
if ($venv) { $env:IDF_PYTHON_ENV_PATH = $venv.FullName }
# keep compiler cache off drive C:
if (-not $env:CCACHE_DIR) { $env:CCACHE_DIR = Join-Path (Split-Path $ToolsPath) "ccache" }

. (Join-Path $IdfPath "export.ps1")
