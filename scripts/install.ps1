#
#    This file is a part of the Koshka shell, (c) toiletbril, 2026
#    See the top-level LICENSE file for the licensing information.
#

# Run:
#     irm "https://fennec.support/kosh/install" | iex

& {
  $ErrorActionPreference = "Stop"
  $ProgressPreference = "SilentlyContinue"
  [Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

  $Releases = "https://github.com/toiletbril/kosh/releases"
  $Work = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid())
  $Here = Get-Location

  try {
    $Arch = switch ([Runtime.InteropServices.RuntimeInformation, mscorlib]::OSArchitecture) {
      "X64" { "amd64" }
      "Arm64" { "aarch64" }
      default { throw "unsupported processor $_" }
    }

    $Version = $env:KOSH_INSTALL_VERSION
    if (-not $Version) {
      $Version = (Invoke-RestMethod "https://api.github.com/repos/toiletbril/kosh/releases/latest").tag_name
    }
    if ($Version -notmatch "^[A-Za-z0-9._-]+$") {
      throw "invalid release version '$Version'"
    }

    $Prefix = $env:KOSH_INSTALL_PREFIX
    if (-not $Prefix) {
      $Prefix = Join-Path $env:LOCALAPPDATA "kosh"
    }

    $Binary = "kosh-win32-$Arch-$Version.exe"
    $Files = @($Binary, "kosh.bash")
    if (Get-Command zstd -ErrorAction SilentlyContinue) {
      $Files += "kosh.1.zst", "kosh.5.zst"
    }

    Write-Host "Installing" -ForegroundColor Blue -NoNewline
    Write-Host " kosh $Version for Windows on $Arch into $Prefix"

    New-Item -ItemType Directory $Work | Out-Null
    Set-Location $Work
    $Url = "$Releases/download/$Version"
    try {
      Invoke-WebRequest "$Url/SHA256SUMS" -OutFile SHA256SUMS -UseBasicParsing
    } catch {
      throw "release $Version has no SHA256SUMS file"
    }
    $Sums = Get-Content SHA256SUMS

    foreach ($File in $Files) {
      $Line = $Sums | Where-Object { ($_ -split "\s+\*?", 2)[1] -eq $File }
      if (-not $Line) {
        throw "release $Version has no $File"
      }
      Invoke-WebRequest "$Url/$File" -OutFile $File -UseBasicParsing
      if ((Get-FileHash -Algorithm SHA256 $File).Hash -ne ($Line -split "\s+")[0]) {
        throw "checksum mismatch for $File"
      }
    }

    $Bin = Join-Path $Prefix "bin"
    $Completions = Join-Path $Prefix "share\bash-completion\completions"
    New-Item -ItemType Directory -Force $Bin, $Completions | Out-Null
    Move-Item -Force $Binary (Join-Path $Bin "kosh.exe")
    Move-Item -Force kosh.bash (Join-Path $Completions "kosh")

    if ($Files -contains "kosh.1.zst") {
      foreach ($Page in "kosh.1", "kosh.5") {
        $Man = Join-Path $Prefix "share\man\man$($Page[-1])"
        New-Item -ItemType Directory -Force $Man | Out-Null
        zstd -dqf "$Page.zst" -o (Join-Path $Man $Page)
      }
    }

    Write-Host "Installed" -ForegroundColor Blue -NoNewline
    Write-Host " $Bin\kosh.exe"

    $Path = (Get-Item "HKCU:\Environment").GetValue("Path", "", "DoNotExpandEnvironmentNames")
    if (($Path -split ";") -notcontains $Bin) {
      if ((Read-Host "Add $Bin to the user PATH? [Y/n]") -notmatch "^n") {
        Set-ItemProperty "HKCU:\Environment" Path "$Path;$Bin" -Type ExpandString
        [Environment]::SetEnvironmentVariable("KOSH_INSTALL", "1", "User")
        [Environment]::SetEnvironmentVariable("KOSH_INSTALL", $null, "User")
        $env:Path += ";$Bin"
      }
    }
  } catch {
    Write-Host "error:" -ForegroundColor Red -NoNewline
    Write-Host " install.ps1: $($_.Exception.Message.TrimEnd('.'))."
    if ($PSCommandPath) {
      exit 1
    }
  } finally {
    Set-Location $Here
    Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
  }
}
