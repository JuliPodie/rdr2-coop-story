# AnimGraph Traversal V16 — skok, climb i spadanie

V16 zachowuje bez zmian potwierdzony visual locomotion driver V15. Nowy kod
dotyczy wyłącznie krótkich stanów fizycznych.

- `PlayerTraversal` nadal dociera niezawodnie przez kanał kontrolny.
- Odbiorca czeka na pasujący action ID albo dojście na 1,25 m od nagranej
  kotwicy i dopiero wtedy uruchamia natywny `TASK_JUMP` lub `TASK_CLIMB`.
- Przez krótki guard direct-root nie nadpisuje fizyki skoku/wspinania.
- Po zakończeniu natywnej animacji sprawdzony gait driver V15 uruchamia się
  ponownie i odzyskuje dokładną pozycję sieciową.
- Przejście do `Airborne` bez transakcji, np. zejście z krawędzi, jednorazowo
  ustawia autorytatywną pozycję i velocity, aby uruchomić natywny falling.

Nowe pola diagnostyczne w `v16/direct-root-visual-traversal/5s`:

- `traversal-expected-ticks`, `traversal-observed-ticks` i
  `traversal-missing-ticks`;
- `jump-task-starts`, `climb-task-starts` i `airborne-launches`;
- `physical-root-yield-ticks`, `traversal-expired` oraz `traversal-pending`.

## Test

Nagraj Ghost Record zawierający osobno: zwykły skok, przeskok przez niski płot,
wspinanie na przeszkodę i zejście/spadnięcie z niewysokiej krawędzi. W replayu
sprawdź animację, pozycję względem czerwonego markera i powrót do płynnego biegu
po lądowaniu. Następnie wyeksportuj jeden ZIP diagnostyczny.
