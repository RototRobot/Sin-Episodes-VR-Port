# Optional content overrides

> Two things change game content. The `hands.vmt` override below lives here as a
> file; the auto-reload flag is applied by **`tools/set_noautoreload.ps1`**,
> because it edits whatever the shipped weapon scripts currently say rather than
> replacing them with copies that would go stale. Both are reversible, both are
> reverted by Steam's "verify integrity of game files", and neither is applied
> automatically.

## `ITEM_FLAG_NOAUTORELOAD` — stop the gun reloading itself when empty

Only wanted alongside `arcade_reload = 1`. See `tools/set_noautoreload.ps1`
(`-Apply` / `-Revert`, and no arguments to report). It adds one line to each
clip-using `SE1\scripts\weapon_*.txt` and keeps the original as
`.sinvr-original`.

**Do not apply it until the reload gesture is tuned and enabled** — no
auto-reload plus a gesture that does not fire is worse than either alone.

Files here are **not applied automatically**. They change game content rather
than mod behaviour, so applying one is a deliberate act — the same reason the
mod refuses to patch `SinEpisodes.exe` for the 4 GB flag and ships
`sinvr_launcher.exe` instead.

## `hands.vmt` — hide the viewmodel arms

```
materials/models/weapons/v_hands/hands.vmt.hidden     $translucent 1, $alpha 0
materials/models/weapons/v_hands/hands.vmt.original   as shipped by the game
```

Copy the one you want to:

```
SE1\materials\models\weapons\v_hands\hands.vmt
```

**Why this is a content file and not a mod setting.** SiN's viewmodels are one
model containing both arms and the weapon, with a single bodypart (`numbodyparts`
is 1 on all three — verified by parsing the `.mdl` headers), so there is no
bodygroup to toggle at runtime. The only separation available is by *material*:
every weapon binds `models/weapons/v_hands/hands` for the arms and its own
material for the gun, so making that one material transparent hides the arms and
leaves the weapon drawing. One file covers every weapon.

**Only needed once the weapon is pinned to a controller.** Until then the arms
are correct — they hold a gun in front of your face, which is what the game was
authored for. With the weapon on a controller they stretch from your face to
wherever your hand is, and look wrong.

### What applying it actually does depends on your Steam branch

| branch | what `SE1\...\hands.vmt` is | risk |
|---|---|---|
| default (`public`) | **ignored** — the game loads its own copy from `vpks\depot_1301_*.vpk` | none, and no effect: the arms stay |
| `loose` beta | the **real game asset** | destructive — keep a backup |

**The `loose` branch is therefore a requirement, not an option.** Tested
2026-09-13 on the public branch (build 98575): the override was in place,
byte-for-byte the hidden version, and the arms still drew. This table used to
say the loose file shadowed the VPK; that was assumed, never tested, and wrong.
The same holds for everything else in `SE1` that replaces a stock file — the
Arcade Reload scripts and the GUI Scaling `.res` files included.

On the `loose` branch the deployed copy is kept as `hands.vmt.sinvr-original`
beside it. Either way, **Steam will overwrite it** on a branch switch or a
"verify integrity of game files", and the arms will come back with no other
symptom. If they reappear, check this file first.

### Why the mod does not write it for you

It could, and it would be reversible. But it is still modifying game content, and
the project's rule is that the mod does not change files it was not asked to
change. A config key that silently rewrites a game asset is exactly the kind of
thing that is impossible to debug six months later when someone verifies their
install and cannot work out why their arms came back.
