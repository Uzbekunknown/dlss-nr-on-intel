param([string]$Root = '')
$ErrorActionPreference='Stop'
if(-not $Root) { $Root=[IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetDirectoryName($PSCommandPath)) '..')) }
$compiler=Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$automation=Join-Path $env:SystemRoot 'Microsoft.NET\assembly\GAC_MSIL\System.Management.Automation\v4.0_3.0.0.0__31bf3856ad364e35\System.Management.Automation.dll'
if(-not (Test-Path -LiteralPath $compiler -PathType Leaf) -or -not (Test-Path -LiteralPath $automation -PathType Leaf)) { throw 'Windows .NET Framework x64 compiler / PowerShell assembly missing.' }
$output=Join-Path $Root 'work\NR-Setup.exe'
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output)) | Out-Null
# The icon is drawn, not kept: the host carries it, and a release carries it for the window.
$icon=Join-Path $Root 'work\nr-setup.ico'
& (Join-Path $Root 'tools\make_icon.ps1') -Out $icon
& $compiler /nologo /target:winexe /platform:x64 /optimize+ ('/out:'+$output) ('/win32icon:'+$icon) ('/reference:'+$automation) /reference:System.Windows.Forms.dll (Join-Path $Root 'dist-tools\NR-Setup.cs')
if($LASTEXITCODE -ne 0) { throw 'Windows wizard host compilation failed.' }
Write-Output ('Built Windows x64 setup host: '+$output)
