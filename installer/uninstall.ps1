<#
Rekarina's uninstaller: removes everything Rekarina put on the PC, for a clean reinstall, and nothing else.

    powershell -NoProfile -ExecutionPolicy Bypass -File rekarina-uninstall.ps1 [-GameDir <folder>] [-List] [-Yes]
        [-KeepSaves yes|no] [-WaitPid <pid>] [-Quiet] [-StartMenu <folder>] [-Desktop <folder>]
        [-RegKey <registry path>]

One implementation for every way in: the launcher's Uninstall plate (installer/uninstall.py: -List for its own
confirmation sheet, then -Yes -KeepSaves -WaitPid <launcher> once the player confirmed there), the "Uninstall
Rekarina" shortcut in the game folder and the Windows "Apps & features" entry (no arguments: this script's own window
shows the list and asks). The setup copies this file into the game folder as rekarina-uninstall.ps1 (installer/oot.py
uninstall_entry), so it works without the launcher: PowerShell runs from Windows' own folder, and the file is read
whole before anything is deleted, so it can remove the folders it came from.

What goes (each folder only when it carries the marks of ours, test_ours):
    the game folder                 rekarina-install.json (or an older totk-install.json) and Rekarina.exe or rekarina.ini
    <home>\setup\<version> folders  unpacked.txt, or Rekarina-Launcher.exe with installer\oot.py; their temp folders
                                    (a build's working files) are inside them. <home> is %LOCALAPPDATA%\Rekarina\home.txt's
                                    folder; <home>\setup and <home> go when they are empty afterwards
    %LOCALAPPDATA%\Rekarina         home.txt, logs, a 1.2.0 install's setup folders, then the folder when empty
    %APPDATA%\Rekarina              launcher.json, update.json, timings.json, then the folder when empty
    shortcuts                       Start menu "Rekarina", desktop "Rekarina Launcher", the game folder's own, each only
                                    when it points into a folder that goes
    the "Apps & features" entry     HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\Rekarina
Never: a game dump or keys anywhere (.nup .nsp .xci .nsz .xcz .keys stay where they are, with the folders that hold
them), anything under a link or junction (the link itself goes, not what it points to), a folder without the marks.
The saves (the game folder's profile\: save, save-tx, account.uid and whatever else it holds) stay where they are
unless the player chose "Delete everything" and confirmed it a second time: a reinstall into the same folder finds them.
-List prints the plan as JSON and changes nothing; -Yes does it without asking (-KeepSaves no: the saves too). Exit
code 0 done (or listed), 1 refused or failed, 2 cancelled.
#>
param(
    [string]$GameDir = '',
    [switch]$List,
    [switch]$Yes,
    [ValidateSet('yes', 'no', '')][string]$KeepSaves = '',
    [int]$WaitPid = 0,
    [switch]$Quiet,
    [string]$StartMenu = '',
    [string]$Desktop = '',
    [string]$RegKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Rekarina'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$Russian = (Get-UICulture).TwoLetterISOLanguageName -eq 'ru'
function T([string]$en, [string]$ru) { if ($Russian) { $ru } else { $en } }

$DumpExt = @('.nup', '.nsp', '.xci', '.nsz', '.xcz', '.keys')
$KeepDir = ''   # the saves' folder while they stay: nothing under it is listed or removed

function Full([string]$p) {
    if (-not $p) { return '' }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}

function Is-Under([string]$path, [string]$folder) {
    if (-not $path -or -not $folder) { return $false }
    $p = (Full $path) + '\'
    $f = (Full $folder) + '\'
    return $p.StartsWith($f, [System.StringComparison]::OrdinalIgnoreCase)
}

function Same([string]$a, [string]$b) { return (Full $a) -ieq (Full $b) }

# Folders that are never ours, nor may hold the whole of something removed: a drive's root, Windows, the program
# folders, the user's own folders.
function Forbidden([string]$path) {
    $p = Full $path
    if ($p.Length -le 3) { return $true }
    $sys = @($env:SystemRoot, $env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramData, $env:USERPROFILE,
             $env:LOCALAPPDATA, $env:APPDATA, [Environment]::GetFolderPath('MyDocuments'),
             [Environment]::GetFolderPath('Desktop'), (Join-Path $env:USERPROFILE 'Downloads'))
    foreach ($s in $sys) { if ($s -and (Same $p $s)) { return $true } }
    foreach ($s in @($env:SystemRoot, $env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($s -and (Is-Under $p $s)) { return $true }
    }
    return $false
}

function Has([string]$folder, [string]$name) { return Test-Path -LiteralPath (Join-Path $folder $name) }

# Whether a folder is ours, by the marks the setup and the single exe leave: 'game', 'setup', 'appdata' or ''.
function Test-Ours([string]$folder, [string]$kind) {
    if (-not $folder -or -not (Test-Path -LiteralPath $folder -PathType Container)) { return $false }
    if (Forbidden $folder) { return $false }
    $item = Get-Item -LiteralPath $folder -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { return $false }
    switch ($kind) {
        'game' {
            return ((Has $folder 'rekarina-install.json') -or (Has $folder 'totk-install.json')) -and
                   ((Has $folder 'Rekarina.exe') -or (Has $folder 'rekarina.ini'))
        }
        'setup' {
            return (Has $folder 'unpacked.txt') -or ((Has $folder 'Rekarina-Launcher.exe') -and (Has $folder 'installer\oot.py'))
        }
        'appdata' {
            $local = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Rekarina' } else { '' }
            $roam = if ($env:APPDATA) { Join-Path $env:APPDATA 'Rekarina' } else { '' }
            return ($local -and (Same $folder $local)) -or ($roam -and (Same $folder $roam))
        }
    }
    return $false
}

# The files of a folder that would go: every file under it but the dump's, nothing under a link (the link itself is
# listed as a folder to remove without its contents).
function Files-Of([string]$folder) {
    $out = New-Object System.Collections.Generic.List[object]
    $stack = New-Object System.Collections.Generic.Stack[string]
    $stack.Push($folder)
    while ($stack.Count) {
        $d = $stack.Pop()
        foreach ($e in [IO.Directory]::EnumerateFileSystemEntries($d)) {
            $a = [IO.File]::GetAttributes($e)
            if ($a -band [IO.FileAttributes]::Directory) {
                if (-not ($a -band [IO.FileAttributes]::ReparsePoint) -and -not ($KeepDir -and (Same $e $KeepDir))) { $stack.Push($e) }
            } elseif ($DumpExt -notcontains [IO.Path]::GetExtension($e).ToLowerInvariant()) {
                $out.Add($e)
            }
        }
    }
    return , $out
}

function Bytes-Of([string]$folder) {
    $n = [int64]0
    foreach ($f in (Files-Of $folder)) { try { $n += (New-Object IO.FileInfo $f).Length } catch { } }
    return $n
}

function Dumps-Under([string]$folder) {
    $out = @()
    $stack = New-Object System.Collections.Generic.Stack[string]
    $stack.Push($folder)
    while ($stack.Count) {
        $d = $stack.Pop()
        foreach ($e in [IO.Directory]::EnumerateFileSystemEntries($d)) {
            $a = [IO.File]::GetAttributes($e)
            if ($a -band [IO.FileAttributes]::Directory) {
                if (-not ($a -band [IO.FileAttributes]::ReparsePoint)) { $stack.Push($e) }
            } elseif ($DumpExt -contains [IO.Path]::GetExtension($e).ToLowerInvariant()) { $out += $e }
        }
    }
    return $out
}

# Removes a folder of ours: every file but the dump's, links as links, then the folders left empty, bottom up.
function Remove-Ours([string]$folder, [string]$kind) {
    if (-not (Test-Ours $folder $kind)) { throw (T "not removed, it does not carry Rekarina's marks: $folder" "не удалено, это не папка Rekarina: $folder") }
    foreach ($f in (Files-Of $folder)) {
        [IO.File]::SetAttributes($f, [IO.FileAttributes]::Normal)
        [IO.File]::Delete($f)
    }
    $dirs = New-Object System.Collections.Generic.List[string]
    $stack = New-Object System.Collections.Generic.Stack[string]
    $stack.Push($folder)
    while ($stack.Count) {
        $d = $stack.Pop()
        $dirs.Add($d)
        foreach ($e in [IO.Directory]::EnumerateDirectories($d)) {
            if ($KeepDir -and (Same $e $KeepDir)) { continue }   # the saves stay, with everything in them
            if ([IO.File]::GetAttributes($e) -band [IO.FileAttributes]::ReparsePoint) {
                [IO.Directory]::Delete($e, $false)   # the link alone; what it points to stays
            } else { $stack.Push($e) }
        }
    }
    for ($i = $dirs.Count - 1; $i -ge 0; $i--) {
        $d = $dirs[$i]
        if (-not [IO.Directory]::EnumerateFileSystemEntries($d).GetEnumerator().MoveNext()) {
            [IO.File]::SetAttributes($d, [IO.FileAttributes]::Directory)
            [IO.Directory]::Delete($d, $false)
        }
    }
}

# The app-data folders give up only their own files; a setup or game folder inside one (a 1.2.0 install's) is an item
# of its own, under its own guard.
$AppdataNames = @('home.txt', 'launcher.json', 'update.json', 'timings.json', 'logs')
function Appdata-Entries([string]$folder) {
    $out = @()
    foreach ($n in $AppdataNames) { $p = Join-Path $folder $n; if (Test-Path -LiteralPath $p) { $out += $p } }
    return $out
}
function Appdata-Bytes([string]$folder) {
    $n = [int64]0
    foreach ($e in (Appdata-Entries $folder)) {
        if (Test-Path -LiteralPath $e -PathType Container) { $n += Bytes-Of $e } else { $n += (Get-Item -LiteralPath $e -Force).Length }
    }
    return $n
}
function Remove-Appdata([string]$folder) {
    if (-not (Test-Ours $folder 'appdata')) { throw "not removed, not Rekarina's application data: $folder" }
    foreach ($e in (Appdata-Entries $folder)) {
        if (Test-Path -LiteralPath $e -PathType Container) {
            foreach ($f in (Files-Of $e)) { [IO.File]::SetAttributes($f, [IO.FileAttributes]::Normal); [IO.File]::Delete($f) }
            Get-ChildItem -LiteralPath $e -Recurse -Force -Directory | Sort-Object { $_.FullName.Length } -Descending |
                ForEach-Object { [void](Remove-If-Empty $_.FullName) }
            [void](Remove-If-Empty $e)
        } else { [IO.File]::Delete($e) }
    }
}

function Remove-If-Empty([string]$folder) {
    if ($folder -and (Test-Path -LiteralPath $folder -PathType Container) -and -not (Forbidden $folder) -and
        -not [IO.Directory]::EnumerateFileSystemEntries($folder).GetEnumerator().MoveNext()) {
        [IO.Directory]::Delete($folder, $false)
        return $true
    }
    return $false
}

function Shortcut-Target([string]$lnk) {
    try { return (New-Object -ComObject WScript.Shell).CreateShortcut($lnk).TargetPath } catch { return '' }
}

# ---- the plan
if (-not $GameDir) { $GameDir = $PSScriptRoot }
$GameDir = Full $GameDir
$localRk = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'Rekarina' } else { '' }
$roamRk = if ($env:APPDATA) { Join-Path $env:APPDATA 'Rekarina' } else { '' }
$homeDir = ''
if ($localRk -and (Has $localRk 'home.txt')) {
    $raw = [IO.File]::ReadAllBytes((Join-Path $localRk 'home.txt'))
    $text = [Text.Encoding]::Unicode.GetString($raw).Trim([char]0xFEFF, [char]0, ' ', "`r", "`n")
    if ($text -and (Test-Path -LiteralPath $text -PathType Container)) { $homeDir = Full $text }
}

$items = New-Object System.Collections.Generic.List[object]
$refused = New-Object System.Collections.Generic.List[object]
$saves = Join-Path $GameDir 'profile'
$savesBytes = [int64]0
if (Test-Path -LiteralPath $saves -PathType Container) { $savesBytes = Bytes-Of $saves; $KeepDir = $saves }
function Add-Item([string]$path, [string]$kind, [string]$what) {
    foreach ($i in $items) { if (Same $i.path $path) { return } }
    $b = if ($kind -eq 'appdata') { Appdata-Bytes $path } else { Bytes-Of $path }
    $items.Add([ordered]@{ path = $path; kind = $kind; what = $what; bytes = $b })
}

if (Test-Ours $GameDir 'game') { Add-Item $GameDir 'game' 'game' }
else { $refused.Add([ordered]@{ path = $GameDir; why = 'the game folder carries no rekarina-install.json with Rekarina.exe' }) }
$setupRoots = @()
if ($homeDir) { $setupRoots += (Join-Path $homeDir 'setup') }
if ($localRk) { $setupRoots += (Join-Path $localRk 'setup') }
foreach ($root in $setupRoots) {
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
    foreach ($d in [IO.Directory]::EnumerateDirectories($root)) {
        if (Test-Ours $d 'setup') { Add-Item $d 'setup' 'setup' }
        else { $refused.Add([ordered]@{ path = $d; why = 'no unpacked.txt or Rekarina-Launcher.exe with installer\oot.py' }) }
    }
}
# A setup folder anywhere else (a setup unpacked from the zip release) is found through the shortcuts that point to its
# launcher, under the same guard
$startDir0 = if ($StartMenu) { $StartMenu } else { [Environment]::GetFolderPath('Programs') }
$deskDir0 = if ($Desktop) { $Desktop } else { [Environment]::GetFolderPath('Desktop') }
foreach ($l in @((Join-Path $GameDir 'Rekarina Launcher.lnk'), (Join-Path $startDir0 'Rekarina.lnk'), (Join-Path $deskDir0 'Rekarina Launcher.lnk'))) {
    if (-not (Test-Path -LiteralPath $l -PathType Leaf)) { continue }
    $t = Shortcut-Target $l
    if ($t -and ([IO.Path]::GetFileName($t) -ieq 'Rekarina-Launcher.exe')) {
        $d = [IO.Path]::GetDirectoryName($t)
        if (Test-Ours $d 'setup') { Add-Item (Full $d) 'setup' 'setup' }
    }
}
foreach ($a in @($localRk, $roamRk)) { if ($a -and (Test-Ours $a 'appdata')) { Add-Item $a 'appdata' 'appdata' } }

$gone = @($items | ForEach-Object { $_.path })
$links = @()
$startDir = if ($StartMenu) { $StartMenu } else { [Environment]::GetFolderPath('Programs') }
$deskDir = if ($Desktop) { $Desktop } else { [Environment]::GetFolderPath('Desktop') }
foreach ($l in @((Join-Path $startDir 'Rekarina.lnk'), (Join-Path $deskDir 'Rekarina Launcher.lnk'))) {
    if (-not (Test-Path -LiteralPath $l -PathType Leaf)) { continue }
    $t = Shortcut-Target $l
    $ours = $false
    foreach ($g in $gone) { if ($t -and (Is-Under $t $g)) { $ours = $true } }
    if ($ours) { $links += $l } else { $refused.Add([ordered]@{ path = $l; why = "it points to $t, which stays" }) }
}
$reg = Test-Path -LiteralPath $RegKey
$dumps = @()
foreach ($i in $items) { $dumps += Dumps-Under $i.path }
$total = [int64]0
foreach ($i in $items) { $total += $i.bytes }

if ($List) {
    $plan = [ordered]@{ game = $GameDir; home = $homeDir; items = $items; shortcuts = $links; registry = $(if ($reg) { $RegKey } else { '' })
                        total_bytes = $total; saves = $(if ($savesBytes) { $saves } else { '' }); saves_bytes = $savesBytes
                        dumps_kept = $dumps; refused = $refused }
    $plan | ConvertTo-Json -Depth 5 -Compress
    exit 0
}

function Size-Text([int64]$n) {
    if ($n -ge 1GB) { return ('{0:N1} GB' -f ($n / 1GB)) }
    if ($n -ge 1MB) { return ('{0:N0} MB' -f ($n / 1MB)) }
    return ('{0:N0} KB' -f [math]::Ceiling($n / 1KB))
}

function Say([string]$text, [string]$icon = 'Information') {
    if ($Quiet -or ($Yes -and -not $WaitPid)) { Write-Output $text; return }
    Add-Type -AssemblyName System.Windows.Forms
    [void][System.Windows.Forms.MessageBox]::Show($text, 'Rekarina', 'OK', $icon)
}

if (-not $items.Count) {
    Say (T "Nothing of Rekarina was found to remove beside $GameDir." "Рядом с $GameDir не найдено ничего от Rekarina.") 'Warning'
    exit 1
}

# ---- asking (no -Yes): the list and the space, then a clear choice of two buttons. "Keep my saves" is the default and
# has the focus; "Delete everything" asks a second time, naming the saves' folder, and stops at a no.
$keep = $KeepSaves -ne 'no'
if (-not $Yes) {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    $lines = @()
    foreach ($i in $items) { $lines += ('{0}   ({1})' -f $i.path, (Size-Text $i.bytes)) }
    foreach ($l in $links) { $lines += $l }
    if ($reg) { $lines += (T 'the "Apps & features" entry' 'запись в «Приложения и возможности»') }
    $form = New-Object System.Windows.Forms.Form
    $form.Text = T 'Uninstall Rekarina' 'Удаление Rekarina'
    $form.Width = 780; $form.Height = 470; $form.StartPosition = 'CenterScreen'; $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false; $form.MinimizeBox = $false
    $label = New-Object System.Windows.Forms.Label
    $label.Left = 16; $label.Top = 14; $label.Width = 732; $label.Height = 40
    $label.Text = T ("These folders and files of Rekarina will be removed; {0} become free." -f (Size-Text $total)) ("Будут удалены эти папки и файлы Rekarina; освободится {0}." -f (Size-Text $total))
    $box = New-Object System.Windows.Forms.TextBox
    $box.Left = 16; $box.Top = 56; $box.Width = 732; $box.Height = 220; $box.Multiline = $true; $box.ReadOnly = $true
    $box.ScrollBars = 'Vertical'; $box.Text = ($lines -join "`r`n")
    $note = New-Object System.Windows.Forms.Label
    $note.Left = 16; $note.Top = 288; $note.Width = 732; $note.Height = 84
    $savesLine = if ($savesBytes -gt 0) {
        T ("Your saves stay in {0} ({1}): a new install into the same folder finds them. ""Delete everything"" removes them too." -f $saves, (Size-Text $savesBytes)) ("Ваши сохранения остаются в {0} ({1}): новая установка в ту же папку их найдёт. «Удалить всё» удалит и их." -f $saves, (Size-Text $savesBytes))
    } else { T 'There are no saves in the game folder.' 'В папке игры нет сохранений.' }
    $note.Text = $savesLine + "`r`n" + (T 'Your game dump and keys are not touched.' 'Дамп игры и ключи не затрагиваются.')
    $keepBtn = New-Object System.Windows.Forms.Button
    $keepBtn.Text = T 'Keep my saves' 'Сохранить сохранения'; $keepBtn.Left = 286; $keepBtn.Top = 384; $keepBtn.Width = 170; $keepBtn.Height = 30
    $keepBtn.DialogResult = 'Yes'
    $allBtn = New-Object System.Windows.Forms.Button
    $allBtn.Text = T 'Delete everything' 'Удалить всё'; $allBtn.Left = 464; $allBtn.Top = 384; $allBtn.Width = 160; $allBtn.Height = 30
    $allBtn.DialogResult = 'No'
    $allBtn.Enabled = $savesBytes -gt 0
    $cancelBtn = New-Object System.Windows.Forms.Button
    $cancelBtn.Text = T 'Cancel' 'Отмена'; $cancelBtn.Left = 632; $cancelBtn.Top = 384; $cancelBtn.Width = 116; $cancelBtn.Height = 30
    $cancelBtn.DialogResult = 'Cancel'
    $form.Controls.AddRange(@($label, $box, $note, $keepBtn, $allBtn, $cancelBtn))
    $form.AcceptButton = $keepBtn; $form.CancelButton = $cancelBtn
    $form.Add_Shown({ $keepBtn.Focus() })
    $answer = $form.ShowDialog()
    if ($answer -eq 'Cancel') { exit 2 }
    $keep = $answer -eq 'Yes'
    if (-not $keep) {
        $sure = [System.Windows.Forms.MessageBox]::Show(
            (T ("Delete everything also deletes your saves in`r`n{0}`r`n`r`nThis cannot be undone. Delete everything?" -f $saves) ("«Удалить всё» удалит и ваши сохранения в`r`n{0}`r`n`r`nЭто нельзя отменить. Удалить всё?" -f $saves)),
            'Rekarina', 'YesNo', 'Warning', 'Button2')
        if ($sure -ne 'Yes') { exit 2 }   # stops at the first no: nothing is removed
    }
}
if (-not $keep) { $KeepDir = '' }

# ---- doing it
if ($WaitPid) { try { Wait-Process -Id $WaitPid -Timeout 120 } catch { } }
foreach ($i in $items) {
    $busy = @(Get-Process -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -and (Is-Under $_.Path $i.path) } catch { $false } })
    if ($busy.Count) {
        Say (T ("Close Rekarina first: {0} is still running." -f $busy[0].Path) ("Сначала закройте Rekarina: {0} ещё работает." -f $busy[0].Path)) 'Warning'
        exit 1
    }
}
$failed = @()
foreach ($i in @($items | Where-Object { $_.kind -ne 'appdata' }) + @($items | Where-Object { $_.kind -eq 'appdata' })) {
    try {
        if ($i.kind -eq 'appdata') { Remove-Appdata $i.path; [void](Remove-If-Empty (Join-Path $i.path 'setup')); [void](Remove-If-Empty $i.path) }
        else { Remove-Ours $i.path $i.kind }
    } catch { $failed += ('{0}: {1}' -f $i.path, $_.Exception.Message) }
}
foreach ($l in $links) { try { Remove-Item -LiteralPath $l -Force } catch { $failed += ('{0}: {1}' -f $l, $_.Exception.Message) } }
if ($reg) { try { Remove-Item -LiteralPath $RegKey -Recurse -Force } catch { $failed += ('{0}: {1}' -f $RegKey, $_.Exception.Message) } }
if ($homeDir) { [void](Remove-If-Empty (Join-Path $homeDir 'setup')); [void](Remove-If-Empty $homeDir) }

$msg = T 'Rekarina is removed.' 'Rekarina удалена.'
if ($keep -and $savesBytes -gt 0) { $msg += ' ' + (T "Your saves stay in $saves." "Ваши сохранения остались в $saves.") }
if ($dumps.Count) { $msg += ' ' + (T 'Game dump files were left where they are.' 'Файлы дампа игры оставлены на месте.') }
if ($failed.Count) {
    $msg += "`r`n`r`n" + (T 'Not everything could be removed:' 'Удалось удалить не всё:') + "`r`n" + ($failed -join "`r`n")
    Say $msg 'Warning'
    exit 1
}
Say $msg
exit 0
