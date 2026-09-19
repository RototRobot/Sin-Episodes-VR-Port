<#
.SYNOPSIS
    Turn the game's reload-when-empty behaviour off (or back on) for arcade
    reloading.

.DESCRIPTION
    With `arcade_reload = 1` the mod reloads when you drop your hand to your
    waist. The game will still reload BY ITSELF the moment a clip runs dry,
    which defeats the point -- and that is not a setting, it is a weapon-script
    flag read once when the script is parsed:

        ITEM_FLAG_NOAUTORELOAD   (1<<1)

    basecombatweapon_shared.cpp's ReloadOrSwitchWeapons() checks it before the
    empty-clip reload, and weapon_parse.cpp reads it by NAME out of each
    scripts\weapon_*.txt. Nothing in the mod can reach it: it is baked into the
    parsed weapon info long before any hook of ours runs.

    This is not an inference from the SDK. SiN already ships the flag set on
    weapon_grenade.txt and weapon_rocket_launcher.txt, so the parser, the flag
    and the code path are all demonstrably live in this build.

    WHY THIS IS A SEPARATE TOOL RATHER THAN A CONFIG KEY
    It edits game content. The project's rule is that the mod does not change
    files it was not asked to change -- the same reason hands.vmt is a content
    override and the 4 GB flag is a launcher rather than a patch. A config key
    that silently rewrote a game asset would be impossible to debug six months
    later when someone verifies their install and cannot work out what changed.

    Every edit is backed up to <file>.sinvr-original, and -Revert restores it.
    Steam's "verify integrity of game files" also reverts everything this does.

.PARAMETER GameDir
    The SiN Episodes install. Defaults to the usual Steam location.

.PARAMETER Apply
    Add the flag. Without this the script only reports.

.PARAMETER Revert
    Restore every .sinvr-original backup.

.EXAMPLE
    .\set_noautoreload.ps1
    Report what each weapon script currently says. Changes nothing.

.EXAMPLE
    .\set_noautoreload.ps1 -Apply

.EXAMPLE
    .\set_noautoreload.ps1 -Revert
#>
[CmdletBinding()]
param(
    [string] $GameDir = "C:\Program Files (x86)\Steam\steamapps\common\SiN Episodes Emergence",
    [switch] $Apply,
    [switch] $Revert
)

$ErrorActionPreference = 'Stop'

$scriptDir = Join-Path $GameDir 'SE1\scripts'
if (-not (Test-Path $scriptDir)) {
    Write-Error "Not found: $scriptDir`nPass -GameDir if the game is installed elsewhere."
}

# Only weapons the player reloads. Selected by clip_size rather than by a
# hardcoded list, so a weapon this project never enumerated is still covered --
# but weapon_npc_pistol is excluded by name because it belongs to NPCs and their
# reloading is not the player's problem.
$FLAG = 'ITEM_FLAG_NOAUTORELOAD'
$candidates = Get-ChildItem (Join-Path $scriptDir 'weapon_*.txt') |
    Where-Object { $_.Name -ne 'weapon_npc_pistol.txt' }

$rows = @()
foreach ($f in $candidates) {
    $text = Get-Content -Raw -Encoding UTF8 $f.FullName

    $clip = 0
    if ($text -match '"clip_size"\s*"(-?\d+)"') { $clip = [int]$Matches[1] }

    # A weapon with no clip has nothing to auto-reload.
    if ($clip -le 0) { continue }

    $rows += [pscustomobject]@{
        File    = $f
        Clip    = $clip
        HasFlag = ($text -match [regex]::Escape($FLAG))
        Backup  = "$($f.FullName).sinvr-original"
    }
}

if ($rows.Count -eq 0) { Write-Error "No clip-using weapon scripts found in $scriptDir" }

# ---------------------------------------------------------------- report
"Weapon scripts in $scriptDir"
""
foreach ($r in $rows) {
    $state = if ($r.HasFlag) { 'no auto-reload' } else { 'auto-reloads when empty' }
    $bak = if (Test-Path $r.Backup) { '  [backup present]' } else { '' }
    "{0,-30} clip {1,-4} {2}{3}" -f $r.File.Name, $r.Clip, $state, $bak
}
""

# ---------------------------------------------------------------- revert
if ($Revert) {
    $n = 0
    foreach ($r in $rows) {
        if (Test-Path $r.Backup) {
            Copy-Item $r.Backup $r.File.FullName -Force
            Remove-Item $r.Backup -Force
            "reverted {0}" -f $r.File.Name
            $n++
        }
    }
    if ($n -eq 0) { "Nothing to revert -- no .sinvr-original backups found." }
    else { "`nReverted $n file(s). Auto-reload is back on." }
    return
}

# ----------------------------------------------------------------- apply
if (-not $Apply) {
    "Report only. Re-run with -Apply to add $FLAG, or -Revert to undo."
    return
}

$changed = 0
foreach ($r in $rows) {
    if ($r.HasFlag) { "already set   {0}" -f $r.File.Name; continue }

    $text = Get-Content -Raw -Encoding UTF8 $r.File.FullName

    # Insert after the last existing ITEM_FLAG_ line, matching the tab-separated
    # layout the shipped scripts already use (weapon_grenade.txt is the model).
    # Anchoring to a sibling flag rather than to the closing brace keeps the
    # insertion inside WeaponData without having to parse KeyValues.
    $matches = [regex]::Matches($text, '(?m)^[ \t]*"ITEM_FLAG_[A-Z]+"[ \t]*"\d"[ \t]*\r?\n')
    if ($matches.Count -eq 0) {
        Write-Warning ("{0}: no ITEM_FLAG_ line to anchor to -- skipped. Add `"{1}`" `"1`" by hand inside WeaponData." -f $r.File.Name, $FLAG)
        continue
    }

    if (-not (Test-Path $r.Backup)) {
        Copy-Item $r.File.FullName $r.Backup
    }

    $last = $matches[$matches.Count - 1]
    $insertAt = $last.Index + $last.Length
    $line = "`t`"$FLAG`"`t`"1`"`r`n"
    $text = $text.Substring(0, $insertAt) + $line + $text.Substring($insertAt)

    # No BOM: the engine's KeyValues parser reads these as plain text and a BOM
    # at the head of the file makes the first token unparseable.
    [System.IO.File]::WriteAllText($r.File.FullName, $text, (New-Object System.Text.UTF8Encoding $false))
    "set           {0}" -f $r.File.Name
    $changed++
}

""
if ($changed -eq 0) {
    "Nothing to do -- every clip weapon already has $FLAG."
} else {
    "Set $FLAG on $changed file(s). Originals kept as .sinvr-original."
    "Takes effect on the next map load. -Revert undoes it, and so does Steam's"
    "'verify integrity of game files'."
}
