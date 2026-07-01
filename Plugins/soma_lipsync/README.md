# soma_lipsync

Custom motion-matching lip sync (PDF "Option A"): a C++ search component plus
an AnimBP that plays back the chosen `UAnimSequence` and layers it onto the
mouth bone branch. Drives 4 mouth bones from the existing 15-viseme stream
emitted by `ASomaVoiceChatbot::OnVisemeFrame` (see
`Plugins/soma_voice/Source/soma_voice/Public/SomaVoiceChatbot.h`).

## Modules

| Module | Type | Purpose |
| ------ | ---- | ------- |
| `soma_lipsync` | Runtime | Types, data assets (header API), matcher component, AnimInstance base, test actor |
| `soma_lipsyncEditor` | UncookedOnly | Bone extraction + `CallInEditor` capture/build implementations |

The runtime module exposes a `FSomaLipsyncEditorBridge` (in
`SomaLipsyncModule.h`) whose function pointers are installed by the editor
module on startup. This lets the data-asset `CallInEditor` buttons live on the
runtime UObjects without taking a hard dependency on `UnrealEd`.

## Bones (rig)

Defined in `SomaLipsyncTypes.h::SomaLipsyncBones`:

- `lowlip_8_JNT`
- `uplip_9_JNT`
- `L_lipcorner_main_JNT`
- `R_lipcorner_main_JNT`

All transforms are stored parent-relative (PDF section 4.1).

## Visemes

15 OVR-style values matching the index order produced by
`ASomaVoiceChatbot::OnVisemeFrame`:

```
0 Sil   1 PP    2 FF    3 TH    4 DD
5 KK    6 CH    7 SS    8 NN    9 RR
10 AA  11 EH   12 IH   13 OH   14 OU
```

## One-time editor setup

After C++ compiles, do the following inside the editor:

### 1. Create `DA_VisemePoseTargets` (`UVisemePoseTargetMap`)

1. Content Browser → `Plugins/soma_lipsync/Content/Data/` → right click →
   *Miscellaneous → Data Asset → Viseme Pose Target Map*. Name it
   `DA_VisemePoseTargets`.
2. Open it. In `SourceVisemeAnims`, add an entry for each of the 15
   `ESomaLipsyncViseme` values and assign the `UAnimSequence` you imported
   per viseme.
3. Set `CaptureTime = 0.0` (or per-viseme overrides if a clip's peak shape is
   not on frame 0).
4. Click `Capture From Source Anims`. The `Map` field should populate with the
   captured 4-bone poses.

### 2. Create `DA_LipsyncDB` (`ULipsyncMotionDatabase`)

1. Same path → right click → *Lipsync Motion Database*. Name `DA_LipsyncDB`.
2. Add the larger pool of facial animation `UAnimSequence`s to
   `SourceSequences`.
3. Confirm `SampleRate = 30.0` and `RequiredBones` is the default 4-bone list.
4. Click `Build Index`. The `Samples` array should fill (≈ `30 × clipLength`
   per clip). Each sample also gets a trajectory `Window` of 5 mouth poses
   centered on the sample time — see *Trajectory Matching* below.

**If you are upgrading from an older build**, re-click `Build Index` once after
the new C++ compiles. Until you do, the matcher falls back to single-pose
costs for any samples whose `Window.bValid` is `false`.

### 3. Create `ABP_LipsyncMM`

1. Content Browser → `Plugins/soma_lipsync/Content/Blueprints/` → right click
   → *Animation → Animation Blueprint*. Pick the Skeleton used by the head
   mesh and set the parent class to `SomaLipsyncAnimInstance`.
2. The native base already exposes `SelectedLipsyncSequence`,
   `SelectedLipsyncTime`, `SelectedMatchCost`, `bHasValidLipsyncMatch`,
   `SmoothedLipsyncBlendWeight` and `CurrentLipsyncViseme` and updates them
   every frame from the matcher component on the owning pawn.
3. Author the AnimGraph:

   ```
   [Base Pose / existing graph]
            │
            ▼
   ┌────────────────────────┐
   │ Sequence Evaluator     │   Sequence       = SelectedLipsyncSequence
   │                        │   Explicit Time  = SelectedLipsyncTime
   │                        │   Should Loop    = false
   └────────────────────────┘
            │
            ▼
   ┌────────────────────────┐   Optional but recommended. Catches any residual
   │ Inertialization        │   pop on clip swaps; pairs well with the
   │                        │   trajectory matcher's MinDwellSeconds gate.
   │                        │   Blend Time ≈ 0.12 s (half-life)
   └────────────────────────┘
            │
            ▼
   ┌────────────────────────┐
   │ Layered Blend Per Bone │   Bone Filter root: jaw / face root that is
   │                        │   the closest common ancestor of the 4 mouth
   │                        │   bones (so the layer only overrides mouth
   │                        │   bones; everything else falls through).
   │                        │   Blend Weight = SmoothedLipsyncBlendWeight
   └────────────────────────┘
            │
            ▼
   [ Output Pose ]
   ```

   **Binding `Sequence` on the Sequence Evaluator (UE5 quirk).** By default the
   Sequence Evaluator node only shows `Explicit Time` as an input pin;
   `Sequence` lives in the *Details* panel as a property because Unreal hides
   asset-reference properties from the pin row by default. Use one of:

   - *Property Binding (recommended).* Select the Sequence Evaluator node →
     in *Details*, find the **Sequence** field → click the small binding icon
     to the right of the asset picker (same icon that appears next to
     `Blend Weight` on Layered Blend Per Bone) → pick
     `SelectedLipsyncSequence` from the variable list. The field then reads
     "Bound to: SelectedLipsyncSequence" and is re-read every frame.
   - *Expose as Pin.* Select the node → in *Details*, right-click the
     **Sequence** property → **Expose as Pin** (sometimes labeled *Show Pin*).
     A new `Sequence` pin appears on the left; drag off it and pick the
     `SelectedLipsyncSequence` getter.

   Either approach is functionally equivalent. Do the same for `Explicit Time`
   if you prefer binding over the input pin. After binding, confirm in
   *Details*:

   - `Sequence` → **Bound** to `SelectedLipsyncSequence` (Option A) or has an
     incoming wire on the exposed pin (Option B).
   - `Explicit Time` → wired/bound to `SelectedLipsyncTime`.
   - `Should Loop` → false.
   - `Start Position` → 0 (ignored once Explicit Time drives the node).

4. Compile + save.

### 4. Create `BP_LipsyncTestActor` (optional but recommended)

1. Right click → *Blueprint Class → All Classes → SomaLipsyncTestActor*.
2. Set the Skeletal Mesh component's mesh + `Anim Class = ABP_LipsyncMM`.
3. Assign `DA_LipsyncDB` to `Matcher → Database` and `DA_VisemePoseTargets`
   to `Matcher → Pose Target Map`.
4. Drop one in a test level. PIE and press keys `1..0`, `-`, `=`, `[`, `]`,
   `\` to cycle the 15 visemes manually before audio is wired.

### 5. Wire to the live voice stream

On the character that should lip-sync:

1. Add a `Soma Lipsync Motion Matcher` component.
2. Assign `Database` and `PoseTargetMap`.
3. Leave `bAutoSubscribeToSomaVoice = true` (the component will find the
   `ASomaVoiceChatbot` in the level on `BeginPlay`, or you can set
   `VoiceChatbotActor` explicitly).
4. Set the character's mesh `Anim Class` to `ABP_LipsyncMM`.

## Console variables

- `soma.lipsync.DebugDraw` — 0 off, 1 matcher state (viseme + clip + cost +
  search/switch counters), 2 + per-bone cost breakdown, 3 + target pose
  translations.

## Trajectory Matching

The matcher does **not** match a single pose. It matches a short trajectory of
5 mouth poses around the current world time. This gives the search the future
context it needs to prefer clips that are about to transition in the right
direction — coarticulation — instead of swapping clips on every per-phoneme
argmax flicker.

**Station offsets** (relative to "now", in seconds):

| Station | Offset | Weight (default) | Role |
| ------- | ------ | ---------------- | ---- |
| 0 (past)    | -0.100 | `PastWeight = 0.5`    | Recent context; keeps the matcher anchored. |
| 1 (now)     |  0.000 | `NowWeight = 1.0`     | Current target pose. Also used as the single-pose fallback. |
| 2 (future1) | +0.067 | `Future1Weight = 0.8` | Near-future target; smooths the next phoneme transition. |
| 3 (future2) | +0.133 | `Future2Weight = 0.6` | Mid-future target. |
| 4 (future3) | +0.200 | `Future3Weight = 0.4` | Far-future target; decays out. |

**Database side.** During `Build Index`, each sample is given a `Window` of 5
mouth poses sampled at the station offsets around its base time. Samples whose
window would fall outside the clip (~300 ms at clip starts/ends) are marked
`bValid = false`; the matcher falls back to the single-pose cost for those, so
nothing breaks if a clip is too short. Existing databases keep working with
the single-pose fallback until you re-click `Build Index`.

**Runtime side.** Each tick, the matcher asks the bound `ASomaVoiceChatbot`
for the viseme entries surrounding each station's world time via
`GetVisemeAtWorldTime` and lerps the corresponding viseme target poses by
midpoint. Between utterances (no timeline), every station collapses to the
current argmax viseme's hard target.

Setting all `FutureWeight*` to zero collapses the cost function to current-pose
matching (useful for A/B testing).

## Tuning knobs (matcher component)

| Property | Default | Effect |
| -------- | ------- | ------ |
| `SearchInterval` | 0.05 s | Lower = more reactive, higher CPU. |
| `SwitchThreshold` | 0.001 | Cost margin needed to swap clips. Raise if the mouth visibly pops. |
| `MinDwellSeconds` | 0.15 s | A freshly-accepted clip cannot be replaced for this long, regardless of cost margin. Safety net on top of the trajectory cost. |
| `LowLipWeight` / `UpLipWeight` / `CornerWeight` | 1.5 / 1.5 / 1.0 | Per-bone position cost weights. |
| `RotationWeight` | 0.0 | Add rotation error to the cost. Position-only by default. |
| `PastWeight` / `NowWeight` / `Future1Weight` / `Future2Weight` / `Future3Weight` | 0.5 / 1.0 / 0.8 / 0.6 / 0.4 | Per-station weights for the trajectory cost. See *Trajectory Matching* above. |
| `VisemeArgmaxThreshold` | 0.05 | Argmax score floor for the OVR stream. |
| `VisemeSwitchMargin` | 0.05 | Hysteresis on viseme switching. |
| `bUseWeightedTargetBlend` | false | EXPERIMENTAL Option B. Body is stubbed; argmax fallback for now. |

## Validation checklist (PDF section 10)

1. Manually cycling visemes on `BP_LipsyncTestActor` changes the target pose
   and the selected database sample.
2. Selected pose visually resembles the target mouth shape.
3. Selected sequence continues advancing in time between searches; AnimBP
   playback does not freeze on a single frame.
4. With `soma.lipsync.DebugDraw 1`, `Switches/s` stays low at steady state.
5. `Layered Blend Per Bone` only affects the mouth-branch bones.
6. With voice wired, calling `ASomaVoiceChatbot::SpeakText` on the demo
   phrases produces visibly correlated mouth motion:
   - `ma ma ma`
   - `pa pa pa`
   - `oo aa ee`
   - `maybe tomorrow`

If the mouth jitters, raise `SwitchThreshold` and/or `VisemeSwitchMargin`. If
shapes don't track, double-check that the per-viseme target pose was captured
from the correct frame (use `CaptureTimeOverrides`).

## Implemented (PDF v2)

- Future-phoneme context / coarticulation (5-station trajectory matching, see
  *Trajectory Matching* above).

## Out of scope (deferred per PDF v2+)

- Bone velocity feature / opening-vs-closing disambiguation.
- Weighted-blended target pose (toggle present, body stubbed).
- Custom AnimGraph node.
- Jaw, cheek, brow, head, emotion bones.
