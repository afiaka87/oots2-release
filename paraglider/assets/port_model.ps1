# SPDX-License-Identifier: MIT
# Copyright (c) HanSoloBlaster, afiaka87
# Converts TotK's paraglider model (decoded Item_Parasail BFRES) into a remake model: TotK geometry and skeleton,
# materials copied from Animal_Cucco's Mt_Body_PBR. -Scale sets the size (TotK's = 1); -TexturePrefix shares textures.
param(
    [Parameter(Mandatory=$true)][string]$ToolboxDir,
    [Parameter(Mandatory=$true)][string]$UnsafeCompatPath,
    [Parameter(Mandatory=$true)][string]$TotkRomfs,
    [Parameter(Mandatory=$true)][string]$SourceModel,
    [Parameter(Mandatory=$true)][string]$DonorModel,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$Project = 'Obj_Paraglider',
    [string]$TexturePrefix = '',
    [double]$Scale = 1.0
)
$ErrorActionPreference = 'Stop'
foreach ($name in 'ToolboxDir','UnsafeCompatPath','TotkRomfs','SourceModel','DonorModel','OutputDirectory') {
    Set-Variable $name ([IO.Path]::GetFullPath((Get-Variable $name -ValueOnly)))
}
. (Join-Path $PSScriptRoot 'toolbox_oot.ps1') -ToolboxDir $ToolboxDir -UnsafeCompatPath $UnsafeCompatPath -TotkRomfs $TotkRomfs | Out-Null

$helperType = [Syroot.NintenTools.NSW.Bfres.Helpers.VertexBufferHelper]
$donorMaterialName = 'Mt_Body_PBR'
$layoutNames = '_p0','_n0','_t0','_u0','_i0','_w0'
if (!$TexturePrefix) { $TexturePrefix = $Project }
if ($Scale -le 0) { throw 'Scale must be positive' }
$factor = [float]$Scale
# Colour and normal maps are per part; roughness and occlusion are one constant texture each.
$partTexture = @{ '_a0'='Alb'; '_n0'='Nrm' }
$sharedTexture = @{ '_rgh0'='Rgh'; '_ao0'='AO' }

function Vec([float]$x,[float]$y,[float]$z,[float]$w) { New-Object Syroot.Maths.Vector4F($x,$y,$z,$w) }
function Attribute-Map($helper) { $map = @{}; foreach ($a in $helper.Attributes) { $map[$a.Name] = $a }; $map }
function Positions($vertexBuffer) {
    $map = Attribute-Map (New-Object $helperType ($vertexBuffer,$null))
    ,@($map['_p0'].Data | ForEach-Object { '{0:R};{1:R};{2:R}' -f $_.X,$_.Y,$_.Z })
}
function Key-Index($dictionary,[string]$name) {
    for ($i=0; $i -lt $dictionary.Count; $i++) { if ($dictionary.GetKey($i) -ceq $name) { return $i } }
    return -1
}

[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
$res = Load-Resource $SourceModel
$donorBody = Load-Resource $DonorModel
$donorFabric = Load-Resource $DonorModel
foreach ($file in $res,$donorBody,$donorFabric) {
    if ($file.UnresolvedExternalStringIDs.Count) { throw 'A model has unresolved names (wrong external string table)' }
}
$model = $res.Models[0]
if ($res.Models.Count -ne 1 -or $model.Name -ne 'Item_Parasail' -or $model.Skeleton.Bones.Count -ne 31 -or
    $model.Shapes.Count -ne 2 -or $model.VertexBuffers.Count -ne 2 -or $model.Materials.Count -ne 2 -or
    $model.Materials[0].Name -ne 'Mt_Body' -or $model.Materials[1].Name -ne 'Mt_Fabric') {
    throw 'Unexpected TotK paraglider model'
}
$donorModel0 = $donorBody.Models[0]
if ($donorModel0.Name -ne 'Animal_Cucco') { throw 'Unexpected donor model' }
$donorShape = $donorModel0.Shapes | Where-Object { $donorModel0.Materials[$_.MaterialIndex].Name -eq $donorMaterialName -and $_.VertexSkinCount -eq 4 } | Select-Object -First 1
if (!$donorShape) { throw 'Donor has no four-weight shape with the donor material' }
$layout = $donorModel0.VertexBuffers[$donorShape.VertexBufferIndex]
if ((@($layout.Attributes | ForEach-Object Name) -join ',') -ne ($layoutNames -join ',')) { throw 'Donor vertex layout changed' }

# Geometry: rewrite both vertex buffers with the donor's attribute set and formats, four weights each.
$before = @()
$low = [double[]]@(1e9,1e9,1e9); $high = [double[]]@(-1e9,-1e9,-1e9)
for ($v=0; $v -lt 2; $v++) {
    $sourceBuffer = $model.VertexBuffers[$v]
    $shape = $model.Shapes | Where-Object VertexBufferIndex -eq $v
    $src = Attribute-Map (New-Object $helperType ($sourceBuffer,$null))
    $count = $src['_p0'].Data.Length
    $rigid = $sourceBuffer.VertexSkinCount -eq 1
    if (!$rigid -and $sourceBuffer.VertexSkinCount -ne 4) { throw 'Unexpected source skin count' }
    $target = New-Object $helperType ($layout,$null)
    foreach ($attribute in $target.Attributes) {
        $data = New-Object 'Syroot.Maths.Vector4F[]' $count
        $from = $src[$attribute.Name]
        for ($i=0; $i -lt $count; $i++) {
            switch ($attribute.Name) {
                '_p0' {
                    $p = $from.Data[$i]; $data[$i] = Vec ($p.X*$factor) ($p.Y*$factor) ($p.Z*$factor) 1
                    $point = @($data[$i].X,$data[$i].Y,$data[$i].Z)
                    for ($axis=0; $axis -lt 3; $axis++) {
                        $low[$axis] = [Math]::Min($low[$axis],$point[$axis]); $high[$axis] = [Math]::Max($high[$axis],$point[$axis])
                    }
                }
                '_i0' {
                    if ($rigid) { if ($from.Data[$i].X -ne 22) { throw 'Rigid body vertex not on Skl_Root' }; $data[$i] = Vec 0 0 0 0 }
                    else { $data[$i] = $from.Data[$i] }
                }
                '_w0' { if ($rigid) { $data[$i] = Vec 1 0 0 0 } else { $data[$i] = $from.Data[$i] } }
                default { $data[$i] = $from.Data[$i] }
            }
        }
        $attribute.Data = $data
    }
    $target.VertexSkinCount = 4
    $converted = $target.ToVertexBuffer()
    $converted.VertexSkinCount = 4
    $before += ,(Positions $converted)
    $model.VertexBuffers[$v] = $converted
    $shape.VertexSkinCount = 4
}

# Detail levels: TotK's second level is far coarser than the remake's small steps (frame 3,630 -> 384 indices),
# so both levels draw the full mesh, as vanilla does for models with no simpler mesh.
$fullIndices = @()
foreach ($shape in $model.Shapes) {
    $fine = $shape.Meshes[0]; $coarse = $shape.Meshes[1]
    if ($shape.Meshes.Count -ne 2 -or $fine.SubMeshes.Count -ne 1 -or $coarse.SubMeshes.Count -ne 1 -or
        $fine.SubMeshes[0].Offset -ne 0 -or $fine.SubMeshes[0].Count -ne $fine.IndexCount) { throw 'Unexpected detail levels' }
    $indices = New-Object 'System.Collections.Generic.List[uint32]'
    foreach ($i in $fine.GetIndices()) { $indices.Add([uint32]$i) }
    $coarse.SetIndices($indices, $fine.IndexFormat)
    $coarse.PrimitiveType = $fine.PrimitiveType; $coarse.FirstVertex = $fine.FirstVertex
    $coarse.SubMeshes[0].Offset = 0; $coarse.SubMeshes[0].Count = $fine.IndexCount
    $fullIndices += ,(@($indices) -join ',')
}

# Skeleton: the body was rigid on Skl_Root (bone 1); it is smooth-skinned now, so drop the rigid matrix.
$skeleton = $model.Skeleton
$matrices = New-Object 'System.Collections.Generic.List[uint16]'
foreach ($index in $skeleton.MatrixToBoneList) { $matrices.Add([uint16]$index) }
if ($matrices.Count -ne 23 -or $matrices[22] -ne 1 -or $skeleton.Bones[1].RigidMatrixIndex -ne 22 -or
    $skeleton.Bones[1].SmoothMatrixIndex -ne 0 -or $skeleton.Bones[1].Name -ne 'Skl_Root') { throw 'Unexpected TotK skeleton' }
$matrices.RemoveAt(22)
$skeleton.MatrixToBoneList = $matrices
$skeleton.Bones[1].RigidMatrixIndex = -1
foreach ($bone in $skeleton.Bones) { if ($bone.RigidMatrixIndex -ne -1) { throw 'Unexpected rigid bone' } }

# Uniform scale: bone offsets, the bind matrices' translation column and the culling volumes follow the vertices.
if ($factor -ne 1) {
    foreach ($bone in $skeleton.Bones) {
        if ($bone.Scale.X -ne 1 -or $bone.Scale.Y -ne 1 -or $bone.Scale.Z -ne 1) { throw 'Unexpected bone scale' }
        $bone.Position = New-Object Syroot.Maths.Vector3F(($bone.Position.X*$factor),($bone.Position.Y*$factor),($bone.Position.Z*$factor))
    }
    for ($i=0; $i -lt $skeleton.InverseModelMatrices.Count; $i++) {
        $matrix = $skeleton.InverseModelMatrices[$i]
        $matrix.M14 *= $factor; $matrix.M24 *= $factor; $matrix.M34 *= $factor
        $skeleton.InverseModelMatrices[$i] = $matrix
    }
    foreach ($shape in $model.Shapes) {
        for ($i=0; $i -lt $shape.RadiusArray.Count; $i++) { $shape.RadiusArray[$i] = $shape.RadiusArray[$i]*$factor }
        for ($i=0; $i -lt $shape.BoundingRadiusList.Count; $i++) {
            $sphere = $shape.BoundingRadiusList[$i]
            $shape.BoundingRadiusList[$i] = Vec ($sphere.X*$factor) ($sphere.Y*$factor) ($sphere.Z*$factor) ($sphere.W*$factor)
        }
        for ($i=0; $i -lt $shape.SubMeshBoundings.Count; $i++) {
            $box = $shape.SubMeshBoundings[$i]
            $box.Center = New-Object Syroot.Maths.Vector3F(($box.Center.X*$factor),($box.Center.Y*$factor),($box.Center.Z*$factor))
            $box.Extent = New-Object Syroot.Maths.Vector3F(($box.Extent.X*$factor),($box.Extent.Y*$factor),($box.Extent.Z*$factor))
            $shape.SubMeshBoundings[$i] = $box
        }
    }
}
$expectedRadius = @($model.Shapes | ForEach-Object { [float]$_.RadiusArray[0] })
$expectedBone = [float]$skeleton.Bones[2].Position.Y
$expectedMatrix = @($skeleton.InverseModelMatrices | ForEach-Object { '{0:R};{1:R};{2:R}' -f $_.M14,$_.M24,$_.M34 })

# Materials: vanilla donor copies with our texture names. TotK draws both parts front-face only, as the donor does.
$records = @()
for ($m=0; $m -lt 2; $m++) {
    $part = @('Body','Fabric')[$m]
    $donorFile = @($donorBody,$donorFabric)[$m]
    $material = $donorFile.Models[0].Materials | Where-Object Name -eq $donorMaterialName
    if (!$material -or $material.ShaderAssign.ShaderArchiveName -ne 'Model' -or $material.ShaderAssign.ShadingModelName -ne 'model') { throw 'Donor material missing' }
    $shader = Json $material.ShaderAssign
    $material.Name = $model.Materials[$m].Name
    if ($material.SamplerDict.Count -ne 4) { throw 'Donor sampler set changed' }
    for ($s=0; $s -lt 4; $s++) {
        $key = $material.SamplerDict.GetKey($s)
        if ($partTexture.ContainsKey($key)) { $material.TextureRefs[$s] = "${TexturePrefix}_${part}_$($partTexture[$key])" }
        elseif ($sharedTexture.ContainsKey($key)) { $material.TextureRefs[$s] = "${TexturePrefix}_$($sharedTexture[$key])" }
        else { throw "Unexpected donor sampler $key" }
    }
    $face = $material.RenderInfos | Where-Object Name -eq 'gsys_render_state_display_face'
    if (!$face -or ($face.GetValueStrings() -join ',') -ne 'front') { throw 'Donor display face changed' }
    if ((Json $material.ShaderAssign) -ne $shader) { throw 'Donor shader options changed' }
    $model.Materials[$m] = $material
    $records += [ordered]@{ material=$material.Name; donor=$donorMaterialName; textures=@($material.TextureRefs) }
}
$model.UserData = $donorModel0.UserData
$model.UserDataDict = $donorModel0.UserDataDict

$res.Name = "$Project.$Project"
$model.Name = $Project
$res.ModelDict.Clear(); $res.ModelDict.Add($Project)
$res.ExternalFiles.Clear(); $res.ExternalFileDict.Clear()
Native-Version $res $donorBody

$out = Join-Path $OutputDirectory "$Project.$Project.bfres"
$res.Save($out)
# A first save leaves the geometry block off a 4096-byte boundary; saving the reloaded file aligns it.
(Load-Resource $out).Save($out)
$saved = [IO.File]::ReadAllBytes($out)
$bufferInfo = [BitConverter]::ToInt64($saved,0xB0)
$bufferSize = [BitConverter]::ToUInt32($saved,$bufferInfo+4)
$bufferStart = [BitConverter]::ToInt64($saved,$bufferInfo+8)
if ($bufferStart % 4096 -or $bufferSize % 4096) { throw 'Geometry block is not 4096-byte aligned' }

$after = Load-Resource $out
$check = $after.Models[0]
if ($after.VersionMajor -ne $donorBody.VersionMajor -or $after.VersionMajor2 -ne $donorBody.VersionMajor2 -or
    $after.VersionMinor -ne $donorBody.VersionMinor -or $after.VersionMinor2 -ne $donorBody.VersionMinor2) { throw 'Version did not follow the donor' }
if ($after.Name -ne "$Project.$Project" -or $check.Name -ne $Project -or $check.Shapes.Count -ne 2 -or
    $check.Skeleton.Bones.Count -ne 31 -or $check.Skeleton.MatrixToBoneList.Count -ne 22 -or
    $check.Skeleton.InverseModelMatrices.Count -ne 22) { throw 'Saved model structure changed' }
for ($v=0; $v -lt 2; $v++) {
    $buffer = $check.VertexBuffers[$v]
    if ((@($buffer.Attributes | ForEach-Object Name) -join ',') -ne ($layoutNames -join ',')) { throw 'Saved vertex layout changed' }
    $now = Positions $buffer
    if ($now.Count -ne $before[$v].Count) { throw 'Vertex count changed' }
    for ($i=0; $i -lt $now.Count; $i++) { if ($now[$i] -ne $before[$v][$i]) { throw "Vertex position changed at $v/$i" } }
    $map = Attribute-Map (New-Object $helperType ($buffer,$null))
    foreach ($weights in $map['_w0'].Data) {
        if ([Math]::Abs($weights.X+$weights.Y+$weights.Z+$weights.W-1) -gt 0.02) { throw 'Skin weights do not sum to one' }
    }
}
for ($m=0; $m -lt 2; $m++) {
    $material = $check.Materials[$m]
    if ($material.ShaderAssign.ShaderArchiveName -ne 'Model' -or $material.Name -ne @('Mt_Body','Mt_Fabric')[$m]) { throw 'Saved material changed' }
}
for ($s=0; $s -lt 2; $s++) {
    if ([float]$check.Shapes[$s].RadiusArray[0] -ne $expectedRadius[$s]) { throw 'Culling radius did not survive the save' }
    $levels = $check.Shapes[$s].Meshes
    if ($levels.Count -ne 2) { throw 'Detail level count changed' }
    foreach ($level in $levels) {
        if ((@($level.GetIndices()) -join ',') -ne $fullIndices[$s] -or $level.SubMeshes[0].Count -ne $level.IndexCount) {
            throw 'A detail level does not hold the full mesh after the save'
        }
    }
}
if ([float]$check.Skeleton.Bones[2].Position.Y -ne $expectedBone) { throw 'Bone offsets did not survive the save' }
$savedMatrix = @($check.Skeleton.InverseModelMatrices | ForEach-Object { '{0:R};{1:R};{2:R}' -f $_.M14,$_.M24,$_.M34 })
if (($savedMatrix -join '|') -ne ($expectedMatrix -join '|')) { throw 'Bind matrices did not survive the save' }

$receipt = [ordered]@{
    project=$Project; scale=$Scale; bones=31; smooth_matrices=22; shapes=@($check.Shapes | ForEach-Object { [string]$_.Name })
    vertices=@($before | ForEach-Object { [int]$_.Count }); materials=$records
    detail_levels=@($check.Shapes | ForEach-Object { ,@($_.Meshes | ForEach-Object { [int]$_.IndexCount }) })
    radius=@($expectedRadius | ForEach-Object { [double]$_ })
    bounds=[ordered]@{ min=$low; max=$high }
    version=('{0}.{1}.{2}.{3}' -f $after.VersionMajor,$after.VersionMajor2,$after.VersionMinor,$after.VersionMinor2)
    geometry_block=[ordered]@{ offset=$bufferStart; size=$bufferSize }
    source_sha256=(Get-FileHash -LiteralPath $SourceModel).Hash.ToLowerInvariant()
    donor_sha256=(Get-FileHash -LiteralPath $DonorModel).Hash.ToLowerInvariant()
    raw_sha256=(Get-FileHash -LiteralPath $out).Hash.ToLowerInvariant()
    raw_bytes=(Get-Item -LiteralPath $out).Length
}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'model-receipt.json'),(Json $receipt))
Write-Host "Verified $Project model: geometry, skeleton, materials and alignment"
