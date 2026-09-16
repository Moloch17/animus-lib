# animus-lib

The code [mod-animus-forge](https://github.com/Moloch17/animus-forge) (training, on the Animus Forge core) and
[mod-animus](https://github.com/Moloch17/animus) (playing trained models, on a stock AzerothCore) share: the
curriculum's stages and everything they are built from. Both modules run the same scenario code, so a stage a
game master watches with mod-animus is exactly the stage the forge trained on, and a model's observations and
actions mean the same in training and in play.

It is an AzerothCore module of its own (`modules/mod-animus-lib`) that registers only the combat hooks env pools
need. It uses public core APIs only and builds on both cores; it has no settings.

## Getting it

mod-animus and mod-animus-forge clone it when you configure and `modules/mod-animus-lib` is missing
(`https://github.com/Moloch17/animus-lib.git`, branch `master`; override with `-DANIMUS_LIB_GIT_URL=...` and
`-DANIMUS_LIB_GIT_REF=...`). To clone it yourself:

```
git clone https://github.com/Moloch17/animus-lib.git modules/mod-animus-lib
```

Build it the way you build the modules that need it: static (the default) or all dynamic.
`cmake/AnimusLibDependency.cmake` holds the rules the dependents' `.cmake` files apply.

## What is in it

| Directory | Contents |
|---|---|
| `src/Scenario/Curriculum/` | The curriculum: `Stages/` (stage and arena definitions), `Blocks/` (observation features and actions), `Layout/` (layouts, manifests, `SeatEncoder`, `SeatView`), `Encounters/` (creatures, pulls, owner, party, scripted and ambushing enemy players), `Character/` (class/role profiles, kit, talents and spec builds, gear, supplies, `SeatCharacter`), `Rewards/`, `Baselines/`, `CurriculumTuning`, `StageScenario` |
| `src/Scenario/` | `Scenario` (the interface a host drives), `StageSettings` (what a host tells it), spawn area clearing, spell checks, summon levels |
| `src/Env/` | `Env`, `EnvPool` (every env of a scenario and the flat buffers a host reads and writes), `PoolRegistry` |
| `src/Bot/` | `BotFactory` (sessionless players with no character row), `BotSlot` (bots rebuilt every episode), `BotAccounts` |
| `src/Model/` | `MlpPolicy` (the exported `.amdl` actor), `ModelLibrary` (a layout's model, checked against its manifest) |
| `src/Core/` | `CoreHooks`: the seams for what only the forge core can do |
| `src/Hooks/` | Damage, heal, cast and creature level hooks for every registered env pool |
| `tools/spec_builds/` | The standard talent builds and glyphs; `generate.py` writes `SpecBuilds.cpp`. Characters draw a standard, a partly random or a fully random build (`Characters.*TalentChance`) |

## Using it from a module

A host builds a scenario and its env pool from `StageSettings`, registers the pool so the hooks feed it, and
drives it from its world update:

```cpp
Animus::StageSettings settings;
settings.Envs = 1;
settings.TuningPrefix = "MyModule.Curriculum.";     // where CurriculumTuning reads its keys

auto scenario = Animus::CreateScenario("stage2_pack", settings);
Animus::EnvPool pool(*scenario, settings);
pool.Setup();
pool.ResetAll();
Animus::PoolRegistry::Register(&pool);

// Every world update:
pool.AdvanceClock(diff);
// Every StageSettings::DecisionMs:
pool.Collect();                     // rewards, episode ends and resets, observations
pool.ChooseLocalActions("fight");   // or fill pool.Actions from a learner or a model
pool.ApplyActions();

// Done:
Animus::PoolRegistry::Unregister(&pool);
pool.Teardown();
```

- **Threads:** everything runs on the world thread outside map updates (a `WorldScript::OnUpdate`); only the hooks
  run on map threads.
- **Loader:** the dependent's `Add<module>Scripts()` calls `Addmod_animus_libScripts()` first. It registers the
  library's scripts once, however many modules call it, so they are registered even when the library was cloned
  during the configure that built them.
- **Placing an env:** `EnvPool::PlaceEnv` builds an env in an existing instance (mod-animus's stage viewer uses the
  game master's) instead of opening a new one.
- **Env ids:** `StageSettings::FirstEnvId` keeps bot account ids and names of pools running side by side apart.
- **Core seams:** on the forge core, mod-animus-forge fills in `CoreHooks` at load (sim sessions and groups that
  never touch the database, reseeding random numbers for evaluation). On a stock core they do nothing: bots write
  the few rows a logout and an instance bind write, and remove their binds again when they go.

## Changing the curriculum

A stage's layouts are what its models were trained on: a change to a block, a stage or the class/role profiles
changes the manifests, and models exported before it are refused by `ModelLibrary`. Retrain in the forge after
such a change, then export the models again. mod-animus-forge's README documents the stages, blocks, encounters
and tuning in detail.
