# TEST V31.10 — Cutscene Mission Ownership + AnimScene OFF/ON (2 PC)

## Cel tej paczki

V31.10 jest poprawionym eksperymentalnym buildem, który po zgodnym preflighcie
przechwytuje rzeczywistą definicję AnimScene Story VM hosta i próbuje utworzyć
ten sam zasób po stronie guesta. Test ma dwa obowiązkowe przebiegi: najpierw oba
PC z `STORY VM CAPTURE` wyłączonym, potem oba PC z włączonym.

Tryb ON zakłada odwracalne hooki wyłącznie dla przypiętego układu
`RDR2.exe 1.0.1491.50` / `ScriptHookRDR2 1.0.1491.17` i siedmiu dokładnie
zgodnych prologów. Niezgodność, brak roli, modelu albo timeout wybiera
`SAFE_FALLBACK`. To nadal build Alpha: crash lub fail-closed fallback są możliwe,
dlatego nie testuj bez świeżej kopii save'a hosta.

## Przygotowanie

1. Obaj gracze muszą użyć dokładnie tej samej paczki V31.10 i protokołu 20.
2. Zachowaj własny lokalny ScriptHook. ZIP nie zawiera `ScriptHookRDR2.dll`,
   `dinput8.dll` ani `NativeTrainer.asi`.
3. Zrób kopię save'a hosta. Guest nie używa swojego postępu jako autorytetu.
4. Uruchamiaj HOST/JOIN z launchera. Nie włączaj `Solo override`.
5. `ANIMGRAPH REPLICA` ustaw identycznie na obu PC i nie zmieniaj go między
   przebiegami A/B. Różnić ma się wyłącznie `STORY VM CAPTURE`.
6. Po dołączeniu odczekaj około 20 sekund, dopiero potem idź bezpośrednio do tej
   samej krótkiej misji i tej samej pierwszej cutscenki.

## Obowiązkowa druga scena konna

1. Nie kończ pomiaru po pierwszej scenie z rozmową. Dojedź do drugiej prezentacji
   z cinematic camera nad jadącą grupą koni.
2. Guest musi wejść w tę scenę równocześnie z hostem. Dutch, Hosea, pozostali
   NPC i konie nie mogą zniknąć przy zmianie fazy. Poprawny log guesta zawiera
   `retained stable host cast ... nodes=N`, gdzie `N` jest większe od zera.
3. Po scenie guest ma wrócić obok bieżącej pozycji hosta, nie do starego punktu
   sprzed rozpoczęcia jazdy. Oczekiwany log to
   `[MISSION_RESUME] preparing from fresh remote-host position` z podanym
   `wire-drift-m`.
4. Kontynuuj jazdę przez co najmniej 90 sekund. Guest nie może umrzeć ani zostać
   cofnięty przez prywatne `weather too harsh`. Jeżeli lokalny save spróbuje
   nałożyć taką karę, log powinien zawierać `LOCAL_HAZARD_GUARD`, a rozgrywka ma
   trwać bez checkpoint reload.
5. Panel `AKTYWNA MISJA HOSTA` i żółty marker wskazują wspólny autorytet i pozycję
   hosta. Dokładny tekst vanilla objective nie jest jeszcze przesyłany przez
   protokół; nie myl braku oryginalnego zdania z utratą MissionState.

## Regresja interakcji i dwóch koni — przed przebiegiem A

1. Guest trzyma M2 przy dwóch zwykłych NPC i wykonuje co najmniej jedną dostępną
   rozmowę `Greet/Antagonize`. Nie może pojawić się szary komunikat blokady moda.
2. Guest podchodzi do swojego konia, gdy host siedzi na swoim. Guest wsiada i
   zsiada dwa razy. Host nie może wykonać animacji zsiadania ani stracić siodła.
3. Zamieńcie role czynności: guest siedzi na swoim koniu, a host wsiada i zsiada
   ze swojego. Guest nie może zostać zrzucony.
4. Dopiero przy rzeczywistym blipowanym aktorze lokalnej misji guesta straż ma
   wyświetlić `COOP: MISJA ZABLOKOWANA DLA GOSCIA` i uniemożliwić jej start.
5. Jeśli zwykła interakcja zostanie odrzucona, naciśnij F7. Poprawny przypadek
   nakładających się kontrolek może zapisać `MOUNT_INPUT_ISOLATION`, ale przy
   zwykłym dosiadaniu własnego konia nie może wystąpić `PEER_DISMOUNT`.

## Przebieg A — STORY VM CAPTURE wyłączony

1. Na obu PC wyłącz `STORY VM CAPTURE`, zapisz ustawienia i uruchom sesję.
2. Host rozpoczyna wybraną cutscenkę. Nie naciskaj skipu przy pierwszej próbie.
3. W obu logach powinien być `CAPTURE][CONFIG] disabled`; nie może być
   `CAPTURE][ENABLED]`, `CAPTURED` ani `NATIVE_CREATE`.
4. Lasso pomiń w tej iteracji; obserwuj wyłącznie cutscenkę i stan misji po niej.
5. Eksporty nazwij `CutsceneMissionV31.10-OFF-HOST.zip` oraz
   `CutsceneMissionV31.10-OFF-GUEST.zip`.

## Przebieg B — STORY VM CAPTURE włączony

1. Zatrzymaj sesję przyciskiem STOP. Włącz `STORY VM CAPTURE` na obu PC, zapisz
   ustawienia i ponownie uruchom HOST/JOIN. Tak — checkbox ma być włączony u
   HOST-a i GUEST-a; host przechwytuje definicję, guest potrzebuje zgody na
   utworzenie własnego bridge-owned AnimScene.
2. Powtórz tę samą misję i możliwie tę samą cutscenkę, bez pierwszego skipu.
3. Obserwuj od początku Loading do co najmniej 15 sekund po odzyskaniu sterowania:
   kamerę, Dutch/Hosea i innych NPC, konie, gesty, lipsync, audio, napisy, pozycję
   Arthura oraz ewentualne `Mission Failed`.
4. Dopiero w drugiej próbie sceny przetestuj wspólny skip: najpierw jedna osoba,
   potem druga. Głosy pozostają ważne do końca tej cutscenki; po drugim głosie
   oba końce muszą przejść dalej. Guest nie może zostać z samym audio.
5. Eksporty nazwij `CutsceneMissionV31.10-ON-HOST.zip` oraz
   `CutsceneMissionV31.10-ON-GUEST.zip`.

## Oczekiwane linie exact path

HOST z Capture ON powinien pokazać:

```text
[ANIMSCENE_HYBRID][CAPTURE][CONFIG] enabled ...
[ANIMSCENE_HYBRID][CAPTURE][PREFLIGHT] ...
[ANIMSCENE_HYBRID][INSPECTOR][HANDLER] native=CREATE_ANIM_SCENE ...
[ANIMSCENE_HYBRID][CAPTURE][ENABLED] role=host, accepted=7/7, detour=enabled, trampoline=register-preserving-indirect ...
[ANIMSCENE_HYBRID][CAPTURED] sequence=... resource=... playback=... scene-flags=... create-options=... roles=...
[ANIMSCENE_HYBRID][CAPTURE_MAP] mapped=22 optional-unbound=0 unresolved-players=0 unresolved-required=0 ...
[ANIMSCENE_HYBRID][PRELOAD_BARRIER] armed, reason=definition-ready-for-guest
```

Następnie transport powinien przejść przez `AnimSceneDefinition`, `GuestReady`
i `HostPlayCommit`. GUEST powinien pokazać:

```text
[ANIMSCENE_HYBRID][CAPTURE][VALIDATED] role=guest, accepted=7/7, detour=disabled, capture=off, native-create=on ...
[ANIMSCENE_HYBRID][NATIVE_CREATE] bridge-owned handle=...
[ANIMSCENE_HYBRID][PREPARE_PROGRESS] stage=waiting-bindings ... resolved-required=.../22
[ANIMSCENE_HYBRID][NATIVE_LOAD] handle=... prebound=22/22 required=22/22
[ANIMSCENE_HYBRID][PREPARE_PROGRESS] stage=waiting-resource|ready ...
[ANIMSCENE_HYBRID][NATIVE_START] bridge-owned exact scene committed ...
```

Na GUEST nie może wystąpić `CAPTURE][ENABLED]` ani `detour=enabled`. Checkbox
pozostaje włączony na obu komputerach, ale tylko HOST ma prawo obserwować Story
VM; GUEST używa zwalidowanych handlerów wyłącznie do własnego native-create.

Przed `HostPlayCommit` hostowa scena ma działać normalnie; logiczny
`PRELOAD_BARRIER` nie może pauzować ani zmieniać jej fazy. Po commit powinien
zapisać `PRELOAD_BARRIER released, reason=guest-ready-commit`. Przy fallbacku też
musi być linia `released` z powodem reject/timeout/reset.

W ODR1_INT najnowszy przebieg miał pełne `mapped=22`. Jeżeli gra zwolni uchwyty
scene-local propów przed drainem, `mapped=14 optional-unbound=8` dla ról
`p_*`/`w_*` również jest bezpieczne, o ile `unresolved-required=0`. Naciśnij F7, jeśli brakuje
wymaganego aktora albo exact path nie przechodzi do Commit. `HANDLER_REJECTED`,
`CAPTURE_REJECTED`, `GuestRejected`, `HostAbort` albo
`SAFE_FALLBACK` są bezpiecznym wynikiem diagnostycznym. Nie podmieniaj DLL i nie
próbuj wymuszać hooka. Zapisz F7 i wyeksportuj logi.

## Zakres tej iteracji

Nie testuj lassa. V31.10 nie zmienia jego implementacji; potrzebny jest czysty
przebieg cutscenki i co najmniej 90 sekund wspólnego gameplayu misji po niej.

## Markery i Mission Failed

- Naciśnij `F7` dokładnie w chwili T-pose, pustej/sekwencyjnej sceny, freeze,
  crashu/disconnectu lub `Mission Failed`.
- Po odzyskaniu sterowania guest ma wsiąść na konia i jechać obok hosta przez
  co najmniej 90 sekund. Jeżeli uruchomi się korekta mission bubble, log guesta
  ma zapisać `teleport guest native applied to mounted root`; nie może po nim
  wystąpić `teleport was overridden by game` ani dystans rzędu setek metrów.
- Jeżeli pokaże się `weather too harsh`, naciśnij F7 natychmiast. Zanotuj, czy
  zdrowie zostało odtworzone przez `LOCAL_HAZARD_GUARD`, czy gra mimo tego
  rozpoczęła prywatny checkpoint reload.
- Podczas spectator/quarantine host nie może widzieć technicznego przesunięcia
  guesta o dokładnie około 180 m. Na wire pozostaje zapisany anchor misji.
- Po ostatnim F7 pozostaw obie gry uruchomione minimum 15 sekund.
- Jeśli gra jednej osoby się zamknie, nie kasuj logów i nie restartuj od razu
  drugiej strony; odczekaj 15 sekund, potem wyeksportuj oba ZIP-y.
- Po cutscence guest podchodzi do własnego żółtego markera. Kryterium blokady to
  brak uruchomienia jego lokalnej misji, nawet jeśli RDR2 nie narysuje szarej
  kłódki.

## Co wysłać

Prześlij cztery archiwa bez ręcznego wycinania zawartości. Każdy eksport zawiera
wyłącznie dwie najnowsze sesje wskazane w `DIAGNOSTICS_SESSIONS.json`:

```text
CutsceneMissionV31.10-OFF-HOST.zip
CutsceneMissionV31.10-OFF-GUEST.zip
CutsceneMissionV31.10-ON-HOST.zip
CutsceneMissionV31.10-ON-GUEST.zip
```

Napisz też przy którym F7 był: cutscene, skip, freeze albo Mission Failed.
Najważniejsza różnica to obecność `CAPTURED` + `CAPTURE_MAP` +
`NATIVE_CREATE/NATIVE_START` w przebiegu ON oraz brak sekwencyjnej lokalnej sceny
guesta w obu przebiegach.
