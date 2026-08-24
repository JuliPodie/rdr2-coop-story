# Test synchronizacji cutscenek — protokół 16

Ten test wymaga dwóch PC z identycznym buildem. Host używa save'a kampanii,
a guest spokojnego save'a poza misją i cutscenką. Tylko host wchodzi w marker
misji; guest nie uruchamia lokalnego markera ani `AnimScene`.

## Przebieg podstawowy

1. Połącz sesję i poczekaj na `REMOTE STREAMING` po obu stronach.
2. Host uruchamia misję z normalną cutscenką. Guest powinien wygasić ekran,
   zostać ukryty i stracić sterowanie, a potem zobaczyć kamerę hosta.
3. W scenie z loadingiem sprawdź, że spectator nie wyłącza się między fazami.
   Łańcuch cutscenka–loading–cutscenka ma zachować tę samą generację.
4. Po odzyskaniu sterowania przez hosta guest pozostaje na czarnym ekranie do
   przygotowania kolizji i wraca obok hosta. Postać i koń muszą używać osobnych
   offsetów, bez nakładania się.
5. Po powrocie obaj gracze chodzą, walczą i kontynuują tę samą sesję bez STOP,
   ponownego JOIN ani restartu gry.

## Kamera i awarie transportu

- Odłącz lub ogranicz UDP podczas sceny. Po 1000 ms guest ma przejść na kamerę
  śledzącą hosta bez odzyskania sterowania i bez wyjścia ze spectator mode.
- Przywróć UDP. Kamera może wrócić tylko dla bieżącej generacji; opóźniony
  snapshot poprzedniej sceny nie może zmienić widoku.
- Zerwij TCP w fazach `Playing`, `Loading` i `PrepareResume`. Po braku stanu
  hosta przez 3 s guest musi wykonać teardown: widoczny ped/mount, kolizja,
  sterowanie i kamera gry.
- Po reconnect sprawdź kolejność odbudowy: misja, FSM cutscenki, graf świata.
- Powtórz STOP/unload podczas każdej fazy i dwa razy z rzędu. Teardown ma być
  idempotentny i nie może pozostawić focus/HD-area ani ukrytej postaci.

## Skip

- Najpierw naciśnij skip tylko na jednym komputerze i odczekaj ponad 5 s.
  Scena nie może zostać pominięta.
- Następnie host i guest naciskają standardowy skip w odstępie do 5 s.
- Powtórz przez F9 → `Głosuj: pomiń cutscenkę` na obu komputerach.
- Guest wysyła `SkipRequest`; dopiero konsensus 2/2 pozwala hostowi podtrzymać
  standardowe wejście najwyżej 2,5 s. `MissionState` nie może zakończyć się
  tylko wskutek pojedynczego żądania.
- Na niepomijalnej scenie wejście wygasa, scena trwa, a po prawdziwym końcu
  silnika oba komputery nadal wykonują zwykły handshake powrotu.
- Powtórz skip w dwóch kolejnych generacjach. Stary lub zduplikowany request
  nie może wpłynąć na nową scenę.

## Kryteria zaliczenia

Test jest zaliczony, gdy:

- guest ani razu nie uruchamia własnej misji lub lokalnego `AnimScene`;
- stara kamera nie przechodzi między generacjami;
- scena z loadingiem nie powoduje chwilowego powrotu do coopa;
- po normalnym końcu i poprawnym `ResumeReady` obaj odzyskują sterowanie bez
  powrotu do `Playing`; utrata hosta nadal uruchamia teardown po 3 s;
- po scenie NPC, minimapa i kamera nie migoczą w cyklu około 0,8 s;
- żadna postać ani koń nie pozostaje niewidzialny, zamrożony lub bez kolizji;
- po scenie można dalej grać razem bez restartu i ponownego JOIN.

Po każdym problemie obie osoby wybierają F9 → `Zapisz znacznik problemu`, grają
jeszcze 10–15 s i eksportują diagnostykę po zatrzymaniu sesji.
