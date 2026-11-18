# SPDX-License-Identifier: MIT
# Copyright (c) HanSoloBlaster, afiaka87
# Builds the paraglider's looping "Wait" animation from TotK's "Float" (decoded Item_Parasail.anim). -Flutter adds a
# tail wave in place of TotK's cloth physics; -Verify compares a finished file with the plan.
param(
    [Parameter(Mandatory=$true)][string]$ToolboxDir,
    [Parameter(Mandatory=$true)][string]$UnsafeCompatPath,
    [Parameter(Mandatory=$true)][string]$TotkRomfs,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$SourceAnim = '',
    [string]$NativeAnim = '',
    [string]$Project = 'Obj_Paraglider',
    [double]$Scale = 1.0,
    [switch]$Flutter,
    [switch]$Verify
)
$ErrorActionPreference = 'Stop'
foreach ($name in 'ToolboxDir','UnsafeCompatPath','TotkRomfs','OutputDirectory','SourceAnim','NativeAnim') {
    $value = Get-Variable $name -ValueOnly
    if ($value) { Set-Variable $name ([IO.Path]::GetFullPath($value)) }
}
. (Join-Path $PSScriptRoot 'toolbox_oot.ps1') -ToolboxDir $ToolboxDir -UnsafeCompatPath $UnsafeCompatPath -TotkRomfs $TotkRomfs | Out-Null

$frames = 30
$animationName = 'Wait'
$out = Join-Path $OutputDirectory "${Project}_Animation.anim.bfres"
$planPath = Join-Path $OutputDirectory 'anim-plan.json'
# Curve targets are byte offsets into a bone's base transform: translate 16/20/24, rotate 32/36/40.
$targetName = @{ 16='TranslateX'; 20='TranslateY'; 24='TranslateZ'; 32='RotateX'; 36='RotateY'; 40='RotateZ' }

function Sample($curve,[int]$frame) {
    $count = $curve.Frames.Length
    $offset = if ($curve.KeyType.ToString() -eq 'Single') { 0.0 } else { [double]$curve.Offset.Single }
    $scale = [double]$curve.Scale
    if ($frame -ge $curve.Frames[$count-1]) { return [double]$curve.Keys[($count-1),0]*$scale+$offset }
    $i = 0
    while ($i -lt $count-2 -and $curve.Frames[$i+1] -le $frame) { $i++ }
    $t = ($frame-$curve.Frames[$i])/($curve.Frames[$i+1]-$curve.Frames[$i])
    $value = (($curve.Keys[$i,3]*$t+$curve.Keys[$i,2])*$t+$curve.Keys[$i,1])*$t+$curve.Keys[$i,0]
    return $value*$scale+$offset
}
function Samples($curve) { ,@(0..$frames | ForEach-Object { Sample $curve $_ }) }

if ($Verify) {
    $plan = [Newtonsoft.Json.Linq.JObject]::Parse([IO.File]::ReadAllText($planPath))
    $res = Load-Resource $out
    if ($res.SkeletalAnims.Count -ne 1 -or $res.MaterialAnims.Count -ne 0) { throw 'Finished file holds more than the one animation' }
    $anim = $res.SkeletalAnims[0]
    if ($anim.Name -ne $animationName -or $anim.FrameCount -ne $frames -or !$anim.Loop -or $anim.BoneAnims.Count -ne 31) { throw 'Finished animation header changed' }
    $checked = 0; $worst = 0.0
    foreach ($bone in $anim.BoneAnims) {
        $expected = $plan['curves'][[string]$bone.Name]
        $have = if ($expected) { @($expected.Properties()).Count } else { 0 }
        if ($bone.Curves.Count -ne $have) { throw "Bone $($bone.Name) has $($bone.Curves.Count) curves, the plan has $have" }
        foreach ($curve in $bone.Curves) {
            $label = $targetName[[int]$curve.AnimDataOffset]
            $want = $expected[$label]
            if (!$want) { throw "Bone $($bone.Name) has an unplanned curve at $($curve.AnimDataOffset)" }
            $got = Samples $curve
            $span = 0.0
            for ($f=0; $f -le $frames; $f++) { $span = [Math]::Max($span,[Math]::Abs([double]$want[$f])) }
            for ($f=0; $f -le $frames; $f++) {
                $difference = [Math]::Abs($got[$f]-[double]$want[$f])
                $worst = [Math]::Max($worst,$difference)
                if ($difference -gt 1e-4+1e-4*$span) { throw "Bone $($bone.Name) $label frame $f reads $($got[$f]), planned $([double]$want[$f])" }
            }
            $checked++
        }
    }
    if ($checked -ne [int]$plan['curve_count']) { throw "Verified $checked curves, the plan has $([int]$plan['curve_count'])" }
    Write-Host ("Verified {0} animation: {1} curves read back as planned (largest difference {2:E2})" -f $Project,$checked,$worst)
    return
}

if (!$SourceAnim -or !$NativeAnim) { throw 'SourceAnim and NativeAnim are required to build' }
if ($Scale -le 0) { throw 'Scale must be positive' }
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$res = Load-Resource $SourceAnim
$native = Load-Resource $NativeAnim
if ($res.UnresolvedExternalStringIDs.Count -or $native.UnresolvedExternalStringIDs.Count) { throw 'An animation file has unresolved names' }
$anim = $res.SkeletalAnims | Where-Object Name -eq 'Float'
if (!$anim -or $anim.FrameCount -ne $frames -or !$anim.Loop -or $anim.BoneAnims.Count -ne 31 -or
    $anim.FlagsRotate.ToString() -ne 'EulerXYZ' -or $native.SkeletalAnims[0].FlagsRotate.ToString() -ne 'EulerXYZ') { throw 'Unexpected TotK paraglider animation' }
$cloth = $anim.BoneAnims | Where-Object Name -eq 'Cloth'
if (!$cloth -or (@($cloth.Curves | ForEach-Object { [int]$_.AnimDataOffset }) -join ',') -ne '40,16,20,24') { throw 'Unexpected canopy curves' }
foreach ($bone in $anim.BoneAnims) { if ($bone.Name -ne 'Cloth' -and $bone.Curves.Count) { throw "Unexpected curves on $($bone.Name)" } }

# One Hermite key per frame; the slope is the central difference, taken around the loop.
function Dense-Curve([int]$target,[double[]]$values) {
    if ([Math]::Abs($values[$frames]-$values[0]) -gt 1e-4) { throw "Curve $target does not loop" }
    $values[$frames] = $values[0]
    $slope = New-Object 'double[]' ($frames+1)
    for ($k=0; $k -le $frames; $k++) {
        $next = $values[($k+1) % $frames]; $previous = $values[($k-1+$frames) % $frames]
        $slope[$k] = ($next-$previous)/2
    }
    $curve = New-Object Syroot.NintenTools.NSW.Bfres.AnimCurve
    $curve.AnimDataOffset = [uint32]$target
    $curve.FrameType = [Syroot.NintenTools.NSW.Bfres.AnimCurveFrameType]::Single
    $curve.KeyType = [Syroot.NintenTools.NSW.Bfres.AnimCurveKeyType]::Single
    $curve.CurveType = [Syroot.NintenTools.NSW.Bfres.AnimCurveType]::Cubic
    $curve.StartFrame = 0; $curve.EndFrame = $frames; $curve.Scale = 1; $curve.Delta = 0
    $curve.Offset = New-Object Syroot.NintenTools.NSW.Bfres.DWord
    $curve.Frames = [float[]](0..$frames)
    $keys = New-Object 'float[,]' ($frames+1),4
    for ($k=0; $k -le $frames; $k++) {
        $keys[$k,0] = [float]$values[$k]
        if ($k -lt $frames) {
            $step = $values[$k+1]-$values[$k]
            $keys[$k,1] = [float]$slope[$k]
            $keys[$k,2] = [float](3*$step-2*$slope[$k]-$slope[$k+1])
            $keys[$k,3] = [float](-2*$step+$slope[$k]+$slope[$k+1])
        }
    }
    $curve.Keys = $keys
    return $curve
}

$factor = [float]$Scale
$planned = [ordered]@{}
foreach ($bone in $anim.BoneAnims) {
    $base = $bone.BaseData
    $base.Translate = New-Object Syroot.Maths.Vector3F(($base.Translate.X*$factor),($base.Translate.Y*$factor),($base.Translate.Z*$factor))
    $bone.BaseData = $base
}

# Canopy: TotK's own curves, sampled per frame; translations follow the model scale.
$clothPlan = [ordered]@{}
$clothCurves = New-Object 'System.Collections.Generic.List[Syroot.NintenTools.NSW.Bfres.AnimCurve]'
foreach ($source in $cloth.Curves) {
    $target = [int]$source.AnimDataOffset
    $values = [double[]](Samples $source)
    if ($target -lt 32) { for ($f=0; $f -le $frames; $f++) { $values[$f] = $values[$f]*$Scale } }
    $clothCurves.Add((Dense-Curve $target $values))
    $clothPlan[$targetName[$target]] = $values
}
$cloth.Curves = $clothCurves
$planned['Cloth'] = $clothPlan

# Tails: joints after the first swing about Z and Y in a wave growing toward the tip (the first keep a quarter-turn rest pose).
$tipBound = 0.0
if ($Flutter) {
    $curveFlags = $cloth.FlagsCurve.GetType()
    # Length of tail behind each joint of a long tail (the mesh runs about 0.3 m past the last joint).
    $behind = @{ 6 = 0.3*$Scale }
    for ($joint=5; $joint -ge 2; $joint--) {
        $nextJoint = $anim.BoneAnims | Where-Object Name -eq "Tail_$($joint+1)_L"
        $behind[$joint] = $behind[$joint+1]+[Math]::Abs($nextJoint.BaseData.Translate.X)
    }
    foreach ($bone in $anim.BoneAnims) {
        if ($bone.Name -notmatch '^Tail_(?:(?<joint>[2-6])|(?<short>[A-D])_2)_(?<side>[LR])$') { continue }
        $rest = $bone.BaseData.Rotate
        if ([Math]::Abs($rest.X) -gt 1e-4 -or [Math]::Abs($rest.Y) -gt 1e-4 -or [Math]::Abs($rest.Z) -gt 1e-4) { throw "Tail joint $($bone.Name) is not at rest" }
        $right = $Matches['side'] -eq 'R'
        if ($Matches['joint']) { $joint = [int]$Matches['joint']; $amplitude = 0.02+0.015*$joint; $phase = 0.9*$joint }
        else { $joint = 2; $amplitude = 0.10; $phase = 1.3*([int][char]$Matches['short']-[int][char]'A') }
        if ($right) { $phase += 0.7 }
        $pitch = [double[]]::new($frames+1); $yaw = [double[]]::new($frames+1)
        for ($f=0; $f -le $frames; $f++) {
            $turn = 2*[Math]::PI*($f % $frames)/$frames
            $pitch[$f] = $amplitude*[Math]::Sin($turn-$phase)
            # Opposite sign on the right so both tails lean the same way in the world.
            $yaw[$f] = 0.5*$amplitude*[Math]::Sin(2*$turn-$phase)*$(if ($right) { -1 } else { 1 })
        }
        $curves = New-Object 'System.Collections.Generic.List[Syroot.NintenTools.NSW.Bfres.AnimCurve]'
        $curves.Add((Dense-Curve 36 $yaw)); $curves.Add((Dense-Curve 40 $pitch))
        $bone.Curves = $curves
        $bone.FlagsCurve = [Enum]::Parse($curveFlags,'RotateY, RotateZ')
        $bone.FlagsTransform = $cloth.FlagsTransform
        $planned[[string]$bone.Name] = [ordered]@{ RotateY=$yaw; RotateZ=$pitch }
        if (!$right -and $bone.Name -match '^Tail_[2-6]_L$') { $tipBound += $amplitude*$behind[$joint] }
    }
    # Upper bound on how far a long tail's tip can rise or fall: each joint's largest angle times the length behind it.
    if ($tipBound -gt 0.5*$Scale) { throw "Tail flutter moves the tip up to $tipBound m" }
}

$curveCount = 0; $baked = 0
foreach ($bone in $anim.BoneAnims) { foreach ($curve in $bone.Curves) { $curveCount++; $baked += 4*($curve.EndFrame-$curve.StartFrame+1)+28 } }
$anim.Name = $animationName
$anim.BakedSize = [uint32]$baked
$res.SkeletalAnims.Clear(); $res.SkeletalAnims.Add($anim)
$res.SkeletalAnimDict.Clear(); $res.SkeletalAnimDict.Add($animationName)
$res.MaterialAnims.Clear(); $res.MaterialAnimDict.Clear()
$res.ExternalFiles.Clear(); $res.ExternalFileDict.Clear()
$res.Name = "${Project}_Animation.anim"
Native-Version $res $native
$res.Save($out)

$plan = [ordered]@{
    project=$Project; animation=$animationName; frames=$frames; scale=$Scale; flutter=[bool]$Flutter
    curve_count=$curveCount; baked_size=[int]$baked; tail_tip_bound_m=$tipBound; curves=$planned
    source_sha256=(Get-FileHash -LiteralPath $SourceAnim).Hash.ToLowerInvariant()
}
[IO.File]::WriteAllText($planPath,(Json $plan))
Write-Host "Wrote $Project animation: $curveCount curves, $frames frames"
