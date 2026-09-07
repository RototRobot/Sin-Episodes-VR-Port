#!/usr/bin/env python3
"""
set_muzzle_offset.py -- move a viewmodel's muzzle-flash attachment along its barrel.

WHY THIS EXISTS
---------------
SiN's assault-rifle muzzle flash sits short of the barrel end.  The three
viewmodels hang the 'muzzle' attachment off three structurally different bones:

    magnum      'muzzle'                 ROOT bone (parent -1)
    scattergun  'ValveBiped.scattergun'  weapon body, and a VERTEX bone
    rifle       'ValveBiped.Muzzle'      marker bone, 'Muzzle' <- 'gun' <- R_Hand

The rifle's is the only one that is BOTH attachment-only (skins no vertices)
AND buried in the animated arm chain.  Two consequences:

  1. Moving it is safe.  It skins nothing, so its transform reaches the muzzle
     flash and nothing else -- no mesh moves, no hitbox moves.
  2. Its muzzle point is measurably short.  Distance from the weapon's root
     bone to the muzzle point is 13.0 units on the rifle against 19.3 on the
     scattergun -- the longer gun with the shorter offset.  The rifle is the
     only weapon with two muzzles (it carries the underslung grenade launcher,
     'se1_ar_grenade01' / secondary_ammo ARNADE), and 13 units reads like the
     grenade tube rather than the rifle barrel.

WHICH WAY IS "DOWN THE BARREL"
------------------------------
Not assumed -- calibrated against the scattergun, whose attachment offset IS
the barrel by construction:

    scattergun  offset (0.797, -19.265, 0)  -> direction (0.041, -0.999, 0.0)
    rifle       gun -> Muzzle               -> direction (0.000, -0.993, 0.118)

Both are -Y in the weapon bone's local frame, agreeing to about a degree.  The
attachment's local translation is expressed in ITS BONE's frame, and
'ValveBiped.Muzzle' has identity rotation relative to 'gun', so the same
direction applies unchanged.

WHAT IS PATCHED
---------------
Twelve bytes: the translation column of the attachment's matrix3x4, at
  localattachmentindex + index*92 + 12 + {12, 28, 44}
Nothing else in the file is touched -- in particular NOT the checksum at 0x08,
which is what the .vvd and .vtx are matched against, so they stay valid.

The offset is applied on top of whatever the bone's ANIMATED transform is, so
this works regardless of which sequence is playing.  That is why the
attachment is patched rather than the bone's bind pose -- a bind-pose edit
would be overridden by any animation track driving that bone.

USAGE
-----
    py -3 tools/set_muzzle_offset.py                    report, change nothing
    py -3 tools/set_muzzle_offset.py --apply            push the rifle 6.0 units
    py -3 tools/set_muzzle_offset.py --apply --along 8  ... a different distance
    py -3 tools/set_muzzle_offset.py --apply --offset 0 -6 0.7    explicit, bone frame
    py -3 tools/set_muzzle_offset.py --revert           restore the originals

Originals are kept beside each model as '<name>.mdl.sinvr-original'.  An
existing backup is never overwritten, so re-applying still reverts cleanly.

Steam's "verify integrity of game files" reverts this, exactly like
hands.vmt and the ITEM_FLAG_NOAUTORELOAD scripts.  If the flash moves back to
where it started after a Steam operation, check that first.
"""

import argparse
import math
import os
import shutil
import struct
import sys

ATTACH_STRIDE = 92
BONE_STRIDE = 216
TRANS_FLOATS = (12, 28, 44)      # tx, ty, tz within the matrix3x4 at +12

DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\SiN Episodes Emergence"
VIEWMODELS = ("v_assault_rifle", "v_magnum", "v_scattergun")


def i32(b, o):
    return struct.unpack_from("<i", b, o)[0]


def f32(b, o):
    return struct.unpack_from("<f", b, o)[0]


def cstr(b, o):
    if not 0 < o < len(b):
        return "<bad>"
    return b[o:b.index(b"\0", o)].decode("ascii", "replace")


class Model:
    """Just enough of studiohdr_t v44 to find and move a muzzle attachment."""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.b = bytearray(f.read())
        b = self.b
        self.ident = bytes(b[0:4]).decode("ascii", "replace")
        self.version = i32(b, 0x04)
        self.numbones, self.boneindex = i32(b, 0x9C), i32(b, 0xA0)
        self.numatt, self.attindex = i32(b, 0xF0), i32(b, 0xF4)

    def valid(self):
        """Refuse to write to anything that is not the file we think it is."""
        if self.ident != "IDST":
            return "not a studiomdl file"
        if self.version != 44:
            return "version %d, expected 44" % self.version
        if not 0 < self.numbones < 256:
            return "numbones %d implausible" % self.numbones
        if not 0 < self.numatt < 64:
            return "numlocalattachments %d implausible" % self.numatt
        end = self.attindex + self.numatt * ATTACH_STRIDE
        if not 0 < self.attindex < end <= len(self.b):
            return "attachment table runs outside the file"
        return None

    def bone_name(self, i):
        o = self.boneindex + i * BONE_STRIDE
        return cstr(self.b, o + i32(self.b, o))

    def bone_parent(self, i):
        return i32(self.b, self.boneindex + i * BONE_STRIDE + 4)

    def bone_local_pos(self, i):
        o = self.boneindex + i * BONE_STRIDE + 32
        return (f32(self.b, o), f32(self.b, o + 4), f32(self.b, o + 8))

    def attachments(self):
        for a in range(self.numatt):
            o = self.attindex + a * ATTACH_STRIDE
            yield (a, cstr(self.b, o + i32(self.b, o)), i32(self.b, o + 8), o)

    def find_muzzle(self):
        for a, name, bone, o in self.attachments():
            if name.lower() == "muzzle":
                return a, bone, o
        return None, None, None

    def get_offset(self, o):
        return tuple(f32(self.b, o + 12 + t) for t in TRANS_FLOATS)

    def set_offset(self, o, xyz):
        for t, v in zip(TRANS_FLOATS, xyz):
            struct.pack_into("<f", self.b, o + 12 + t, float(v))

    def barrel_direction(self):
        """
        Unit vector down the barrel, in the muzzle BONE's local frame.

        Taken from the offset between the weapon's root bone and the muzzle
        marker -- 'further along the way it already sits' -- which needs no
        assumption about which axis is forward.
        """
        _, bone, _ = self.find_muzzle()
        if bone is None:
            return None, "no muzzle attachment"
        parent = self.bone_parent(bone)
        if parent < 0:
            return None, ("'%s' is a ROOT bone -- no parent to measure from"
                          % self.bone_name(bone))
        p = self.bone_local_pos(bone)
        d = math.dist(p, (0.0, 0.0, 0.0))
        if d < 1.0:
            return None, ("'%s' sits %.2f units from its parent -- too close "
                          "to give a direction" % (self.bone_name(bone), d))
        return tuple(c / d for c in p), d


def model_path(game, name):
    return os.path.join(game, "SE1", "models", "Weapons", name, name + ".mdl")


def backup_path(p):
    return p + ".sinvr-original"


def report(game):
    print("Muzzle attachments, and how far each sits from its weapon's root bone.")
    print("The distance is the number that matters: the rifle is the longer gun")
    print("with the shorter offset.")
    print("")
    for name in VIEWMODELS:
        p = model_path(game, name)
        if not os.path.exists(p):
            print("  %-18s NOT FOUND at %s" % (name, p))
            continue
        m = Model(p)
        bad = m.valid()
        if bad:
            print("  %-18s UNREADABLE -- %s" % (name, bad))
            continue
        a, bone, o = m.find_muzzle()
        if a is None:
            print("  %-18s has no 'muzzle' attachment" % name)
            continue
        off = m.get_offset(o)
        bn, par = m.bone_name(bone), m.bone_parent(bone)
        kind = "ROOT bone" if par < 0 else "child of '%s'" % m.bone_name(par)
        mark = "  <-- patched" if os.path.exists(backup_path(p)) else ""
        print("  %-18s bone '%s' (%s)%s" % (name, bn, kind, mark))
        print("  %-18s attachment offset  = (%7.3f %8.3f %7.3f)  |%.2f|"
              % ("", off[0], off[1], off[2], math.dist(off, (0, 0, 0))))
        # Deliberately NOT summed with the bone's own offset from its parent:
        # those two distances mean different things per weapon.  The rifle's
        # marker bone hangs off the weapon root 'gun', so its 13.01 IS the
        # muzzle distance; the scattergun's bone IS the weapon root and its
        # parent is the hand, so adding that would tack on 6 units of wrist.
        if par >= 0:
            d = math.dist(m.bone_local_pos(bone), (0, 0, 0))
            print("  %-18s bone sits %.2f units from '%s'" % ("", d, m.bone_name(par)))
    print("")


def apply_patch(game, target, along, explicit, dry):
    p = model_path(game, target)
    if not os.path.exists(p):
        print("ERROR: %s not found." % p)
        return 1

    m = Model(p)
    bad = m.valid()
    if bad:
        print("ERROR: refusing to write to %s -- %s" % (target, bad))
        return 1

    a, bone, o = m.find_muzzle()
    if a is None:
        print("ERROR: %s has no attachment named 'muzzle'." % target)
        return 1

    before = m.get_offset(o)

    if explicit is not None:
        new = tuple(explicit)
        how = "explicit, in the muzzle bone's local frame"
    else:
        d, info = m.barrel_direction()
        if d is None:
            print("ERROR: cannot derive a barrel direction for %s -- %s" % (target, info))
            print("       Pass --offset X Y Z to set it directly.")
            return 1
        new = tuple(along * c for c in d)
        how = ("%.2f units along (%.3f %.3f %.3f), the direction the marker "
               "already sits from '%s'"
               % (along, d[0], d[1], d[2], m.bone_name(m.bone_parent(bone))))

    print("model      : %s" % p)
    print("attachment : [%d] 'muzzle' on bone %d '%s'" % (a, bone, m.bone_name(bone)))
    print("offset     : (%.3f %.3f %.3f)  ->  (%.3f %.3f %.3f)"
          % (before[0], before[1], before[2], new[0], new[1], new[2]))
    print("             %s" % how)

    if dry:
        print("")
        print("(dry run -- nothing written; pass --apply to write)")
        return 0

    bak = backup_path(p)
    if not os.path.exists(bak):
        shutil.copy2(p, bak)
        print("backup     : %s" % bak)
    else:
        print("backup     : %s (already exists, kept)" % bak)

    m.set_offset(o, new)
    with open(p, "wb") as f:
        f.write(m.b)
    print("")
    print("Written. Restart the game -- model data is cached at map load.")
    return 0


def revert(game):
    n = 0
    for name in VIEWMODELS:
        p = model_path(game, name)
        bak = backup_path(p)
        if os.path.exists(bak):
            shutil.copy2(bak, p)
            os.remove(bak)
            print("  reverted %s" % name)
            n += 1
    print("")
    print("%d model(s) reverted." % n if n else "Nothing to revert.")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Move a viewmodel's muzzle-flash attachment along its barrel.")
    ap.add_argument("--game", default=DEFAULT_GAME, help="game folder")
    ap.add_argument("--model", default="v_assault_rifle", choices=VIEWMODELS,
                    help="which viewmodel to patch (default: v_assault_rifle)")
    ap.add_argument("--along", type=float, default=6.0, metavar="N",
                    help="push the muzzle N units further down the barrel "
                         "(default: 6.0, taking the rifle from 13.0 to 19.0 "
                         "against the scattergun's 19.3)")
    ap.add_argument("--offset", type=float, nargs=3, metavar=("X", "Y", "Z"),
                    help="set the attachment offset explicitly, in the muzzle bone's frame")
    ap.add_argument("--apply", action="store_true", help="write the change")
    ap.add_argument("--revert", action="store_true", help="restore every .sinvr-original")
    args = ap.parse_args()

    if not os.path.isdir(args.game):
        print("ERROR: game folder not found: %s" % args.game)
        return 1

    if args.revert:
        return revert(args.game)

    if not args.apply and args.offset is None:
        report(args.game)
        print("Nothing changed. Pass --apply to patch, --revert to undo.")
        return 0

    return apply_patch(args.game, args.model, args.along, args.offset, dry=not args.apply)


if __name__ == "__main__":
    sys.exit(main())
