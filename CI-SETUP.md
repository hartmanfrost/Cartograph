## CI setup: building & packaging the Cartograph mod

### The honest limitation (read this first)

**GitHub-hosted runners CANNOT build this mod.** `FactoryGame.uproject` pins
`EngineAssociation: "5.6.1-CSS"` — Coffee Stain's **private** Unreal Engine fork.
That engine is distributed **only** as releases on the private repo
`github.com/satisfactorymodding/UnrealEngine`, gated behind:

1. An **Epic Games account linked to your GitHub** (sign the UE EULA at
   unrealengine.com -> Connect GitHub; verify you can open
   `github.com/EpicGames/UnrealEngine` without a 404), and
2. **Membership in the satisfactorymodding engine repo**, granted via the
   community linker: <https://linker.ficsit.app/link>.

A vanilla `ubuntu-latest` / `windows-latest` runner has no engine, no
credentials, and not enough ephemeral disk for the ~tens-of-GB engine + the
from-source Wine toolchain. Cartograph is a **real C++ GameFeature module**
(`Mods/GameFeatures/Cartograph/Source/Cartograph/`), so content-only cooking is
not enough — it must be compiled against the CSS engine. **Any pipeline claiming
a github-hosted runner compiles this is fiction.** The workflow therefore targets
a **self-hosted runner** and there is **no working github-hosted fallback** (a
build-only github "large runner" still cannot legitimately obtain the engine).

This workflow is a faithful adaptation of the only authoritative blueprint — SML's
own CI — with `-dlcname=SML` swapped for `-dlcname=Cartograph`:
<https://raw.githubusercontent.com/satisfactorymodding/SatisfactoryModLoader/master/.github/workflows/build.yml>

### Prerequisites

**1. A self-hosted runner** labeled exactly `self-hosted, satisfactory, linux`.
Recommended: a high-vCPU (16–32 core), fast-NVMe Linux host (a bare VM, an
Actions Runner Controller pool on k8s like SML's upstream `arc-k8s`, or a baked
Docker/VM image). Register it against this repo (or the org):
<https://docs.github.com/en/actions/hosting-your-own-runners>.

The runner must have, on a **persistent** disk that survives between jobs:

| Component | How to provision | Why persistent |
| --- | --- | --- |
| **CSS engine 5.6.1** (`/tmp/work/ue`) | `gh release download --repo satisfactorymodding/UnrealEngine -p "UnrealEngine-CSS-Editor-Linux.tar.zst.*"` then `cat ... | zstd -d | tar -xf -`. The workflow does this once and writes `.installed`. | Tens of GB; re-downloading every job is wasteful |
| **Wine 11.0** (typelib-patched) + **MSVC 17.8** via `mircearoata/msvc-wine` (Win SDK 10.0.22621) | Build once into `$RUNNER_TOOL_CACHE` (Wine from source ~30 min). This is the cross-toolchain that lets a Linux box emit Win64 `.dll/.pdb`. | Re-building Wine from source per job is the single slowest cold step |
| **Wwise SDK** `2023.1.14.8770` (integration `2023.1.14.3555`) | `mircearoata/wwise-cli download`/`integrate-ue` (workflow does this, gated on already-integrated). | Large; integration is slow |
| **Local DDC** (`/tmp/work/ddc/cartograph`) | Created automatically; `UE-LocalDataCachePath` points UE at it. | Warm shader/asset cooks |

> The repo's `Mods/WwisePatches` plugin has `PreBuildSteps` that run
> `applyPatches.sh` automatically inside UnrealBuildTool — it dos2unix-normalizes
> and patches the Wwise SDK so it compiles against the CSS engine. **Keep it
> in place**; no extra workflow step is needed for it.

**2. Required secrets** (repo or org level — Settings -> Secrets and variables -> Actions):

| Secret | What it is |
| --- | --- |
| `BOT_TOKEN` | A GitHub PAT / bot-account token with **read access to the private `satisfactorymodding/UnrealEngine` repo**. The account owner must have linked Epic + been granted org access. Also used to download `wwise-cli`. |
| `WWISE_EMAIL` | Audiokinetic account email — required by `wwise-cli` to download the Wwise SDK. |
| `WWISE_PASSWORD` | Audiokinetic account password. |

Fork/external PRs cannot read these secrets, so the `permission-check` job fails
them early with a friendly message (mirrors SML). **Never expose the self-hosted
runner to untrusted fork code on a public repo.**

### Runner strategy options (trade-offs)

- **A. Self-hosted Linux + Wine/MSVC (this workflow, recommended).** Exact replica
  of upstream SML CI; cross-compiles Win64 client + Win64/Linux server in one
  `PackagePlugin` call. Cold first build is long (builds Wine, downloads engine);
  warm builds are minutes because everything is cached on the persistent volume.
- **B. Self-hosted Windows + native VS 2022 (17.8/17.14).** Install the
  `UnrealEngine-CSS-Editor-Win64` installer; use `Build.bat` + `RunUAT.bat`
  (no Wine). Simpler toolchain, but you maintain a Windows box and the Linux
  server target needs the UE clang cross-toolchain. Change `runs-on` labels to
  `[self-hosted, satisfactory, windows]` and swap `.sh` -> `.bat`.
- **C. Pre-baked Docker/AMI image** with engine + toolchain + Wwise, run on
  ephemeral self-hosted runners. Fastest, most reproducible cold start; rebuild
  the image when the engine or Wwise version bumps.
- **D. github-hosted — NOT viable** (see limitation above).

### What is cached (and where)

- **On the persistent runner volume (NOT `actions/cache` — too big):** the CSS
  engine, Wine + MSVC toolchain, integrated Wwise SDK, the local Zen DDC, and the
  project's `Intermediate/` compile artifacts. Each setup step is gated behind an
  existence check (`.installed` marker / `Plugins/Wwise` present) so it runs once
  and is reused. This is the primary build-cache mechanism — the same approach
  SML uses; the workflow deliberately uses **no** `actions/cache` for these
  multi-GB paths.
- **Why not `actions/cache@v4` for the engine?** It exceeds practical cache sizes
  and would re-upload/-download tens of GB per run; the per-repo cache is LRU-
  evicted and meant for small dependency dirs. Persistent runner storage wins.
- **Always rebuilt fresh (the deliverable):** the mod's compiled `.dll`/`.so` and
  cooked content — i.e. the packaged zip.

### Build optimization

- `-installed` -> the prebuilt engine is treated as read-only; only the plugin's
  modules compile. `-nocompileeditor` during packaging skips re-building the
  editor (already built in the Development-Editor step).
- `MaxParallelActions=0` (in the generated `BuildConfiguration.xml`) lets UBT
  saturate all cores; unity build + PCH stay ON (defaults). For a single small
  GameFeature module these are the meaningful levers — don't over-tune.
- A high-vCPU runner with fast NVMe for `Intermediate/` and the DDC is the biggest
  wall-clock win.

### How to download the artifact

After a successful run: open the run in the **Actions** tab -> **Summary** ->
**Artifacts** -> download **`Cartograph-<sha>`**. It contains the per-target and
merged zips from `Saved/ArchivedPlugins/Cartograph/`. That zip **is** the
distributable mod — upload it to the ficsit.app mod repository (SMR), which is the
`.smod`-equivalent. Build/package logs are in the `logs-<sha>` artifact.

### Triggers & concurrency

- `push` to `Cartograph` (default branch — warms caches for PRs), `pull_request`,
  and `workflow_dispatch` (manual release button). Docs-only edits are skipped.
- `concurrency` cancels superseded feature/PR builds but **protects the default
  branch** so release builds aren't cancelled mid-flight.

### The fragile bit: Wwise version coupling

The Wwise **download** version (`2023.1.14.8770`) and **integration** version
(`2023.1.14.3555`) are intentionally different (Wwise's API uses the first 3
version parts plus a separate 5th integration suffix). This is the historical
"Wwise version mismatch" CI failure. **Bump both in lockstep with every CSS engine
update** and re-verify against the live engine before expecting a green build.

