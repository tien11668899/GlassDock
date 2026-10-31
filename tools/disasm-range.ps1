# Disassembles explorer.exe from an RVA onward in chunks (tools\disasm reads 0x800 bytes per call),
# up to -Max instructions or the first `int3` padding after a `ret`; optionally only lines that match
# -Match. Usage: disasm-range.ps1 -Rva 0x57e6c [-Max 3000] [-Match '164h']
# explorer.pdb is Microsoft's and not in the repository: download it from the Microsoft symbol
# server (the RSDS GUID and age in explorer.exe name it) into dev\symbols\explorer.
param([Parameter(Mandatory)][string]$Rva, [int]$Max = 2000, [string]$Match = '')
$tool = Join-Path $PSScriptRoot 'disasm\target\release\disasm.exe'
$pdb = Join-Path (Split-Path $PSScriptRoot) 'dev\symbols\explorer\explorer.pdb'
$addr = [Convert]::ToInt64(($Rva -replace '^0x', ''), 16)
$lines = New-Object System.Collections.Generic.List[string]
$sawRet = $false
while ($lines.Count -lt $Max) {
    $chunk = & $tool C:\Windows\explorer.exe $pdb ('@' + $addr.ToString('x')) 300 2>$null | Where-Object { $_ -match '^0x[0-9a-f]+\s' }
    if (-not $chunk) { break }
    $stop = $false
    # The last decoded instruction may be cut off at the 0x800-byte window: drop the tail.
    $keep = @($chunk)[0..([Math]::Max(0, @($chunk).Count - 4))]
    foreach ($l in $keep) {
        if ($l -match '^0x([0-9a-f]+)\s+([0-9A-F]+)\s+(.*)$') {
            $a = [Convert]::ToInt64($Matches[1], 16); $bytes = $Matches[2]; $text = $Matches[3]
            if ($sawRet -and $text -match '^int3') { $stop = $true; break }
            $sawRet = $text -match '^ret'
            $lines.Add($l)
            $addr = $a + $bytes.Length / 2
        }
    }
    if ($stop -or @($keep).Count -lt 2) { break }
}
if ($Match) { $lines | Where-Object { $_ -match $Match } } else { $lines }
