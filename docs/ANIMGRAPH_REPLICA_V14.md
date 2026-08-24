# AnimGraph Replica V14 — test porównawczy

`ANIMGRAPH REPLICA` jest drugim, eksperymentalnym silnikiem ruchu. Nie zastępuje
dotychczasowego Task/Navmesh i domyślnie pozostaje wyłączony.

## Co robi V14.0

- bezpośrednio odwzorowuje interpolowaną pozycję i obrót zdalnego gracza;
- nie uruchamia tasków chodzenia, pathfindingu ani awaryjnego navmeshu;
- przesyła osobną próbką stan idle/chód/bieg/sprint;
- przy aktywnym skryptowym MoveNetwork kończy niepewną próbkę fail-closed;
- zachowuje osobny licznik sekwencji, epokę lokomocji i rygorystyczną walidację;
- zapisuje pięciosekundowe pomiary błędu przed korektą oraz źródło próbek.

V14.0 nie odczytuje jeszcze identyfikatora/stanu MoveNetwork, pełnej listy clipów,
warstw, ich faz i wag, IK ani root motion wewnętrznego grafu. Pola protokołu są
na to przygotowane, ale pozostają nieważne, dopóki nie powstanie potwierdzony
reader dokładnie dla RDR2 1491.50.

## Jak przetestować solo

1. Uruchom `URUCHOM_COOP.bat`.
2. Wejdź w `Ustawienia` i zaznacz `ANIMGRAPH REPLICA`.
3. Wróć do `Test solo` i kliknij `START`.
4. Po wczytaniu Story Mode otwórz F9.
5. Nagraj trasę przez `Ghost Record: start / stop`: spacer, sprint, ostre skręty,
   schody, płot, celowanie i strzał.
6. Wróć w pobliże początku i uruchom `Ghost Replay: start / stop`.
7. Powtórz test po odznaczeniu opcji, aby porównać Task/Navmesh.
8. Wyeksportuj jeden `RDR2-Coop-Diagnostics.zip` po każdym wariancie.

W teście dwóch komputerów host i klient muszą wybrać ten sam silnik. W przeciwnym
razie sidecar zapisze `network.motion-mode.mismatch` i odrzuci niezgodny strumień
AnimGraph zamiast mieszać dwa kontrolery.
