# Test 2PC V26.0 — cutscenka, crouch, cover i fizyka graczy

Oba komputery muszą mieć dokładnie ten sam ZIP V26.0. Przed instalacją należy
zamknąć RDR2 i poprzedni launcher. Script Hook RDR2 pozostaje osobnym wymaganiem
i nie znajduje się w paczce.

## Przygotowanie

1. Host i guest uruchamiają launcher, wybierają właściwą platformę i wykonują
   `Sprawdź instalację`.
2. Obaj ładują Story Mode i dopiero po pojawieniu się postaci tworzą/dołączają
   do sesji.
3. Przed misją wykonują krótki test ruchu i zapisują marker F9, jeśli proxy,
   nick albo koń zacznie się rozjeżdżać.

## Test ruchu i fizyki

1. Każdy gracz przez 10 sekund idzie i biegnie w kuckach. Druga strona ocenia,
   czy crouch jest ciągły i czy `observed` w diagnostyce nie pozostaje zerem.
2. Każdy dwa razy wchodzi pod osłonę Q, pozostaje 5 sekund i wychyla się.
   Niedozwolona jest pętla `cover → stand → cover` co około sekundę.
3. Host i guest kolejno wykonują: jeden cios, blok, grapple/powalenie oraz
   lasso. Po powaleniu należy odczekać co najmniej 3 sekundy. Obie strony
   zapisują marker przy pierwszej różnicy.
4. Osoba związana czeka 5 sekund, następnie druga wysyła uwolnienie. Proxy nie
   może wstać przed niezawodnym stanem `Free`.

## Test cutscenki

1. Guest nie wchodzi pierwszy w lokalny żółty marker. Host uruchamia pierwszą
   misję, a guest pozostaje w pobliżu, aby sprawdzić automatyczną izolację.
2. Podczas sceny guest sprawdza: kamerę, hostowych aktorów/NPC, brak własnych
   duplikatów oraz brak widocznego proxy graczy przeszkadzającego scenie.
3. Po końcu sceny oba ekrany mogą na krótko pozostać czarne. Host nie powinien
   odzyskać sterowania wcześniej niż guest. Po `ResumeReady` obaj wracają obok
   hostowego anchoru.
4. Jeżeli czarny ekran trwa ponad 30 sekund, nie wyłączać od razu gry: zapisać
   marker F9, zatrzymać sesję i wyeksportować diagnostykę z obu komputerów.
5. Jeżeli wystąpi `[FATAL][SEH]`, nie kontynuować checkpointu w tym samym
   procesie RDR2. Zamknąć grę, wyeksportować diagnostykę launcherem i uruchomić
   RDR2 ponownie, aby nie pozostawić starego świata po zatrzymanym ASI.

## Pliki do przekazania

Przekazać dwa niezmienione pliki `RDR2-Coop-Diagnostics.zip`, oznaczone jako
HOST i GUEST, oraz krótką kolejność markerów. Nie trzeba kończyć testu po każdym
problemie, chyba że wystąpi fatalny błąd natywny, czarny ekran bez odzyskania
sterowania albo nieodwracalny softlock misji.
