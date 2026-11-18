# SPDX-License-Identifier: MIT
# Copyright (c) HanSoloBlaster, afiaka87
"""Actor pack for the held paraglider (Obj_Paraglider and its child-size variant), built from vanilla parts.

Lift settings are Animal_Cucco's (hold point moved), the body is Obj_Item_Base's drop-item body, and the animation
setup is Animal_Octopus's ("Wait" in a loop); no AI, life, damage or sensor component. Like vanilla packs it never
repeats a ResidentCommon file, and every "?" reference must resolve.
"""

from __future__ import annotations

import math
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

import zstandard

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "tools"))
import byml7  # noqa: E402
from byml7 import F32  # noqa: E402

GRIP = (0.286, 0.010, -0.057)  # right-hand grip centre from the model origin, TotK size, metres
BODY_RADIUS = 0.05
SEQUENCE = "?AS/Animal_Octopus.root.asb"
# References the game resolves without a romfs file of that name inside a pack or ResidentCommon.
NO_PACK_FILE = ("?ActorSystem/PreActorSetting/ActorOnly.game__actor__PreActorSetting.bgyml",)


class PackError(ValueError):
    pass


@dataclass(frozen=True)
class Variant:
    name: str
    scale: float
    raise_m: float = 0.0  # lifts the paraglider above the hold point, for hands that end above the grips


VARIANTS = (Variant("Obj_Paraglider", 0.9, 0.10), Variant("Obj_Paraglider_Child", 0.65))


def f32(value: float) -> F32:
    return F32(struct.unpack("<I", struct.pack("<f", value))[0])


def vector(x: float, y: float, z: float) -> dict:
    return {"X": f32(x), "Y": f32(y), "Z": f32(z)}


def sarc_read(data: bytes) -> dict[str, bytes]:
    if data[:4] != b"SARC" or data[6:8] != b"\xff\xfe":
        raise PackError("not a little-endian SARC archive")
    header, _, data_start = struct.unpack_from("<HxxII", data, 4)
    count, multiplier = struct.unpack_from("<HI", data, header + 6)
    if data[header:header + 4] != b"SFAT" or multiplier != 101:
        raise PackError("unexpected SARC file table")
    nodes = header + 0x0C
    names = nodes + 0x10 * count + 8
    files = {}
    for index in range(count):
        _, attributes, start, end = struct.unpack_from("<IIII", data, nodes + 0x10 * index)
        at = names + (attributes & 0xFFFF) * 4
        files[data[at:data.index(b"\0", at)].decode()] = data[data_start + start:data_start + end]
    return files


def sarc_hash(name: str) -> int:
    value = 0
    for byte in name.encode():
        value = (value * 101 + byte) & 0xFFFFFFFF
    return value


def sarc_write(files: dict[str, bytes]) -> bytes:
    """The game's own layout: entries by name hash, names padded to 4 bytes, every file aligned to 8."""
    order = sorted(files, key=sarc_hash)
    if len({sarc_hash(name) for name in order}) != len(order):
        raise PackError("SARC name hash collision")
    names, nodes, data = bytearray(), bytearray(), bytearray()
    for name in order:
        data += bytes(-len(data) % 8)
        nodes += struct.pack("<IIII", sarc_hash(name), 0x01000000 | len(names) // 4, len(data), len(data) + len(files[name]))
        data += files[name]
        names += name.encode() + b"\0"
        names += bytes(-len(names) % 4)
    head = 0x14 + 0x0C + len(nodes) + 8 + len(names)
    data_start = head + (-head % 8)
    out = struct.pack("<4sHHIIHH", b"SARC", 0x14, 0xFEFF, data_start + len(data), data_start, 0x0100, 0)
    out += struct.pack("<4sHHI", b"SFAT", 0x0C, len(order), 101) + bytes(nodes)
    out += struct.pack("<4sHH", b"SFNT", 8, 0) + bytes(names)
    return out + bytes(data_start - head) + bytes(data)


class Game:
    """Read-only access to extracted game files (one or more folders of the same game version)."""

    def __init__(self, roots: list[Path]):
        self.roots = roots
        self.dictionaries = {}
        self._resident = None
        for name, blob in sarc_read(self._plain(self.read("Pack/ZsDic.pack.zs"))).items():
            if name.endswith(".zsdic"):
                dictionary = zstandard.ZstdCompressionDict(blob)
                self.dictionaries[dictionary.dict_id()] = dictionary

    @property
    def resident(self) -> dict[str, bytes]:
        if self._resident is None:
            self._resident = self.pack("Pack/ResidentCommon.pack.zs")
        return self._resident

    @staticmethod
    def _plain(blob: bytes) -> bytes:
        return zstandard.ZstdDecompressor().decompress(blob, max_output_size=1 << 28)

    def path(self, relative: str) -> Path:
        for root in self.roots:
            if (root / relative).is_file():
                return root / relative
        raise PackError(f"{relative} is in none of {[str(root) for root in self.roots]}")

    def has(self, relative: str) -> bool:
        return any((root / relative).is_file() for root in self.roots)

    def read(self, relative: str) -> bytes:
        return self.path(relative).read_bytes()

    def load(self, relative: str) -> tuple[bytes, int]:
        return self.unzs(self.read(relative))

    def unzs(self, blob: bytes) -> tuple[bytes, int]:
        dict_id = zstandard.get_frame_parameters(blob).dict_id
        if dict_id and dict_id not in self.dictionaries:
            raise PackError(f"frame needs zstd dictionary {dict_id}, which the game does not hold")
        options = {"dict_data": self.dictionaries[dict_id]} if dict_id else {}
        return zstandard.ZstdDecompressor(**options).decompress(blob, max_output_size=1 << 30), dict_id

    def rezs(self, raw: bytes, dict_id: int) -> bytes:
        options = {"dict_data": self.dictionaries[dict_id]} if dict_id else {}
        blob = zstandard.ZstdCompressor(level=19, write_content_size=True, **options).compress(raw)
        if self.unzs(blob) != (raw, dict_id):
            raise PackError("compression round trip failed")
        return blob

    def pack(self, relative: str) -> dict[str, bytes]:
        return sarc_read(self.unzs(self.read(relative))[0])

    def actor_pack(self, actor: str) -> dict[str, bytes]:
        return self.pack(f"Pack/Actor/{actor}.pack.zs")


def selftest(game: Game) -> None:
    """The archive writer rebuilds vanilla packs byte for byte; the BYML writer keeps the rewritten values."""
    rewritten = ("Component/LiftableParam/", "Component/Physics/", "Component/ModelInfo/", "Component/AnimationParam/",
                 "Phive/LocatorContainerParam/", "Phive/ControllerSetParam/", "Phive/RigidBodyEntityParam/", "Phive/ShapeParam/")
    checked = 0
    for actor in ("Animal_Cucco", "Animal_Octopus", "Obj_Item_Base"):
        raw, _ = game.unzs(game.read(f"Pack/Actor/{actor}.pack.zs"))
        files = sarc_read(raw)
        if sarc_write(files) != raw:
            raise PackError(f"{actor}: rebuilt archive differs from vanilla")
        for name, data in files.items():
            if name.startswith(rewritten) or name.startswith("Actor/" + actor):
                version, tree = byml7.load(data)
                if byml7.typed(byml7.load(byml7.dump(tree, version))[1]) != byml7.typed(tree):
                    raise PackError(f"{actor}: {name} does not survive a BYML rewrite")
                checked += 1
    if checked < 12:
        raise PackError("vanilla packs no longer hold the template files")


def _tree(files: dict[str, bytes], name: str) -> dict:
    if name not in files:
        raise PackError(f"template file {name} is missing")
    version, tree = byml7.load(files[name])
    if version != 7 or not isinstance(tree, dict):
        raise PackError(f"{name}: unexpected BYML version or root")
    return tree


def _dump(tree: dict) -> bytes:
    data = byml7.dump(tree, 7)
    if byml7.typed(byml7.load(data)[1]) != byml7.typed(tree):
        raise PackError("BYML re-read differs from the tree written")
    return data


def _expect(tree: dict, key: str, value) -> None:
    if key not in tree or byml7.typed(tree[key]) != byml7.typed(value):
        raise PackError(f"template changed: {key} is {tree.get(key)!r}, expected {value!r}")


def sphere_shape(template: dict, radius: float) -> dict:
    """The drop item's sphere with another radius; volume and inertia follow the template's own rounding."""
    sphere = template["Sphere"][0]
    old = sphere["Radius"].value
    auto = template["AutoCalc"]
    if len(template["Sphere"]) != 1 or abs(auto["Volume"].value - 4 / 3 * math.pi * old ** 3) > 1e-5 or any(
        abs(auto["Tensor"][axis].value - 0.4 * old * old) > 1e-6 for axis in "XYZ"
    ):
        raise PackError("drop-item shape template no longer follows the sphere formulas")
    return {
        "AutoCalc": {
            "Axis": vector(0, 0, 0), "Center": vector(0, 0, 0),
            "Max": vector(radius, radius, radius), "Min": vector(-radius, -radius, -radius),
            "Tensor": vector(*[0.4 * radius * radius] * 3), "Volume": f32(4 / 3 * math.pi * radius ** 3),
        },
        "Sphere": [{"Center": vector(0, 0, 0), "MaterialPresets": list(sphere["MaterialPresets"]), "Radius": f32(radius)}],
    }


def build_pack(game: Game, variant: Variant, animated: bool) -> dict[str, bytes]:
    name, scale = variant.name, variant.scale
    cucco, octopus, item = game.actor_pack("Animal_Cucco"), game.actor_pack("Animal_Octopus"), game.actor_pack("Obj_Item_Base")
    table = game.actor_pack("Item_FairyBless_DefenseDouble")
    files: dict[str, bytes] = {}

    # the cucco's lift settings; only the hold point moves (the grips sit at the paraglider's origin)
    lift = _tree(cucco, "Component/LiftableParam/Animal_Cucco.game__component__LiftableParam.bgyml")
    _expect(lift, "LiftableType", byml7.U32(1577663790))
    _expect(lift, "LiftPosOffset", vector(0.0, -0.2125, -0.025))
    _expect(lift, "PutPosOffset", vector(0.0, -0.2125, -0.025))
    hold = vector(0.0, -GRIP[1] * scale + variant.raise_m, -GRIP[2] * scale)
    lift["LiftPosOffset"], lift["PutPosOffset"] = hold, dict(hold)
    files[f"Component/LiftableParam/{name}.game__component__LiftableParam.bgyml"] = _dump(lift)

    locators = _tree(cucco, "Phive/LocatorContainerParam/Animal_Cucco.phive__LocatorContainerParam.bgyml")
    rows = locators["BoneLocatorList"]
    if list(locators) != ["BoneLocatorList"] or len(rows) != 2 or any(row["BoneName"] != "Skl_Root" for row in rows) or not (
        rows[0]["OffsetTranslation"]["X"].value > 0 > rows[1]["OffsetTranslation"]["X"].value
    ):
        raise PackError("cucco hand locators changed")
    for row, side in zip(rows, (1, -1)):
        row["OffsetTranslation"] = vector(side * GRIP[0] * scale, GRIP[1] * scale, GRIP[2] * scale)
    files[f"Phive/LocatorContainerParam/{name}.phive__LocatorContainerParam.bgyml"] = _dump(locators)

    # Body: the drop item's, without its pick-up sensor. Its hit mask is ground and objects only.
    shared_mask = "Phive/ContactCollectionEntityParam/HitOnlyGroundAndObject.phive__ContactCollectionEntityParam.bgyml"
    shared_controller = "Phive/RigidBodyControllerEntityParam/Item_Pouch.phive__RigidBodyControllerEntityParam.bgyml"
    files[shared_mask], files[shared_controller] = item[shared_mask], item[shared_controller]
    control = _tree(item, "Phive/ControllerSetParam/DropItem.phive__ControllerSetParam.bgyml")
    _expect(control, "AliasEntityList", [{"AliasEntity": "Main", "ControllerName": "Body", "IsCharacterController": False, "RigidBodyName": "Body"}])
    _expect(control, "ConstraintControllerPath", "")
    body = _tree(item, "Phive/RigidBodyEntityParam/DropItem_0.phive__RigidBodyEntityParam.bgyml")
    _expect(body, "EnableLayerHitMask", "HitOnlyGroundAndObject")
    _expect(body, "LayerEntity", "SmallObject")
    _expect(body, "ShapeName", "Body_Body")

    def work(kind: str, stem: str) -> str:
        return f"Work/Phive/{kind}/{stem}.phive__{kind}.gyml"

    files[f"Phive/ControllerSetParam/{name}.phive__ControllerSetParam.bgyml"] = _dump({
        "AliasEntityList": control["AliasEntityList"],
        "CColEntityNamePathAry": control["CColEntityNamePathAry"],
        "ConstraintControllerPath": "",
        "ControllerEntityNamePathAry": control["ControllerEntityNamePathAry"],
        "LocatorContainer": work("LocatorContainerParam", name),
        "RigidBodyEntityNamePathAry": [{"FilePath": work("RigidBodyEntityParam", f"{name}_0"), "Name": "Body"}],
        "ShapeNamePathAry": [{"FilePath": work("ShapeParam", f"{name}_Body_0"), "Name": "Body_Body"}],
    })
    files[f"Phive/RigidBodyEntityParam/{name}_0.phive__RigidBodyEntityParam.bgyml"] = _dump(body)
    shape = _tree(item, "Phive/ShapeParam/DropItem_Body_0.phive__ShapeParam.bgyml")
    files[f"Phive/ShapeParam/{name}_Body_0.phive__ShapeParam.bgyml"] = _dump(sphere_shape(shape, BODY_RADIUS * scale))
    physics = _tree(item, "Component/Physics/Obj_Item_Base.engine__component__PhysicsParam.bgyml")
    _expect(physics, "ControllerSetPath", work("ControllerSetParam", "DropItem"))
    physics["ControllerSetPath"] = work("ControllerSetParam", name)
    files[f"Component/Physics/{name}.engine__component__PhysicsParam.bgyml"] = _dump(physics)

    files[f"Component/ModelInfo/{name}.engine__component__ModelInfo.bgyml"] = _dump({"FmdbName": name, "ModelProjectName": name})
    parameter_table = "GameParameter/GameParameterTable/Item_FairyBless_DefenseDouble.engine__actor__GameParameterTable.bgyml"
    _expect(_tree(table, parameter_table), "Category", "GameParameterTableDefault")
    files[f"GameParameter/GameParameterTable/{name}.engine__actor__GameParameterTable.bgyml"] = table[parameter_table]

    components = {
        "ActorLogicNodeRef": "Work/Logic/NodeDefinition/Node/ActorLogicSimpleActor.aidefn",
        "ActorPositionCalculatorRef": "?Component/ActorPositionCalculatorParam/Default.game__component__ActorPositionCalculatorParam.bgyml",
        "AffiliationRef": "?Component/AffiliationParam/ObjectCommon.game__component__AffiliationParam.bgyml",
        "GameParameterTableRef": f"?GameParameter/GameParameterTable/{name}.engine__actor__GameParameterTable.bgyml",
        "LODRef": "?Component/LODParam/ForbidPushJobDefault_Default.engine__component__LODParam.bgyml",
        "LiftableRef": f"?Component/LiftableParam/{name}.game__component__LiftableParam.bgyml",
        "ModelBindRef": "?Component/ModelBindParam/Default.engine__component__ModelBindParam.bgyml",
        "ModelInfoRef": f"?Component/ModelInfo/{name}.engine__component__ModelInfo.bgyml",
        "PhysicsRef": f"?Component/Physics/{name}.engine__component__PhysicsParam.bgyml",
        "PreActorSettingRef": NO_PACK_FILE[0],
        "ReactionRef": "?Component/Reaction/Default.engine__component__ReactionParam.bgyml",
        "SystemSetting": "?ActorSystem/ActorSystemSetting/GameActorDefault.engine__actor__ActorSystemSetting.bgyml",
        "WorldInfoRef": "?Component/WorldInfoParam/Default.game__component__WorldInfoParam.bgyml",
    }
    if animated:
        auto_play = "Component/ASInfo/FldObjCommon.engine__component__ASInfo.bgyml"
        settings = _tree(octopus, auto_play)
        _expect(settings, "AutoPlayASName", "Auto")
        _expect(settings, "AutoPlaySkeletals", [{"FileName": "FldObjAutoAnim"}])
        octopus_actor = _tree(octopus, "Actor/Animal_Octopus.engine__actor__ActorParam.bgyml")["Components"]
        if octopus_actor["ASRef"] != SEQUENCE or octopus_actor["ASInfoRef"] != "?" + auto_play:
            raise PackError("octopus animation setup changed")
        files[auto_play] = octopus[auto_play]
        files[f"Component/AnimationParam/{name}.engine__component__AnimationParam.bgyml"] = _dump(
            {"AnimationResources": [{"ModelProjectName": f"{name}_Animation"}]})
        components |= {
            "ASInfoRef": "?" + auto_play,
            "ASRef": SEQUENCE,
            "AnimationRef": f"?Component/AnimationParam/{name}.engine__component__AnimationParam.bgyml",
        }
    files[f"Actor/{name}.engine__actor__ActorParam.bgyml"] = _dump({"Components": dict(sorted(components.items()))})

    check_references(game, files)
    return files


def check_references(game: Game, files: dict[str, bytes]) -> None:
    """Every reference must lead to a file the game can find, the way a vanilla pack's references do."""

    def strings(node):
        if isinstance(node, dict):
            for value in node.values():
                yield from strings(value)
        elif isinstance(node, list):
            for value in node:
                yield from strings(value)
        elif isinstance(node, str):
            yield node

    def found(relative: str) -> bool:
        return relative in files or relative in game.resident or game.has(relative) or game.has(relative + ".zs")

    for name, data in files.items():
        for text in strings(byml7.load(data)[1]):
            if text.startswith("?"):
                if text not in NO_PACK_FILE and not found(text[1:]):
                    raise PackError(f"{name}: {text} resolves nowhere")
            elif text.startswith("Work/") and text.endswith(".gyml"):
                compiled = text[len("Work/"):-len(".gyml")] + ".bgyml"
                if not found(compiled):
                    raise PackError(f"{name}: {text} resolves nowhere")


def registration(variant: Variant, bounds: tuple[tuple[float, float, float], tuple[float, float, float]], animated: bool) -> dict:
    """Rows for the data builder's --rsdb-extra file (clone a vanilla row, then set these columns)."""
    name = variant.name
    low, high = bounds
    row = {
        "__RowId": name, "ActorName": name, "FmdbName": name, "ModelProjectName": name,
        "AnimationResources": [{"ModelProjectName": f"{name}_Animation"}] if animated else [],
        "ASInfoAutoPlaySkeletals": [{"FileName": "FldObjAutoAnim"}] if animated else [],
        "ASInfoAutoPlayMaterials": [{"FileName": "FldObjAutoAnim"}, {"FileName": "FldObjWaterAutoAnim"}] if animated else [],
        "ModelAabbMin": dict(zip("XYZ", low)), "ModelAabbMax": dict(zip("XYZ", high)),
        "InstanceHeapSize": 524288, "PreActorRenderInfoHeapSize": 65536,
    }
    return {
        "ActorInfo": {"clone": "Animal_Octopus", "set": row},
        "GameActorInfo": {"clone": "Animal_Octopus", "set": {"__RowId": name, "MainRigidBodyLayer": "SmallObject"}},
    }
