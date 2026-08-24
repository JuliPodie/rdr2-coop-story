# RDR2 Coop Story — protokół R2CP v20

Protocol 20 keeps the reliable `AnimSceneDefinition`/`AnimSceneControl`
handshake from v19 and extends the existing world-entity kind byte with
`Object = 2`. Only objects captured as roles of the active host AnimScene enter
that lane. V20 peers reject V19 packages during the handshake, so a peer that
cannot create those scene props cannot silently accept an incomplete cast.

Wszystkie liczby wielobajtowe są little-endian. Pointery, adresy pamięci
i lokalne uchwyty encji RDR2 są zabronione na wire.

## Nagłówek ramki

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 4 | magic `R2CP` (`0x50433252`) |
| 4 | 2 | wersja `20` |
| 6 | 2 | `MessageType` |
| 8 | 4 | sequence |
| 12 | 8 | tick |
| 20 | 4 | długość payloadu |

Nagłówek ma dokładnie 24 B. Maksymalny payload pipe/TCP to 1 MiB,
a uwierzytelniona datagrama UDP nie może przekroczyć 1200 B.

## Typy wiadomości

Numery pozostają zgodne z enumami C++ i C#. Wersja 20 zachowuje ID 1–40;
`AnimSceneDefinition` pozostaje pod ID 39, a `AnimSceneControl` pod ID 40.

| ID | Typ | ID | Typ |
|---:|---|---:|---|
| 1 | Hello | 13 | MissionState |
| 2 | HelloAck | 14 | SpectatorState |
| 3 | Heartbeat | 15 | Command |
| 4 | PlayerState | 16 | ResyncRequest |
| 5 | EntitySpawn | 17 | ResyncSnapshot |
| 6 | EntityUpdate | 18 | Error |
| 7 | EntityDespawn | 19 | Goodbye |
| 8 | DamageIntent | 20 | SessionMenuRequest |
| 9 | DamageApplied | 21 | SessionMenuStatus |
| 10 | DownedState | 22 | PlayerIdentity |
| 11 | ReviveRequest | 23 | WorldState |
| 12 | ReviveComplete | 24 | EquipmentState |
| 25 | PauseVote | 31 | MissionCameraState |
| 26 | PlayerMountState | 32 | InteractionIntent |
| 27 | PlayerTraversal | 33 | InteractionResult |
| 28 | PlayerAnimationState | 34 | RestraintState |
| 29 | MotionReplicationConfig | 35 | MissionCinematicState |
| 30 | PlayerAction | 36 | MissionCinematicAction |
| 37 | PlayerAppearanceState | 38 | AnimSceneReplicaState |
| 39 | AnimSceneDefinition | 40 | AnimSceneControl |

Pipe `Hello` ma pusty payload. Sidecar odpowiada pipe `HelloAck` o długości
jednego bajtu: Host `0`, Guest `1`. Nie należy mylić go z JSON-owym
`HelloAck` używanym podczas TCP handshake.

## `PlayerState` — 64 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | player `NetEntityId` |
| 8 | 1 | slot: Host `0`, Guest `1` |
| 9 | 1 | lifecycle: Alive `0`, Downed `1`, Reviving `2`, Spectator `3` |
| 10 | 2 | reserved, musi być `0` |
| 12 | 12 | position: `x`, `y`, `z` jako `3 × f32` |
| 24 | 12 | velocity: `x`, `y`, `z` jako `3 × f32` |
| 36 | 4 | heading `f32` |
| 40 | 4 | health fraction `f32`, zakres 0–1 |
| 44 | 4 | `PlayerStateFlags` `u32` |
| 48 | 12 | `aimTarget`: `x`, `y`, `z` jako `3 × f32` |
| 60 | 4 | `fireSequence` `u32` |

Flagi:

| Bit | Nazwa |
|---:|---|
| 0 | `InMission` |
| 1 | `InCutscene` |
| 2 | `Mounted` |
| 3 | `Aiming` |
| 4 | `Firing` |
| 5 | `AimTargetValid` |
| 6 | `MeleeCombat` |
| 31 | `OnlineModeDetected` |

Wszystkie wartości zmiennoprzecinkowe muszą być skończone. Gdy
`AimTargetValid` nie jest ustawione, trzy składowe `aimTarget` muszą być
zerowe. `fireSequence` jest licznikiem zdarzeń strzału; odbiorca porównuje go
z ostatnio zastosowaną wartością zamiast traktować samo `Firing` jako pewne
zdarzenie pojedynczego strzału.

## `PlayerIdentity` — `10 + N` B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | player `NetEntityId` |
| 8 | 1 | slot: Host `0`, Guest `1` |
| 9 | 1 | długość nicku `N` w bajtach UTF-8 |
| 10 | `N` | nick UTF-8: 1–24 znaki Unicode, maks. 64 B |

## `PlayerMountState` — 60 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | player `NetEntityId` |
| 8 | 8 | mount `NetEntityId` |
| 16 | 1 | slot: Host `0`, Guest `1` |
| 17 | 1 | flags: bit 0 `Present`, bit 1 `Mounted`, bit 2 `Dead`, bit 3 `BorrowedPeerMount` |
| 18 | 2 | reserved, musi być `0` |
| 20 | 4 | model hash `u32` |
| 24 | 12 | position `3 × f32` |
| 36 | 12 | velocity `3 × f32` |
| 48 | 4 | heading `f32`, zakres `[0, 360)` |
| 52 | 4 | health fraction `f32`, zakres 0–1 |
| 56 | 4 | generation `u32` |

`Present=0` oznacza usunięcie repliki konia; pola modelu i transformu muszą
być zerowe, a `generation` pozostaje niezerowym licznikiem. Przy `Present=1`
model jest niezerowy, oba identyfikatory są ważne
i różne. Wiadomość jest dwukierunkowym snapshotem właściciela gracza i
korzysta z uwierzytelnionego UDP.

## Stan encji World Mirror — 76 B

`EntitySpawn` i `EntityUpdate` używają identycznego
`WorldEntityStatePayload`.

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | entity `NetEntityId` |
| 8 | 4 | model hash `u32`, różny od zera |
| 12 | 1 | kind: Ped `1`, captured AnimScene Object `2` |
| 13 | 1 | `WorldEntityStateFlags` |
| 14 | 1 | combat target: None `0`, Host `1`, Guest `2` |
| 15 | 1 | task: Idle `0`, Locomotion `1`, Scenario `2`, Fleeing `3`, Combat `4`, Mounted `5`, Dead `6`, Cinematic `7` |
| 16 | 12 | position `3 × f32` |
| 28 | 12 | velocity `3 × f32` |
| 40 | 4 | heading `f32`, zakres `[0, 360)` |
| 44 | 4 | health fraction `f32`, zakres 0–1 |
| 48 | 4 | weapon hash `u32` |
| 52 | 8 | parent `NetEntityId`; koń rodzic dla dosiadanego człowieka |
| 60 | 12 | semantyczny `taskTarget` `3 × f32` |
| 72 | 4 | reserved tail, musi być `0` |

Flagi encji:

| Bit | Nazwa |
|---:|---|
| 0 | `Human` |
| 1 | `Horse` |
| 2 | `Dead` |
| 3 | `InCombat` |
| 4 | `Firing` |
| 5 | `Aiming` |
| 6 | `Mounted` |
| 7 | `ScriptOwned` |

Encja nie może być jednocześnie `Human` i `Horse`. Encja bez `InCombat`
musi mieć target `None`. Nie-ludzka encja musi mieć zerowy weapon hash.
`Firing` albo `Aiming` wymaga uzbrojonego człowieka. `Mounted` wymaga
człowieka, tasku `Mounted` oraz ważnego `parent`; pozostałe encje mają
zerowy `parent`. `taskTarget` opisuje zamiar wysokiego poziomu, nie
wewnętrzny task tree RDR2.

`Object` jest ograniczony do rekwizytu przechwyconego jako rola aktywnej
AnimScene hosta. Wymaga zerowych flag ped-only, targetu, weapon hash i parent;
dozwolony task to `Idle` albo `Cinematic`. Protokół 20 nie zamienia tej lane w
ogólną replikację wszystkich obiektów świata.

`Cinematic` oznacza host-authoritative root aktora podczas sceny. Receiver nie
uruchamia dla niego własnego zadania AI ani `TASK_STAND_STILL`; nie oznacza to
jednak, że payload zawiera klip albo wewnętrzny obiekt AnimScene.

`EntitySpawn`, `EntityUpdate` i `EntityDespawn` są host-authoritative.
Sidecar hosta odrzuca próbę publikacji tych stanów przez guesta.

## `EntityDespawn` — 8 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | prawidłowy entity `NetEntityId` |

Payload ma tylko identyfikator encji. Typ wiadomości to istniejący
`EntityDespawn` o ID 7.

## `DamageIntent` — 32 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | attacker `NetEntityId` |
| 8 | 8 | target `NetEntityId` |
| 16 | 4 | weapon hash `u32`, różny od zera |
| 20 | 4 | proponowane damage `f32` |
| 24 | 4 | `shotSequence` `u32`, różny od zera |
| 28 | 4 | reserved `u32`, musi być `0` |

Damage musi być skończone i należeć do zakresu `(0, 100]`.
`DamageIntent` jest żądaniem guesta, a nie autorytatywną zmianą zdrowia.
Host waliduje rolę nadawcy, identyfikatory i lokalny stan encji przed
zastosowaniem obrażeń do prawdziwego ambientowego NPC.

## `WorldState` — 24 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 1 | godzina `u8`, zakres 0–23 |
| 1 | 1 | minuta `u8`, zakres 0–59 |
| 2 | 1 | sekunda `u8`, zakres 0–59 |
| 3 | 1 | flags: bit 0 `WeatherValid` |
| 4 | 4 | `weatherFrom` `u32` |
| 8 | 4 | `weatherTo` `u32` |
| 12 | 4 | weather blend `f32`, zakres 0–1 |
| 16 | 1 | dzień `u8`, zakres 1–31 |
| 17 | 1 | miesiąc `u8`, zakres 0–11 |
| 18 | 2 | rok `u16`, zakres 1800–2200 |
| 20 | 4 | reserved tail `u32`, musi być `0` |

Gdy `WeatherValid` jest wyłączone, oba hashe i blend muszą być zerowe.
Gdy jest włączone, oba hashe muszą być niezerowe. `WorldState` jest
host-authoritative: host odrzuca taki stan odebrany od guesta.

## `PauseVote` — 12 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 1 | kind: `RequestToggle` `1`, `AuthoritativeState` `2` |
| 1 | 1 | voter slot: Host `0`, Guest `1` |
| 2 | 1 | flags: bit 0 `HostVoted`, bit 1 `GuestVoted`, bit 2 `Paused` |
| 3 | 1 | reserved, musi być `0` |
| 4 | 4 | generation |
| 8 | 4 | reserved tail, musi być `0` |

Guest może wysłać wyłącznie `RequestToggle` dla slotu Guest. Host koordynuje
głosy, zwiększa `generation` po każdej wspólnej zmianie i publikuje
`AuthoritativeState`. Guest odrzuca starsze generacje i nie może sam nadać
stanu autorytatywnego. Pauza lub wznowienie następuje dopiero po głosie obu
graczy.

## `MissionState` — 48 B

`MissionState` pozostaje niezawodnym stanem hosta. Niesie epokę misji,
rewizję, generację checkpointu, fazę i bezpieczną kotwicę hosta. Guest nie może
publikować własnego stanu misji.

## `MissionCinematicState` — 48 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | host `NetEntityId` |
| 8 | 4 | mission epoch `u32` |
| 12 | 4 | cinematic generation `u32` |
| 16 | 4 | revision `u32` |
| 20 | 4 | checkpoint generation `u32` |
| 24 | 1 | faza: `Playing` 1, `Loading` 2, `PrepareResume` 3, `Completed` 4, `Aborted` 5 |
| 25 | 1 | reserved, musi być `0` |
| 26 | 2 | flagi `CameraExpected`, `AnchorValid`, `SkipPending`, `ResumeTimedOut` |
| 28 | 12 | resume anchor `3 × f32` |
| 40 | 4 | resume heading `f32` |
| 44 | 4 | reserved, musi być `0` |

Stan jest host-only, niezawodny i buforowany latest-only. Jedna niezerowa
generacja obejmuje cały łańcuch `Playing → Loading → Playing`, dopóki host nie
odzyska stabilnie kontroli. Starsza epoka, generacja lub rewizja jest odrzucana;
ta sama rewizja z inną treścią również jest błędem.

## `MissionCinematicAction` — 32 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | host `NetEntityId` |
| 8 | 4 | mission epoch `u32` |
| 12 | 4 | cinematic generation `u32` |
| 16 | 4 | action ID `u32` |
| 20 | 1 | akcja: `PresentationReady` 1, `ResumeReady` 2, `SkipRequest` 3 |
| 21 | 1 | sender slot; musi być Guest `1` |
| 22 | 2 | flaga `FallbackUsed` |
| 24 | 8 | reserved, musi być `0` |

Akcja jest niezawodnym komunikatem guest→host. Duplikaty action ID, obca rola
oraz żądania dla starej generacji są ignorowane.

## `MissionCameraState` — 56 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | host `NetEntityId` |
| 8 | 4 | mission epoch `u32` |
| 12 | 4 | cinematic generation `u32` |
| 16 | 4 | revision `u32` |
| 20 | 4 | flagi `Active`, fade oraz dokładnie jedno źródło: `SourceRenderingScriptCamera`, `SourceCinematicGameplayCamera` albo `SourceGameplayCameraFallback` |
| 24 | 12 | position `3 × f32` |
| 36 | 12 | rotation `3 × f32` |
| 48 | 4 | FOV `f32` |
| 52 | 4 | reserved, musi być `0` |

Kamera jest host-only i latest-only przez uwierzytelniony UDP, próbkowany do
30 Hz. Guest przyjmuje wyłącznie snapshot bieżącej epoki i generacji. Pierwszy
keyframe jest wysyłany w `Loading`, przed `Playing`. Guest zachowuje ostatnią
klatkę przez 2500 ms krótkiej utraty snapshotów; dopiero potem może uruchomić
odwracalny fallback śledzący hosta. Filmowe cięcia stosują transform bez
gameplayowego wygładzania.

Aktywny payload musi mieć dokładnie jedną flagę źródła. Payload nieaktywny nie
może mieć żadnej flagi źródła.

## `PlayerAppearanceState` — 32 B + komponenty

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | player `NetEntityId` |
| 8 | 1 | slot: Host `0`, Guest `1` |
| 9 | 1 | schema version, obecnie `1` |
| 10 | 2 | `CompleteComponentSet`, `StoryMetaPed` |
| 12 | 4 | revision `u32` |
| 16 | 4 | model hash `u32` |
| 20 | 2 | liczba komponentów, 1–64 |
| 22 | 2 | reserved, musi być `0` |
| 24 | 8 | fingerprint `u64` |
| 32 | 4 × N | uporządkowane hashe shop-component `u32` |

Stan jest niezawodny, dwukierunkowy i latest-only. Odbiorca stosuje pełny
zestaw wyłącznie do proxy o identycznym modelu; różnica modelu nie powoduje
resetu komponentów. Kolejność jest zachowana, ponieważ późniejsze warstwy
MetaPed mogą nadpisywać wcześniejsze. Lokalne struktury tekstur, pointery i
uchwyty nie opuszczają procesu.

## `AnimSceneReplicaState` — 72 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | host `NetEntityId` |
| 8 | 4 | mission epoch `u32` |
| 12 | 4 | cinematic generation `u32` |
| 16 | 4 | definition revision `u32`; `0` oznacza signature-only fallback |
| 20 | 4 | state revision `u32` |
| 24 | 4 | dictionary hash `u32` |
| 28 | 4 | flagi `Active`, `Running`, `Loaded`, `CameraActive`, `OriginValid` |
| 32 | 4 | normalized phase `f32` |
| 36 | 4 | duration seconds `f32` |
| 40 | 4 | rate `f32` |
| 44 | 12 | origin position `3 × f32` |
| 56 | 12 | origin rotation `3 × f32` |
| 68 | 2 | active camera count `u16` |
| 70 | 2 | reserved, musi być `0` |

Jest to host-only snapshot UDP do 20 Hz. Nie przenosi lokalnego uchwytu sceny.
Revision `0` zachowuje dotychczasowy signature-only `ATTACHED`/`SAFE_FALLBACK`.
Revision niezerowe może sterować dokładną sceną tylko po przyjęciu definicji o
tym samym kluczu i fingerprincie; spóźniony snapshot starej definicji jest
odrzucany.

## `AnimSceneDefinition` — `60 + R + P + Σ(20 + N)` B, maks. 8192 B

Wiadomość jest host-only, niezawodna i uporządkowana. Nie wolno jej wysyłać przez
UDP ani scalać jako latest-only.

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | host `NetEntityId` |
| 8 | 4 | mission epoch `u32` |
| 12 | 4 | cinematic generation `u32` |
| 16 | 4 | definition revision `u32`, niezerowe |
| 20 | 4 | dictionary hash `u32` |
| 24 | 8 | fingerprint low `u64` |
| 32 | 8 | fingerprint high `u64` |
| 40 | 4 | duration seconds `f32` |
| 44 | 2 | długość resource name `R`, 1–256 B |
| 46 | 2 | długość playback list `P`, 0–128 B |
| 48 | 2 | liczba ról, 0–48 |
| 50 | 2 | reserved, musi być `0` |
| 52 | 4 | scene flags przekazane do `CREATE_ANIM_SCENE` |
| 56 | 1 | create option flags; dozwolona maska `0x03` |
| 57 | 3 | reserved, musi być `0` |
| 60 | `R` | resource name, drukowalne ASCII bez NUL/control |
| `60+R` | `P` | playback list, drukowalne ASCII bez NUL/control |

Po stringach występują role posortowane rosnąco po unikalnej nazwie. Stały
nagłówek roli ma 20 B:

| Offset roli | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | stabilne `NetEntityId`; `0` tylko dla niewymaganej roli bez bindingu |
| 8 | 4 | model hash; `0` przy braku bindingu |
| 12 | 4 | binding flags; `0` przy braku bindingu |
| 16 | 2 | `Required` bit 0, `Player` bit 1 |
| 18 | 1 | kind: Ped `1`, Horse `2`, Object `3`, Vehicle `4`, Pickup `5` |
| 19 | 1 | długość nazwy `N`, 1–64 B |
| 20 | `N` | nazwa roli, drukowalne ASCII bez NUL/control |

Fingerprint to pierwsze 16 bajtów SHA-256 kanonicznego payloadu z wyzerowanymi
bajtami fingerprintu, interpretowane jako dwa little-endian `u64`. Decoder
odrzuca niekanoniczną kolejność, duplikat nazwy, nieznaną flagę/kind, zły
fingerprint i niespójne pola roli.

## `AnimSceneControl` — 64 B

| Offset | Rozmiar | Pole |
|---:|---:|---|
| 0 | 8 | host `NetEntityId` |
| 8 | 4 | mission epoch `u32` |
| 12 | 4 | cinematic generation `u32` |
| 16 | 4 | definition revision `u32` |
| 20 | 4 | action ID `u32`, niezerowe |
| 24 | 16 | fingerprint low/high |
| 40 | 8 | host sample tick `u64` (korelacja diagnostyczna, nie wspólny zegar) |
| 48 | 4 | start phase `f32` |
| 52 | 4 | rate `f32` |
| 56 | 1 | kind: GuestReady `1`, GuestRejected `2`, HostPlayCommit `3`, HostAbort `4` |
| 57 | 1 | sender slot: Host `0`, Guest `1` |
| 58 | 1 | reason enum |
| 59 | 1 | reserved, musi być `0` |
| 60 | 4 | flags |

Dozwolona sekwencja to `Definition -> GuestReady -> HostPlayCommit`.
`GuestRejected` albo timeout kończą się `HostAbort` i flagą `FallbackUsed`.
Guest może wysłać tylko Ready/Rejected, host tylko Commit/Abort. Każda kontrola
musi dokładnie zgadzać się z hostem, epoką, generacją, revision i fingerprintem
aktywnej definicji. Sidecar zachowuje FIFO, nie używa UDP i nie blokuje hostowego
Story VM podczas oczekiwania.

`playAtHostTick` zachowuje nazwę pola wire, ale zegary HOST/GUEST nie są wspólne.
Pole oznacza moment próbki hosta i służy korelacji logów. Guest uruchamia
przygotowaną scenę natychmiast po poprawnym commit, od podanej `start phase`, a
dalszy strumień fazy koryguje dryf. Nie wolno planować lokalnego startu przez
porównanie tego ticku z zegarem guesta.

Guest ma 8000 ms na cold-load i lokalne przygotowanie definicji, natomiast host
przyjmuje odpowiedź przez 10000 ms, uwzględniając transport. Po odebraniu
`GuestReady` host ma
2500 ms na commit/abort, a guest czeka na tę decyzję 4000 ms. Niesymetryczne okna
usuwają wyścig na granicy legalnego timeoutu. Po CREATE guest ponawia lokalne
rozwiązywanie ról i nie może wywołać `LOAD_ANIM_SCENE`, dopóki wszystkie role z
flagą `Required` nie istnieją, nie mają oczekiwanego modelu i nie są przypięte.
Po LOAD nadal wymaga `resource-loaded`; dopiero wtedy wysyła `GuestReady`. Hostowy
`PRELOAD_BARRIER` jest wyłącznie stanem logicznym: game-owned handle hosta nie
jest pauzowany, przyspieszany ani usuwany. Każdy reject, timeout, terminal albo
teardown kończy próbę fail-safe. Wire nie przenosi lokalnego handle.

Diagnostyczny `PREPARE_PROGRESS` nie jest nowym payloadem protokołu. Lokalnie
rozróżnia `waiting-bindings`, `waiting-resource`, `ready` i `failed`; może podać
wyłącznie nazwę roli oraz stabilny `NetEntityId`, nigdy pointer ani uchwyt RDR2.

## Pozostałe stałe payloady

- `Command` — 32 B: opcode `u16`, flags `u16`, entity ID `u64`,
  pozycja `3 × f32`, heading `f32`, value `f32`.
- `DownedState`/`SpectatorState` — 16 B.
- `ReviveRequest` — 16 B: reviver ID i target ID.
- `ReviveComplete` — 20 B: oba ID i health fraction.
- `EquipmentState` — 24 B:

  | Offset | Rozmiar | Pole |
  |---:|---:|---|
  | 0 | 8 | entity ID `u64`, różny od zera |
  | 8 | 4 | weapon hash `u32` |
  | 12 | 4 | ammo `u32` |
  | 16 | 4 | flags: bit 0 `Equipped`, bit 1 `Reloading` |
  | 20 | 4 | reserved `u32`, musi być `0` |

`WorldState`, `EquipmentState`, `PlayerAppearanceState`, `EntitySpawn`, `EntityDespawn`, `PauseVote`,
`MissionState`, `MissionCinematicState`, `MissionCinematicAction`,
`AnimSceneDefinition` i `AnimSceneControl` są
wiadomościami kontrolnymi TCP. Szybkie `PlayerState`, `PlayerMountState`,
`EntityUpdate`, `MissionCameraState` oraz `AnimSceneReplicaState` korzystają
z uwierzytelnionego UDP.

## `NetEntityId`

`NetEntityId` jest `u64`: host/session epoch `u32` w starszej połowie
i counter `u32` w młodszej. Obie części muszą być niezerowe. Identyfikator
jest stabilny na wire; lokalne uchwyty RDR2 nigdy go nie zastępują.

## Opcode poleceń

| ID | Opcode | ID | Opcode |
|---:|---|---:|---|
| 1 | SpawnReplica | 9 | RetryCheckpoint |
| 2 | ApplyTransform | 10 | SoloOverrideOn |
| 3 | DespawnReplica | 11 | SoloOverrideOff |
| 4 | SpectatorOn | 12 | Resync |
| 5 | SpectatorOff | 13 | Unload |
| 6 | TeleportGuest | 14 | ToggleDiagnostics |
| 7 | EnterDowned | 15 | ResyncEquipment |
| 8 | CompleteRevive |  |  |

`Unload` i `ToggleDiagnostics` są lokalnymi operacjami bridge'a i nie są
dozwolonymi poleceniami z peera. Guest nie może wysyłać do hosta
autorytatywnych `Command`.

## Uwierzytelnienie i reconnect

TCP handshake używa losowych nonce'ów i HMAC-SHA256 opartych o wspólny token
sesji. Datagram UDP ma układ `[tag HMAC 16 B][sender InstanceId 16 B][ramka R2CP]`.
`InstanceId` jest zapisane w kolejności big-endian/RFC 4122, a skrócony HMAC
obejmuje razem `InstanceId` i całą ramkę. Odbiorca wymaga dokładnego identyfikatora
procesu poznanego w bieżącym handshake TCP oraz sekwencji nowszej od ramki
`Hello/HelloAck`, zanim przypnie port UDP. Opóźniony datagram starego procesu albo
socketu jest więc odrzucany przed pinningiem. Cały datagram nadal ma limit 1200 B.
Token sesji nie jest wysyłany wprost ani logowany.

Guest reconnectuje z wykładniczym backoffem ograniczonym do 10 sekund.
Po udanym handshake wysyła `ResyncRequest`. Sidecar odtwarza najpierw
`MissionState`, następnie `MissionCinematicState`, a dopiero potem graf świata,
aby proxy nie pojawiły się przed odtworzeniem izolacji misji i cutscenki.
Aktywna `AnimSceneDefinition` jest replayowana dopiero po grafie świata, tak aby
każda wymagana rola mogła już rozwiązać swoje `NetEntityId`. Cache definicji nie
przechowuje lokalnych uchwytów i jest czyszczony przez dopasowany `HostAbort`,
terminalną fazę cutscenki albo reset sesji.

Po reconnect/resync wymagającym ponownego prepare host publikuje definicję z nową
`DefinitionRevision` i ponownie wyliczonym fingerprintem. Odpowiedź `GuestReady`
ze starszej próby jest odrzucana także wtedy, gdy dotarła po nowym
`PlayerState`. Host nie przyjmuje bieżącego `Ready`, dopóki nie wyśle wszystkich
stabilnych spawnów oraz nowej definicji. Lokalny guest czyści stary desired-state
graf przed wysłaniem resyncu do hosta, więc encja usunięta podczas przerwy nie
pozostaje jako osierocone proxy.

Host przechwytuje i wysyła replay `MissionState -> MissionCinematicState ->
EntitySpawn parent-first -> AnimSceneDefinition` jako jeden batch związany z
konkretnym `ControlPeerToken`; live reliable state korzysta z tej samej bramki.
Wymiana peera zatrzymuje stary suffix zamiast skierować go do nowego połączenia.
Po stronie guesta reset cache/grafu jest dostarczany do bridge'a pod barierą
kolejki i request do hosta wychodzi dopiero po potwierdzonym pełnym zapisie. Named
pipe ma własny token generacji oraz stan ready przypięty do jego `HelloAck`, więc
reset przeznaczony dla pipe A nie może zostać zapisany do pipe B przed negocjacją.
Każdy inbound callback ponownie sprawdza ten sam `ControlPeerToken` przed mutacją
cache/rejestru i przed enqueue. Wszystkie lane'y network→bridge przechowują
predicate generacji sprawdzany bezpośrednio przed zapisem, a globalny numer
enqueue zachowuje kolejność między coalesced `MissionState`, critical spawnami i
definicją. Unieważniona ramka starego peera nie jest retryowana w nowej sesji.

## V31.5 interaction/mount behavior on protocol 19

V31.5 nie zmienia ID wiadomości, rozmiaru payloadu ani wersji protokołu. Lokalny
`MISSION_FLAG` guesta jest zajmowany wyłącznie podczas prezentacji hosta,
kwarantanny lokalnej Story VM lub w pobliżu potwierdzonego mission-owned aktora z
przypiętym blipem. Sam aktywny lease albo `MissionState.Active` nie zajmuje flagi,
więc zwykłe M2/talk i mount pozostają dostępne.

Celowe ściągnięcie drugiego gracza z konia nadal używa
`PlayerAction(Knockdown)`, ale istniejące pole `variantHash` musi mieć wartość
`0x4D50554C` (`MPUL`) i flagę `VariantValid`. Odbiorca nie może wykonać
`TaskDismountAnimal` na lokalnym graczu dla zwykłego/nieoznaczonego Knockdown.
Nadawca nie tworzy `MPUL`, gdy ten sam input jest aktualnie wejściem kontekstowym
talk/mount albo własny koń znajduje się w zasięgu interakcji. Relacja
`PlayerMountState` może mutować tylko dwukierunkowo zweryfikowany uchwyt zdalnej
repliki; lokalny `PLAYER_PED_ID` jest zabroniony.

# V26.0 behavior on protocol 16

No wire IDs or payload sizes changed. `MissionCinematicState` IDs 35–36 retain
their protocol-16 encoding. The behavioral contract is stricter:

- `PrepareResume` is held while an authenticated guest replica is fresh;
- only a matching-generation `ResumeReady` or confirmed peer loss releases it;
- `ResumeTimedOut` remains decodable for compatibility but V26.0 does not use a
  five-second forced completion;
- entity spawn/update frames are accepted during the matching cinematic
  generation so host actors can accompany the replicated camera;
- `RestraintState` still uses its existing payload, but both endpoints now keep
  the victim's visible proxy constrained until the authoritative `Free` state.

## V27.0 behavior on protocol 16

Wire IDs and payload sizes remain unchanged. The behavioral rules are now:

- a connected guest answers the first `Loading` state with
  `PresentationReady` before the host publishes `Playing`;
- `PrepareResume` is monotonic and can never return to `Playing` in the same
  cinematic generation;
- `Completed` suppresses transient camera re-entry until the local cutscene
  signal has remained clear for 1500 ms;
- `SkipRequest` is a guest vote, not an immediate command. Host and guest votes
  must overlap inside one five-second window and match the current generation;
- the last active camera snapshot remains presentation-fresh for 1000 ms to
  cover the host's 750 ms control-recovery debounce.

## V28.0 behavior on protocol 17

Wire IDs and payload sizes remain unchanged, but validation is intentionally
incompatible with V16:

- active camera frames identify exactly one sampling source;
- the host sends camera keyframes during `Loading` and waits for both the first
  valid keyframe and `PresentationReady` before publishing `Playing`;
- the guest applies authored camera cuts directly and holds the last frame for
  2500 ms through a short UDP gap;
- `WorldTaskKind.Cinematic` updates Story actor roots at up to 30 Hz without
  inventing a local AI task;
- the full-session guest mission sentinel remains the only owner of the local
  vanilla mission gate. Mission progress is still host-only.

## V29.2 behavior on protocol 18

- ordered Story MetaPed shop-component sets are sampled every two seconds and
  re-applied only when their fingerprint changes;
- component application is fail-closed on a model mismatch;
- the host selects only an engine-owned AnimScene that currently owns an active
  camera and publishes its portable dictionary/duration/phase/rate/origin
  signature;
- the guest attaches only to a matching local engine scene and uses its authored
  camera, actor animation, voice, subtitle and audio tracks;
- phase drift is corrected by bounded rate control or a short pause when the
  guest gets ahead;
- only a complete local-handle sweep may report `SAFE_FALLBACK`; if no matching
  local scene exists, no mission or scene is invented and the V28
  camera-keyframe presentation remains active;
- guest-local Story scenes are suppressed for the full authoritative host
  mission and the full stable-clear quarantine; an exact bridge-owned
  AnimScene temporarily exempts the process-global skip input.

## V29.3 launcher/session behavior on protocol 18

V29.3 does not change wire IDs or payloads. A complete HOST or JOIN
configuration written by the launcher sets `inGameMenuEnabled=false`, so the
sidecar starts the selected LAN role before the game bridge connects. The F8
overlay starts closed and remains an emergency/manual fallback. Sidecar emits
local process-only `COOP_LOBBY_STATUS` lines for peer connectivity, remote
identity and bridge connectivity; they contain no session token and are used
only to update the launcher's colored HOST/GUEST lobby and ping display.

`F7` is also local-only: one rising edge writes the existing correlated
`USER_MARKER` diagnostic and displays a 2.5-second confirmation. It introduces
no new network message and does not change protocol 18 compatibility.

## V29.4 password UX on protocol 18

V29.4 removes the opaque R2C1 token from the normal user interface without
changing the authenticated handshake or any wire payload. Both launchers derive
the existing internal `SessionCredentials` token from the same normalized
4–64-character password and canonical host IPv4 using PBKDF2-HMAC-SHA256 with
600,000 iterations and a versioned domain-separated salt. The first 16 derived
bytes form the session identifier and the remaining 32 bytes form the HMAC
secret expected by protocol 18.

The clear-text password is never serialized. Only the derived credential is
written to the local sidecar configuration, where the existing diagnostic
redactors continue to treat it as a secret. HOST/JOIN actions were removed from
the F8 overlay, preventing the legacy clipboard invite path from bypassing the
launcher password flow.

## V29.5 mission/lasso behavior on protocol 18

V29.5 does not add or resize a wire payload. The guest keeps Story marker
initialization available in co-op free roam, then asserts its process-local
mission flag when authenticated host mission/presentation authority is active
or guest-local quarantine is required. The 20 m Story actor context guard stays
armed for the whole guest lease, covering the delivery race without keeping
the mission flag permanently occupied. The visible lock/padlock is local
vanilla presentation; mission progress remains host-only.

The guest objective panel is also local-only. `MissionState.hostAnchor` still
drives the yellow spatial marker; exact localized objective text is not added to
protocol 18 because the verified SDK surface cannot read the host Story VM's
current objective string.

For lasso, an authenticated target-bearing `PlayerAction(Lasso, Begin/Sustain)`
starts the receiver's native lasso task before `PhysicalTargetEffect` is set.
The latter flag remains the later authoritative confirmation that the sender's
engine observed restraint. Existing actor/target IDs, weapon hash, target point,
action ID and revision are sufficient. A missing native constraint may retry
the lasso task once, but it must never be replaced by a synthetic ragdoll;
`RestraintState.Free` and the existing bounded lease still release ownership.

## V29.6 runtime corrections on protocol 18

V29.6 changes no wire ID, layout or authority rule. The guest reserves its
process-local Story mission gate for the complete authenticated guest lease.
This was intended to make marker scripts see the vanilla locked state during
initialization. V30.2 diagnostics later proved that already-initialized marker
visuals can remain yellow; the V30.3 section defines the enforceable behavior.

AnimScene signature matching now retains a stopped but still loaded matching
handle and permits one bounded local restart. Camera fallback always masks the
transform-only host-world proxy cast; those entities do not own the local
AnimScene graph and must not be rendered as a substitute cast.

Reliable `MeleeAttack` terminal delivery no longer cancels the receiver's
bounded native strike before impact. A lasso visual task remains motor owner
until terminal even when the native constraint query does not confirm after its
single retry. When the local engine owns a physical lasso constraint to the
remote proxy, network root correction yields to that constraint. All of these
are receiver-side scheduling changes using existing protocol-18 action fields.

## V30.0 directional AnimGraph semantics on protocol 18

V30.0 does not resize `PlayerState`, `PlayerAction` or
`PlayerAnimationState`. The previously transmitted `movementHeading`,
`localForwardSpeed` and `localRightSpeed` values are now generated in RDR2's
native heading convention and classified into eight receiver-side directions.

Bits 20–22 of `PlayerState.flags`, previously reserved, are assigned to
`InWater`, `Swimming` and `SwimmingUnderwater`. Older protocol-18 relays preserve
the 32-bit flags word and ignore these bits; the V30 bridge uses them to avoid
classifying vertical swimming as airborne and to compare requested water state
with the local RDR2 graph.

`PlayerAction.NormalizedPhaseValid` now accompanies the normalized lifetime of
the reliable semantic action transaction. It is explicitly not an exact RAGE
clip cursor. Exact `primaryNormalizedPhase` remains invalid in
`PlayerAnimationState` until a versioned memory reader positively resolves the
clip and phase for the supported executable hash.

## V30.1 correlated diagnostic marker on protocol 18

V30.1 adds `CommandOpcode.DiagnosticMarker = 16` without resizing the existing
32-byte `CommandPayload` or changing the protocol version. The command is a
diagnostic-only authenticated control transition and has no gameplay authority.
`TargetEntityId` carries a non-zero correlation value below `2^53`: bits 48–51
identify the origin role, bits 24–47 contain a bounded sender tick and bits 0–23
contain the local marker number. `Position`, `Heading` and `Value` are diagnostic
context only.

Unlike gameplay commands, `DiagnosticMarker` is authorized symmetrically. A
local bridge may emit it only when its encoded origin matches its selected role;
an authenticated peer may deliver it only when the encoded origin matches the
opposite role. The receiver never applies it through ScriptHook gameplay command
bindings. It records the shared marker, starts a bounded 15-second diagnostic
burst and displays a transient confirmation. All existing wire layouts and
protocol-18 gameplay authority rules remain unchanged.

## V30.2 receiver recovery on protocol 18

V30.2 changes no wire layout. A bounded native shoot task advances the remote
weapon graph while the proxy weapon is temporarily empty, so the visual recoil
cannot create a second damaging projectile. Crouch and cover observation
watchdogs recover state lost by another receiver-side task.

## V30.3 cutscene, mission and lasso corrections on protocol 18

V30.3 changes no message ID or payload size. Host/guest V30.2 evidence showed a
healthy AnimScene/camera stream but no matching local scene on the guest. A
signature match still produces `ATTACHED`. Without one, the guest keeps the host
camera and renders a non-colliding, smoothed host-world proxy cast; this is
reported as `PROXY_CAST_FALLBACK` and does not claim authored gestures, lipsync,
dialogue audio or an exact clip phase.

When host presentation ends, the guest probes running local camera-owning
AnimScenes for five seconds. A late guest-save scene is quarantined and receives
the existing single 2500 ms vanilla skip-input window, preventing a second
sequential presentation without adding a scene-control wire command.

The local mission flag remains reserved for the full guest lease, but it is no
longer treated as proof that a marker is gray. The enforceable receiver-side
guard scans nearby non-mount Story actors for the full lease, suppresses context
controls, and displays a local gray lock notice. Host replicas, both players and
mountable animals are excluded.

If RDR2 has no aimed entity, a narrow camera-ray corridor can associate a lasso
Begin with the peer. A target-only Sustain cannot recreate `TASK_LASSO_PED` after
the physical restraint lease releases it; only a fresh Begin, newly acquired
target or renewed physical effect can restart it. Synthetic lasso ragdoll remains
forbidden. These are local sampling/scheduling changes under protocol 18.
