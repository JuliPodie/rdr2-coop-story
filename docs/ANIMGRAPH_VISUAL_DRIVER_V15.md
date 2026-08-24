# AnimGraph Visual Driver V15 — test T-Pose

Pierwszy test V14 potwierdził, że bezpośrednia replikacja korzenia utrzymuje
marker i postać w tym samym miejscu. Ujawnił jednocześnie T-Pose przez około
99% czasu. Samo `FORCE_PED_MOTION_STATE` zwracało sukces, ale taskless ped nie
miał działającego kontrolera lokomocji, a korekcja współrzędnych dodatkowo
kasowała taski i IK.

V15 zachowuje precyzyjny, bezpośredni korzeń i dodaje osobny sterownik wizualny:

- idle używa długiego `TASK_STAND_STILL`;
- chód, bieg i sprint używają długiego `TASK_GO_STRAIGHT_TO_COORD` w kierunku
  ruchu, bez navmeshu i bez wyboru trasy;
- zadanie jest odnawiane po zmianie gait, większym skręcie albo po 2,5 sekundy;
- każda korekcja pozycji zachowuje aktywny task oraz IK;
- pozycja sieciowa nadal ma pełny priorytet i task nie może powodować narastania
  błędu ani powrotu do starych checkpointów.

Diagnostyka `v15/direct-root-visual-driver/5s` zapisuje:

- `expected-moving-ticks` — klatki, w których strumień wymaga ruchu;
- `observed-moving-ticks` — klatki, w których RDR2 faktycznie zgłasza aktywną
  lokomocję;
- `missing-locomotion-ticks` — oczekiwany ruch bez działającego grafu;
- `visual-task-starts` i `visual-task-refreshes` — częstotliwość odnawiania
  sterownika wizualnego.

## Test solo

1. W launcherze otwórz `Ustawienia` i zaznacz `ANIMGRAPH REPLICA`.
2. Uruchom `Test solo`, wczytaj Story Mode i otwórz F9.
3. Nagraj 20–40 sekund Ghost Record: stanie, chód, bieg, sprint i ostre skręty.
4. Zatrzymaj nagranie i uruchom Ghost Replay.
5. Oceń osobno pozycję względem markera oraz występowanie T-Pose/migotania.
6. Wyeksportuj `RDR2-Coop-Diagnostics.zip`.

To nadal nie jest pełny reader wewnętrznych clipów, faz, warstw i IK AnimGraphu.
V15 bezpiecznie uruchamia natywną lokomocję na replice, pozostawiając protokół
gotowy na późniejsze, zweryfikowane dane MoveNetwork dla builda 1491.50.
