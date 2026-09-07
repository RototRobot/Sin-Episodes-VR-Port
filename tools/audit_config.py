# Cross-check the three places a config key has to exist.
#
# HANDOVER.md section 7b calls the config file "a trap in three different ways",
# and all three are silent:
#
#   1. A key in KnownKeys that the generated default does not contain makes
#      EVERY launch see a missing key, so the mod backs the file up and
#      regenerates it -- forever, discarding hand-tuned values each time.
#   2. A key in the default text but not in KnownKeys is never counted as
#      present.
#   3. A key in KnownKeys that nothing reads is dead config: it looks
#      supported, and changing it does nothing.
#
# Optional keys -- the per-weapon viewmodel offsets -- are deliberately
# absent-by-default and must NOT be in KnownKeys, so "read but not known" is
# reported separately rather than as a failure.
#
# ---- COMPOSED KEYS -----------------------------------------------------------
#
# The body zones build their key names at runtime from a prefix and a suffix
# (`_snprintf_s( key, "%s_forward", pfx )`), so a literal-string grep cannot see
# them and reported all thirty as dead. Rather than drop the check or hardcode a
# list that would go stale the moment a zone was added, the prefixes are parsed
# out of zone_set.h and combined with the suffixes below -- so a new zone is
# still audited, and a zone whose keys stop being generated still fails.
#
# Usage:  py -3 tools\audit_config.py
# Exits non-zero if anything is wrong, so it can gate a build.

import io, re, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CONFIG = os.path.join(ROOT, "src", "common", "config.h")
ZONE_SET = os.path.join(ROOT, "src", "sinvr", "input", "zone_set.h")

ZONE_SUFFIXES = ["forward", "lateral", "up",
                 "size_forward", "size_lateral", "size_up"]

src = io.open(CONFIG, encoding="utf-8", newline="").read()

# --- 1. KnownKeys ------------------------------------------------------------
m = re.search(r"KnownKeys\s*\(\s*size_t&\s*count\s*\)\s*\{(.*?)\n\t\}", src, re.S)
assert m, "could not find KnownKeys in config.h"
known = set(re.findall(r'"([a-z0-9_]+)"', m.group(1)))

# --- 2. the generated default text -------------------------------------------
defaults = set(re.findall(r'"\s*([a-z0-9_]+)\s*=\s*[^"\\]*\\n"', src))

# --- 3. reads across the whole source ----------------------------------------
reads = set()
code_src = ""
for root, _dirs, files in os.walk(os.path.join(ROOT, "src")):
    for fn in files:
        if not fn.endswith((".h", ".cpp")):
            continue
        t = io.open(os.path.join(root, fn), encoding="utf-8", errors="replace").read()
        code_src += t
        reads |= set(re.findall(r'Get(?:Bool|Int|Float|String)\(\s*"([a-z0-9_]+)"', t))
        reads |= set(re.findall(r'Get(?:Bool|Int|Float|String)\(\s*\n\s*"([a-z0-9_]+)"', t))

        # The LAUNCHER reads the cfg with its own parser, because it runs
        # before sinvr.dll exists and cannot use Config at all. Those are real
        # reads and were invisible here.
        #
        # It went unnoticed only because every launcher key so far ALSO happens
        # to be read by the mod -- so the check was passing for the wrong
        # reason, and the first launcher-only key (dxvk_sample_rate_shading)
        # was reported as dead config.
        # The first argument is a path EXPRESSION, not necessarily a bare
        # identifier -- `gameDir / L"sinvr.cfg"` is as valid a caller as `cfg`.
        # Matching only `\w+` made the check depend on how the caller happened
        # to be written, and reported a live key as dead the first time one was
        # written the other way.
        reads |= set(re.findall(r'ReadCfgFloat\(\s*[^,]+,\s*"([a-z0-9_]+)"', t))

# Composed zone keys, from the prefixes the code actually declares.
composed = set()
if os.path.exists(ZONE_SET):
    zs = io.open(ZONE_SET, encoding="utf-8", errors="replace").read()
    prefixes = re.findall(r'"\s*(zone_[a-z0-9_]+)\s*"', zs)
    for pfx in set(prefixes):
        for suf in ZONE_SUFFIXES:
            composed.add("%s_%s" % (pfx, suf))
reads |= composed

print("KnownKeys entries      : %d" % len(known))
print("keys in default text   : %d" % len(defaults))
print("keys read in code      : %d  (%d of them composed at runtime)"
      % (len(reads), len(composed)))
print()

bad = False


def report(title, items, fatal=True):
    global bad
    items = sorted(items)
    print("--- %s ---" % title)
    if not items:
        print("    (none)")
    else:
        for k in items:
            print("    %s" % k)
        if fatal:
            bad = True
    print()


report("in KnownKeys but NEVER READ (dead config)", known - reads)
report("in KnownKeys but ABSENT from the generated default "
       "(regenerates the cfg forever)", known - defaults)
report("in the generated default but NOT in KnownKeys "
       "(never counted as present)", defaults - known)
report("read in code but NOT in KnownKeys (optional keys -- expected)",
       reads - known, fatal=False)

# ---- TRAP 4: THE GENERATED CFG AND THE CODE FALLBACK DISAGREEING ------------
#
# Every key has TWO defaults: the value written into a freshly generated
# sinvr.cfg, and the fallback passed to GetBool/GetFloat/GetInt when the key is
# absent. They live in different files, are written by different edits months
# apart, and NOTHING made them agree.
#
# When they diverge the fallback becomes a lie -- it documents a default nobody
# runs, because a generated cfg always contains the key. And it is the value a
# player gets if they delete a line, so one install can behave two ways
# depending on how its cfg was made.
#
# Found the day sixdof_body and shot_from_gun were made default-on: the
# generated text said 1 while the code still said false, so a fresh install
# would have had the README's promises and none of the features. Four unrelated
# keys had silently drifted the same way.
#
# Values that are not plain numeric literals (named constants like kLogInfo)
# cannot be compared without resolving them, so they are listed for the eye
# rather than guessed at.
DEFAULT_RE = '"([a-z0-9_]+) = ([^"]*?)' + re.escape(chr(92)) + 'n"'
CALL_RE = r'Get(Bool|Float|Int)\(\s*"([a-z0-9_]+)"\s*,\s*([^)]+?)\s*\)'

default_text = dict(re.findall(DEFAULT_RE, src))


def _norm(kind, v):
    v = v.strip().rstrip("f")
    if kind == "Bool":
        return {"true": "1", "false": "0"}.get(v, v)
    try:
        return "%g" % float(v)
    except ValueError:
        return None          # a named constant -- not comparable here


mismatched, unresolved = set(), set()
for m in re.finditer(CALL_RE, code_src):
    kind, key, val = m.group(1), m.group(2), m.group(3)
    if key not in default_text:
        continue
    a, b = _norm(kind, val), _norm(kind, default_text[key])
    if a is None or b is None:
        unresolved.add("%-28s code default `%s` is not a literal"
                       % (key, val.strip()))
    elif a != b:
        mismatched.add("%-28s generated cfg = %-10s code fallback = %s"
                       % (key, default_text[key], val.strip()))

report("generated cfg and CODE FALLBACK disagree (the fallback is a lie)",
       mismatched)
report("code default is a named constant -- check by eye, not compared",
       unresolved, fatal=False)

print("RESULT: %s" % ("PROBLEMS FOUND" if bad else "clean"))
sys.exit(1 if bad else 0)
