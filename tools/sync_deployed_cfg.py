# Bring a deployed sinvr.cfg up to date with newly added config keys, WITHOUT
# letting the mod regenerate it.
#
# ---- THE PROBLEM THIS EXISTS FOR --------------------------------------------
#
# The mod counts how many KnownKeys are missing from sinvr.cfg. If any are, it
# backs the file up to .old and rewrites it from the built-in defaults --
# discarding every hand-tuned value in it. That is correct behaviour (a config
# missing keys is a config from an older build), but it means ADDING A SINGLE KEY
# to the source silently destroys the tuning on the test rig the next time the
# game starts.
#
# HANDOVER.md 7b says to hand-add new keys to the deployed cfg for exactly this
# reason. Doing that by hand is fine for one key and a transcription error
# waiting to happen for thirty -- the body zones added thirty-one at once.
#
# So: parse the keys and their default values out of config.h's generated text,
# compare against the deployed file, and append whatever is missing, comments and
# all. Values already in the file are never touched.
#
# Obsolete keys -- present in the cfg but no longer in KnownKeys -- are REPORTED,
# not deleted. They are harmless (the mod only counts missing ones) and deleting
# something a human put there is not this script's call to make.
#
# Usage:
#   py -3 tools\sync_deployed_cfg.py [--cfg PATH] [--apply]
#
# Without --apply it only reports.

import io, re, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CONFIG_H = os.path.join(ROOT, "src", "common", "config.h")
DEFAULT_CFG = (r"C:\Program Files (x86)\Steam\steamapps\common"
               r"\SiN Episodes Emergence\sinvr.cfg")

cfg_path = DEFAULT_CFG
apply = False
args = sys.argv[1:]
i = 0
while i < len(args):
    if args[i] == "--cfg" and i + 1 < len(args):
        cfg_path = args[i + 1]
        i += 2
    elif args[i] == "--apply":
        apply = True
        i += 1
    else:
        sys.exit("unknown argument: %s" % args[i])

src = io.open(CONFIG_H, encoding="utf-8", newline="").read()

# --- KnownKeys ---------------------------------------------------------------
m = re.search(r"KnownKeys\s*\(\s*size_t&\s*count\s*\)\s*\{(.*?)\n\t\}", src, re.S)
assert m, "could not find KnownKeys in config.h"
known = [k for k in re.findall(r'"([a-z0-9_]+)"', m.group(1))]

# --- the generated default text, as real lines -------------------------------
# Each source line is  \t\t\t"...\n"  -- recover the text between the quotes and
# turn the escaped \n back into a line break.
body = []
for lit in re.findall(r'^\t+"((?:[^"\\]|\\.)*)"\s*$', src, re.M):
    body.append(lit.replace('\\n', '\n').replace('\\"', '"').replace('\\\\', '\\'))
default_text = "".join(body)
default_lines = default_text.split("\n")

# Map key -> (value, the comment block immediately above it).
entries = {}
pending = []
for line in default_lines:
    stripped = line.strip()
    if stripped.startswith("#"):
        pending.append(line)
        continue
    km = re.match(r'^([a-z0-9_]+)\s*=\s*(.*)$', stripped)
    if km:
        entries[km.group(1)] = (km.group(2), pending)
        pending = []
    elif not stripped:
        pending.append(line)
    else:
        pending = []

missing_from_default = [k for k in known if k not in entries]
if missing_from_default:
    sys.exit("config.h is inconsistent -- these KnownKeys have no default line, "
             "so the mod would regenerate forever: %s"
             % ", ".join(missing_from_default))

# --- the deployed file -------------------------------------------------------
if not os.path.exists(cfg_path):
    sys.exit("not found: %s\nThe mod writes it on first run." % cfg_path)

cfg = io.open(cfg_path, encoding="utf-8", newline="").read()
present = set(re.findall(r'^\s*([a-z0-9_]+)\s*=', cfg, re.M))

missing = [k for k in known if k not in present]

# Keys that are absent from KnownKeys ON PURPOSE, not because they were removed.
#
# The per-weapon viewmodel offsets are absent-by-default -- absent means "use the
# global" -- so listing them in KnownKeys would make every launch see them
# missing and regenerate the file forever (HANDOVER 7b, trap 3). They are
# therefore TUNED VALUES that look exactly like stale ones, and telling somebody
# they are obsolete is how a tuning session gets deleted.
OPTIONAL_PATTERNS = [
    re.compile(r'^viewmodel_(offset|angle)_[a-z]+_v_[a-z0-9_]+$'),
    # Bore zeros: per weapon AND per hand, e.g.
    #     aim_bore_right_v_magnum_left
    # Same argument as above and then some -- these are minutes of squinting
    # down iron sights, they cannot live in KnownKeys, and the only thing
    # standing between them and somebody's spring clean is this line.
    re.compile(r"^aim_bore_(right|up|fwd|yaw|pitch)_v_[a-z0-9_]+_(left|right)$"),
    # Holster zone geometry. Absent by default ON PURPOSE: the mod carries a
    # profile for each hand and left_handed picks between them, so shipping the
    # keys meant a cfg value overriding the built-in profile -- which made every
    # left-handed fresh install reach for right-handed zones. Present here only
    # when a player has tuned their own, at which point they are exactly the
    # kind of value this whole file exists to protect.
    re.compile(r"^zone_holster_(hip|left|right)_(forward|lateral|up|size_forward|size_lateral|size_up)$"),
]

all_unknown = sorted(present - set(known))
optional = [k for k in all_unknown if any(p.match(k) for p in OPTIONAL_PATTERNS)]
obsolete = [k for k in all_unknown if k not in optional]

print("deployed cfg : %s" % cfg_path)
print("KnownKeys    : %d" % len(known))
print("present      : %d" % len(present & set(known)))
print("MISSING      : %d   <- the mod regenerates the file if this is not 0"
      % len(missing))
print("obsolete     : %d   (harmless, reported only)" % len(obsolete))
print("optional     : %d   (deliberately not KnownKeys -- DO NOT DELETE)"
      % len(optional))
print()

if obsolete:
    print("--- in the cfg but no longer a KnownKey: a build removed these ---")
    for k in obsolete:
        print("    %s" % k)
    print("    (harmless -- the mod only counts MISSING keys -- so they are left"
          " alone)")
    print()

if optional:
    print("--- optional by design: absent means 'use the global' ---")
    for k in optional:
        print("    %s" % k)
    print("    These are TUNED VALUES, not leftovers. They are kept out of")
    print("    KnownKeys on purpose; listing them there would make every launch")
    print("    see a missing key and regenerate the file. Do not remove them.")
    print()

if not missing:
    print("Nothing to add. The mod will leave this file alone.")
    sys.exit(0)

print("--- missing, would be appended with their defaults ---")
for k in missing:
    print("    %-34s = %s" % (k, entries[k][0]))
print()

if not apply:
    print("Report only. Re-run with --apply to append them.")
    sys.exit(0)

backup = cfg_path + ".presync"
if not os.path.exists(backup):
    io.open(backup, "w", encoding="utf-8", newline="").write(cfg)

out = [cfg]
if not cfg.endswith("\n"):
    out.append("\n")
out.append("\n# ==== added by tools/sync_deployed_cfg.py ====\n"
           "# Keys that appeared in a newer build. Values are the built-in\n"
           "# defaults; everything already in this file was left untouched.\n")
for k in missing:
    value, comments = entries[k]
    text = "\n".join(comments).strip("\n")
    if text:
        out.append("\n" + text + "\n")
    else:
        out.append("\n")
    out.append("%s = %s\n" % (k, value))

io.open(cfg_path, "w", encoding="utf-8", newline="").write("".join(out))
print("Appended %d key(s). Original kept as %s" % (len(missing), os.path.basename(backup)))
