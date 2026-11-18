# SPDX-License-Identifier: MIT
# Copyright (c) HanSoloBlaster, afiaka87
# Headless Switch Toolbox setup for OoT remake (Switch 2) model work. Dot-source it.
# Needs the 2026-11-05 Toolbox build: its root FirstPlugin.Plg.dll reads the remake's external string table.
param([string]$ToolboxDir,[string]$UnsafeCompatPath,[string]$TotkRomfs)
function Get-AssemblyVersion([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try { return [Reflection.AssemblyName]::GetAssemblyName($path).Version.ToString() } catch { return $null }
}
[AppDomain]::CurrentDomain.add_AssemblyResolve({
    param($sender,$args)
    $name = ($args.Name -split ',')[0]
    $assemblyName = [Reflection.AssemblyName]$args.Name
    $requested = if ($assemblyName.Version) { $assemblyName.Version.ToString() } else { '' }
    if ($name -eq 'System.Runtime.CompilerServices.Unsafe' -and (Test-Path -LiteralPath $UnsafeCompatPath)) {
        if ($requested -and (Get-AssemblyVersion $UnsafeCompatPath) -ne $requested) { return $null }
        return [Reflection.Assembly]::LoadFrom($UnsafeCompatPath)
    }
    foreach ($directory in @($ToolboxDir,"$ToolboxDir/Lib","$ToolboxDir/Lib/Plugins")) {
        $path = Join-Path $directory ($name+'.dll')
        if (Test-Path -LiteralPath $path) {
            if ($name -eq 'System.Runtime.CompilerServices.Unsafe' -and $requested -and
                (Get-AssemblyVersion $path) -ne $requested) { continue }
            return [Reflection.Assembly]::LoadFrom($path)
        }
    }
    return $null
}) | Out-Null
Set-Location -LiteralPath $ToolboxDir
$env:PATH = "$ToolboxDir/x64;$ToolboxDir;"+$env:PATH
foreach ($relative in @(
    'Toolbox.Library.dll','Gl_EditorFramework.dll','Toolbox.exe',
    'Lib/OpenTK.dll','Lib/OpenTK.GLControl.dll','Lib/Newtonsoft.Json.dll',
    'Lib/Syroot.Maths.dll','Lib/Syroot.BinaryData.dll','Lib/Syroot.NintenTools.Bfres.dll',
    'Syroot.NintenTools.NSW.Bfres.dll','Lib/Syroot.NintenTools.NSW.Bntx.dll',
    'Lib/Plugins/System.Buffers.dll','Lib/Plugins/System.Memory.dll',
    'Lib/Plugins/System.Numerics.Vectors.dll','Lib/Plugins/System.Threading.Tasks.Extensions.dll',
    'Lib/Plugins/ZstdSharp.dll','FirstPlugin.Plg.dll'
)) { [Reflection.Assembly]::LoadFrom((Join-Path $ToolboxDir $relative)) | Out-Null }
[Reflection.Assembly]::LoadFrom($UnsafeCompatPath) | Out-Null

function Find-Type([string]$name) {
    foreach ($assembly in [AppDomain]::CurrentDomain.GetAssemblies()) {
        $type = $assembly.GetType($name,$false)
        if ($type) { return $type }
    }
    throw "Missing Toolbox type: $name"
}
function Set-Runtime([string]$name,$value) {
    $type = Find-Type 'Toolbox.Library.Runtime'
    $type.GetField($name,[Reflection.BindingFlags]'Public,NonPublic,Static').SetValue($null,$value)
}
Set-Runtime 'ExecutableDir' $ToolboxDir
Set-Runtime 'TotkGamePath' $TotkRomfs
Set-Runtime 'DisplayViewport' $false
Set-Runtime 'UseOpenGL' $false
Set-Runtime 'OpenTKInitialized' $true
Set-Runtime 'AddFilesToActiveObjectEditor' $false
Set-Runtime 'MainForm' ([Activator]::CreateInstance((Find-Type 'Toolbox.Library.Forms.ObjectEditor'),$true))

function Load-Resource([string]$path) {
    $file = [Activator]::CreateInstance((Find-Type 'FirstPlugin.BFRES'),$true)
    $file.FilePath = $path
    $file.FileName = [IO.Path]::GetFileName($path)
    $file.IFileInfo = [Toolbox.Library.IFileInfo]::new()
    $stream = [IO.File]::OpenRead($path)
    try { $file.Load($stream) } finally { $stream.Dispose() }
    return $file.resFile
}
function Json($value) { [Newtonsoft.Json.JsonConvert]::SerializeObject($value) }
function Native-Version($resource,$native) {
    foreach ($name in 'VersionMajor','VersionMajor2','VersionMinor','VersionMinor2','Alignment','TargetAddressSize') {
        $resource.$name = $native.$name
    }
}
