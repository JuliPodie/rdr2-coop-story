# Test cutscenki V28 — protokół 17

Test wykonujcie na obu komputerach wyłącznie w Story Mode i na tej samej
paczce `CinematicKeyframeV28.0`. Starszy build ma zostać odrzucony podczas
handshake.

## Przed sceną

1. Host i guest czekają na `REMOTE STREAMING`.
2. Guest sprawdza dwa pobliskie markery misji. Powinny być szare/zablokowane;
   nie wolno uruchamiać lokalnej kopii misji.
3. Na obu komputerach wybierzcie F9 → `Zapisz znacznik problemu`.
4. Tylko host wchodzi w żółty marker misji.

## Oczekiwany przebieg kamery

- Guest powinien na krótko wygasić ekran, a następnie zobaczyć kadr hosta.
- Nie powinien pojawić się widok z trzeciej osoby śledzący plecy Arthura.
- Cięcia, FOV oraz obrót powinny zmieniać się w tej samej chwili co u hosta.
- Jedna utracona klatka sieciowa nie może przełączyć kamery na follow-camera.
- Skip wymaga głosu hosta i guesta w ciągu pięciu sekund.
- Po scenie guest wraca obok hosta; HUD/NPC nie mogą migać co około 0,8 s.

## Aktorzy sceny

V28 synchronizuje pozycję i heading hostowych aktorów w osobnym kanale 30 Hz.
Nie oczekujcie jeszcze identycznych klipów AnimScene, dialogu, napisów ani audio
u guesta — brak tych elementów należy opisać, ale nie oznaczać jako regresji
kamery. Zaznaczcie natomiast każdy T-pose, `stand still`, teleport większy niż
około metr albo brak ważnego aktora.

## Po scenie

1. Sprawdźcie chód, kucanie, Q/osłonę i celowanie zza osłony.
2. Jeżeli coś się rozjedzie, zapiszcie kolejny marker na obu komputerach i
   testujcie jeszcze 10–15 sekund.
3. Zatrzymajcie sesję i wyeksportujcie dwa nierozpakowane ZIP-y:
   `CinematicKeyframeV28.0-HOST.zip` oraz
   `CinematicKeyframeV28.0-GUEST.zip`.

Najważniejsze wpisy w logu guesta to `MISSION_CAMERA][RX]` z polem `source`,
`MISSION_SPECTATOR`, `MISSION_CINEMATIC`, `ENTITY_GRAPH_GUEST` oraz
`LOCAL_HAZARD_GUARD`. Bez logu guesta można potwierdzić wysyłanie hosta, ale nie
da się potwierdzić, czy RDR2 guesta faktycznie renderował kamerę.
