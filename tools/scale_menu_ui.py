#!/usr/bin/env python3
"""Scale SiN's menu dialogs up for high VR render resolutions.

WHY THIS EXISTS
---------------
SiN's GameUI dialogs -- Load, Save, Options, New Game -- are laid out in
ABSOLUTE PIXELS in `SE1\\resource\\*.res`:

    LoadGameDialog      480 x 460
    DialogOptionsIngame 824 x 736

Those numbers were authored for roughly a 1024x768 screen. Once
`vr_allow_oversize_window` lets the render reach the headset's native size,
the same dialog is a fifth of the screen wide and unreadable in the headset.

They cannot be fixed in code: `SetProportional` exists only in the client-side
panels the SDK ships source for, and these dialogs live in `GameUI.dll`, a
closed binary.

WHAT WAS MEASURED, AND WHAT IT CHANGES
--------------------------------------
Three runs at 1600x900, reading the dialog's pixel edges off a screenshot:

    .res frame            measured left edge / width
    xpos 272 (shipped)    472 / 480
    xpos c-240            472 / 480      <- IDENTICAL, so xpos is ignored
    wide 960 tall 920     232 / 960      <- re-centred on its own

Two facts came out of that, and both shape this script:

  1. **`xpos`/`ypos` on the dialog FRAME do nothing.** The dialog positions
     itself in code. Centre-relative coordinates (`c-240`) were tested and
     changed nothing -- and note the control is what proved it, because
     `ypos c-230` happened to predict the same 220 that self-centring gives.
  2. **`wide`/`tall` ARE read, and the dialog re-centres around the new size.**
     So position needs no fixing and adapts at any resolution for free.

Child controls are a different case: they ARE laid out from the file, relative
to their parent, so all four of their values must scale. Scaling the frame
alone leaves a large empty panel with a small list in the corner -- which is
exactly what the measurement above looked like on screen.

FONTS -- AND THE TWO SCHEMES ARE NOT THE SAME
---------------------------------------------
Source can band a font by vertical resolution (`"yres" "1200 10000"`), and the
first assumption here was that fonts therefore only needed the band covering
your height adjusted. Counting them says otherwise:

    SourceScheme.res   24 sizes,  5 bands   <- the MENU scheme
    ClientScheme.res   57 sizes, 30 bands   <- the in-game HUD scheme

So nineteen of the menu scheme's twenty-four sizes are **unbanded** and do not
scale with resolution at all. A band-only pass changed exactly ONE value in it.
That is why the menus stay small however high you render, and why this script
scales every size in `SourceScheme.res` rather than only the banded ones.

`ClientScheme.res` is the opposite -- properly banded, and it drives the HUD,
which is a different complaint from "the Load dialog is tiny". It is left alone
unless `--hud-fonts` is passed, and even then only the band covering your
height is touched.

NOT APPLIED AUTOMATICALLY
-------------------------
This edits game content, so it follows the same rule as `set_noautoreload.ps1`:
originals are kept as `.sinvr-original`, `--revert` puts them back, and running
it with no arguments only reports. Steam's "verify integrity" reverts it too.

USAGE
-----
    py -3 tools/scale_menu_ui.py                          report only
    py -3 tools/scale_menu_ui.py --for-height 2392        show the plan
    py -3 tools/scale_menu_ui.py --for-height 2392 --apply
    py -3 tools/scale_menu_ui.py --scale 2.5 --apply
    py -3 tools/scale_menu_ui.py --revert
"""

import argparse
import os
import re
import shutil
import sys

# The MENU dialogs, by name. An explicit allowlist rather than a pattern:
# `SE1\resource` also holds HUD layout, schemes and developer panels, and the
# rule in this project is that we do not change files we were not asked to.
# Anything not named here is left alone.
DIALOGS = [
    "LoadGameDialog.res",
    "SaveGameDialog.res",
    "SaveGamePanel.res",
    "SaveBeforeQuitDialog.res",
    "NewGameDialog.res",
    "NewGameChapterPanel.res",
    "SkillSelectionDialog.res",
    "ContentControlDialog.res",
]

# ---- THE OPTIONS DIALOG CANNOT BE SCALED, AND SCALING ITS PAGES HURTS ------
#
# Measured: the Options frame renders at 511x406 whatever the .res says. It was
# scaled to 2053x1834 and came out 511x406 anyway -- neither the scaled value
# nor the shipped 824x736. Then the reason turned up in the binary: GameUI.dll
# names every OptionsSub*.res page but there is NO .res for the Options FRAME
# at all, so VGUI has no file to read and the size comes from code.
#
# That makes scaling the PAGES worse than leaving them alone. Their controls
# are laid out relative to a parent that never grows, so at 2.49x they are
# positioned outside a 511x406 box and clipped -- reported from the headset as
# "too many inputs crammed into the small window".
#
# So these are deliberately NOT in the list above. The frame is the constraint
# and no file can move it.
OPTIONS_FAMILY = [
    "DialogOptionsIngame.res",
    "OptionsSubAudio.res",
    "OptionsSubDifficulty.res",
    "OptionsSubKeyboard.res",
    "OptionsSubKeyboardAdvancedDlg.res",
    "OptionsSubMouse.res",
    "OptionsSubVideo.res",
    "OptionsSubVideoAdvancedDlg.res",
    "OptionsSubVideoGammaDlg.res",
    "OptionsSubVoice.res",
]

# Font sizes live here. The two schemes are NOT equivalent and are treated
# differently, which was measured rather than assumed:
#
#   SourceScheme.res   the MENU scheme.  24 sizes, only 5 yres bands -- so
#                      nineteen of its fonts do not scale with resolution at
#                      all. This is why the menus stay small, and why every
#                      size in it is scaled, banded or not.
#   ClientScheme.res   the in-game HUD scheme. 57 sizes across 30 bands, i.e.
#                      already properly banded. Touching it changes the HUD,
#                      which is a different complaint from "the Load dialog is
#                      tiny", so it is OFF unless --hud-fonts is given.
MENU_SCHEME = "SourceScheme.res"
HUD_SCHEME = "ClientScheme.res"
SCHEMES = [MENU_SCHEME, HUD_SCHEME]

# Only these keys are geometry. Everything else in a .res block -- tabPosition,
# autoResize, pinCorner, visible, textAlignment -- is not a pixel count and
# multiplying it would corrupt the layout in ways that are hard to spot.
GEOM_KEYS = ("xpos", "ypos", "wide", "tall")

BACKUP_SUFFIX = ".sinvr-original"

# The height the shipped dialogs look correct at. 460 of 960 is about half the
# screen, which is what they were drawn for; it is the number --for-height
# divides by, and it is a starting point rather than a measurement.
BASELINE_HEIGHT = 960.0

KV = re.compile(r'^(\s*)"([A-Za-z_0-9]+)"(\s+)"(-?\d+)"(\s*(?://.*)?)$')
YRES = re.compile(r'^\s*"yres"\s+"(\d+)\s+(\d+)"')


def scale_int(text, factor):
    return str(int(round(int(text) * factor)))


def scale_dialog(text, factor):
    """Scale every geometry value in a dialog .res.

    The frame's own xpos/ypos are scaled too. They are ignored by the engine --
    measured, see the header -- so this is harmless, and special-casing the
    first block would be a rule to get wrong later for no benefit.
    """
    out, changed = [], 0
    for line in text.split("\n"):
        m = KV.match(line.rstrip("\r"))
        if m and m.group(2).lower() in GEOM_KEYS:
            indent, key, gap, value, tail = m.groups()
            out.append('%s"%s"%s"%s"%s' % (indent, key, gap, scale_int(value, factor), tail))
            changed += 1
        else:
            out.append(line)
    return "\n".join(out), changed


def scale_scheme(text, factor, target_height, include_unbanded):
    """Scale font heights.

    A font block is a run of key/value lines optionally ending in a `yres` line
    saying which resolutions it serves, so a `tall` has to be held until the
    band is known.

    BANDED fonts are scaled only when the band covers `target_height` -- the
    other bands describe resolutions this machine is not running, and changing
    them would be editing someone else's case.

    UNBANDED fonts apply at every resolution. `include_unbanded` decides
    whether to scale them, and it is the whole reason the menus can be fixed
    at all: SourceScheme leaves nineteen of its twenty-four sizes unbanded, so
    a band-only pass changes almost nothing there.
    """
    lines = text.split("\n")
    out = list(lines)
    changed = 0
    pending = []  # indices of "tall" lines not yet attributed to a band

    for i, line in enumerate(lines):
        stripped = line.rstrip("\r")
        m = KV.match(stripped)
        if m and m.group(2).lower() == "tall":
            pending.append(i)
            continue

        y = YRES.match(stripped)
        if y:
            lo, hi = int(y.group(1)), int(y.group(2))
            if lo <= target_height <= hi:
                for idx in pending:
                    mm = KV.match(lines[idx].rstrip("\r"))
                    indent, key, gap, value, tail = mm.groups()
                    out[idx] = '%s"%s"%s"%s"%s' % (
                        indent, key, gap, scale_int(value, factor), tail)
                    changed += 1
            pending = []
            continue

        # A closing brace ends a font entry. Anything still pending had no
        # yres line, so it applies at every resolution.
        if stripped.strip() == "}":
            if include_unbanded:
                for idx in pending:
                    mm = KV.match(lines[idx].rstrip("\r"))
                    indent, key, gap, value, tail = mm.groups()
                    out[idx] = '%s"%s"%s"%s"%s' % (
                        indent, key, gap, scale_int(value, factor), tail)
                    changed += 1
            pending = []

    return "\n".join(out), changed


def resource_dir(game):
    return os.path.join(game, "SE1", "resource")


def find_game(explicit):
    if explicit:
        return explicit
    guess = r"C:\Program Files (x86)\Steam\steamapps\common\SiN Episodes Emergence"
    return guess


def do_revert(res):
    restored = 0
    # OPTIONS_FAMILY is included so a revert undoes what an EARLIER build of
    # this script scaled, before those files were excluded.
    for name in DIALOGS + SCHEMES + OPTIONS_FAMILY:
        path = os.path.join(res, name)
        backup = path + BACKUP_SUFFIX
        if os.path.isfile(backup):
            shutil.copyfile(backup, path)
            os.remove(backup)
            print("  restored %s" % name)
            restored += 1
    if restored == 0:
        print("  nothing to revert -- no %s files found" % BACKUP_SUFFIX)
    return restored


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", help="game folder (default: the usual Steam path)")
    ap.add_argument("--scale", type=float, help="multiply geometry by this")
    ap.add_argument("--for-height", type=int,
                    help="derive the scale from a per-eye render height "
                         "(the -h the launcher reports)")
    ap.add_argument("--font-scale", type=float,
                    help="scale fonts by this instead of the geometry scale. "
                         "Lower it when the Options dialog overflows: its frame "
                         "is fixed at 511x406 by GameUI.dll and cannot grow, so "
                         "big fonts overflow it however the pages are laid out.")
    ap.add_argument("--hud-fonts", action="store_true",
                    help="also scale the in-game HUD's fonts (ClientScheme.res). "
                         "Off by default: the HUD is a separate complaint from "
                         "the menus, and the mod already anchors it.")
    ap.add_argument("--apply", action="store_true", help="write the changes")
    ap.add_argument("--revert", action="store_true", help="restore the originals")
    args = ap.parse_args()

    game = find_game(args.game)
    res = resource_dir(game)
    if not os.path.isdir(res):
        print("ERROR: no resource folder at %s" % res)
        print("Pass --game <folder> if the game is installed elsewhere.")
        return 1

    print("game     : %s" % game)
    print("resources: %s" % res)
    print()

    if args.revert:
        print("REVERTING to the shipped files:")
        do_revert(res)
        return 0

    already = [n for n in DIALOGS + SCHEMES + OPTIONS_FAMILY
               if os.path.isfile(os.path.join(res, n + BACKUP_SUFFIX))]
    if already:
        print("ALREADY SCALED -- %d file(s) have a %s beside them." % (len(already), BACKUP_SUFFIX))
        print("Scaling again would compound on top of the last run, so revert first:")
        print("    py -3 tools/scale_menu_ui.py --revert")
        return 1

    if args.for_height:
        factor = args.for_height / BASELINE_HEIGHT
    elif args.scale:
        factor = args.scale
    else:
        print("No --scale or --for-height given, so this is a report only.")
        print()
        print("The shipped dialog sizes, and what they occupy at a few heights:")
        print("  %-26s %9s %9s %9s" % ("dialog", "size", "at 1080", "at 2392"))
        for name in ("LoadGameDialog.res", "DialogOptionsIngame.res", "NewGameDialog.res"):
            p = os.path.join(res, name)
            if not os.path.isfile(p):
                continue
            t = open(p, encoding="utf-8", errors="replace").read()
            w = re.search(r'"wide"\s+"(\d+)"', t)
            h = re.search(r'"tall"\s+"(\d+)"', t)
            if w and h:
                hh = int(h.group(1))
                print("  %-26s %9s %8.0f%% %8.0f%%" % (
                    name, "%sx%s" % (w.group(1), h.group(1)),
                    100.0 * hh / 1080, 100.0 * hh / 2392))
        print()
        print("Pick a scale with --for-height <your per-eye render height>, e.g.")
        print("    py -3 tools/scale_menu_ui.py --for-height 2392")
        return 0

    if factor <= 0 or factor > 8:
        print("ERROR: scale %.3f is not sensible (expected roughly 1.0 to 4.0)" % factor)
        return 1

    target_h = args.for_height or int(BASELINE_HEIGHT * factor)
    fontFactor = args.font_scale if args.font_scale else factor
    if fontFactor <= 0 or fontFactor > 8:
        print("ERROR: font scale %.3f is not sensible" % fontFactor)
        return 1
    print("scale    : x%.3f%s" % (
        factor,
        "  (from height %d / baseline %d)" % (target_h, BASELINE_HEIGHT)
        if args.for_height else ""))
    print("fonts    : x%.3f%s" % (
        fontFactor, "" if abs(fontFactor - factor) < 0.001 else "  (--font-scale)"))
    print("font band: the one covering yres %d" % target_h)
    print("note     : the Options dialog is EXCLUDED -- its frame is fixed at")
    print("           511x406 in GameUI.dll and no .res can resize it.")
    print("mode     : %s" % ("APPLY" if args.apply else "dry run -- nothing written"))
    print()

    total = 0
    for name in DIALOGS:
        path = os.path.join(res, name)
        if not os.path.isfile(path):
            print("  %-36s (absent, skipped)" % name)
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        new, n = scale_dialog(text, factor)
        total += n
        print("  %-36s %3d value(s)" % (name, n))
        if args.apply and n:
            shutil.copyfile(path, path + BACKUP_SUFFIX)
            open(path, "w", encoding="utf-8", errors="replace", newline="").write(new)

    print()
    for name in SCHEMES:
        if name == HUD_SCHEME and not args.hud_fonts:
            print("  %-36s skipped (pass --hud-fonts to scale the HUD too)" % name)
            continue
        path = os.path.join(res, name)
        if not os.path.isfile(path):
            print("  %-36s (absent, skipped)" % name)
            continue
        # Menus: every size, because most of them are unbanded and would
        # otherwise be left behind. HUD: only the band this machine runs in.
        unbanded = (name == MENU_SCHEME)
        text = open(path, encoding="utf-8", errors="replace").read()
        new, n = scale_scheme(text, fontFactor, target_h, unbanded)
        total += n
        print("  %-36s %3d font size(s) %s" % (
            name, n,
            "(all sizes -- menu scheme is mostly unbanded)" if unbanded
            else "in the yres band covering %d" % target_h))
        if args.apply and n:
            shutil.copyfile(path, path + BACKUP_SUFFIX)
            open(path, "w", encoding="utf-8", errors="replace", newline="").write(new)

    print()
    print("%d value(s) %s." % (total, "changed" if args.apply else "would change"))
    if args.apply:
        print("Originals kept as *%s. Revert with --revert." % BACKUP_SUFFIX)
        print("Takes effect on the next launch -- .res files are read at startup.")
    else:
        print("Add --apply to write it.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
