# RDR2 Coop Story — test solo i Intent Route V12.0

Test uruchamia prawdziwy sidecar hosta i syntetycznego guesta przez loopback
`127.0.0.1`. Snapshoty przechodzą przez ten sam protokół, UDP, Named Pipe i
bridge co w teście dwóch komputerów. Nie sprawdza prawdziwego pingu ani wejścia
z klawiatury drugiej osoby.

## Ghost Record

`Ghost Record: start / stop` w menu F9 zapisuje przychodzace do syntetycznego
guesta stany prawdziwego lokalnego gracza z czestotliwoscia okolo 20 Hz.
`Ghost Replay: start / stop` odtwarza je jeden raz w oryginalnym czasie i w
dokladnych wspolrzednych swiata, wysylajac je z powrotem przez zwykla sciezke
guest -> UDP/TCP -> host sidecar -> Named Pipe -> bridge. Nick repliki to
`GHOST REPLAY`, a marker `CEL SIECI` pokazuje pozycje zapisana w nagraniu.

V12.0 używa semantycznej trasy zamiast przedłużać sam wektor velocity. Punkty
przed puppetem są wybierane według chodu i krzywizny, a postęp nie może nagle
przeskoczyć na bliski przestrzennie, lecz odległy fragment pętli. Zarejestrowany
skok lub climb ma własny ID, epokę, punkt startu i heading; wygasa po 3 sekundach.
Zwykły ruch po ziemi nie jest już popychany przez `SET_ENTITY_VELOCITY`, a
`move rate override` nie przekracza 1,15. Bufor używa czasu nadawcy, mierzy
jitter i dobiera opóźnienie interpolacji w zakresie około 75–160 ms.

Zalecany test: nagraj pieszo 20–60 sekund ruchu przez drzwi budynku, wyjscie,
skok, zmiane kierunku, celowanie i pojedynczy strzal; zatrzymaj zapis, wroc do
miejsca startu i uruchom replay. Nagranie jest jednorazowe, ma limit 10 minut
oraz 12 000 klatek i trafia do diagnostyki jako `recordings/ghost-last.json`.

## Najprostsze uruchomienie z launchera

1. Zamknij RDR2 i stare okna Coop Story.
2. Rozpakuj najnowszy ZIP do nowego folderu i uruchom `URUCHOM_COOP.bat`.
3. Wskaż `RDR2.exe`, osobno rozpakowany Script Hook RDR2 i wybierz platformę
   Steam albo Rockstar.
4. Na ekranie głównym wybierz tryb `Test solo`, platformę Steam albo Rockstar,
   a następnie jeden okrągły przycisk `START`. Launcher sam sprawdzi instalację,
   w razie potrzeby ją zaktualizuje, uruchomi sidecar testowy i wystartuje grę.
5. Wejdź wyłącznie do Story Mode i wczytaj dowolny spokojny save.
6. Otwórz `F9`, przejdź na `Test solo: start / stop` i naciśnij Enter.
7. Stań na płaskim, otwartym terenie i obserwuj pełną pętlę przez co najmniej
   75–120 sekund.

Bot powinien być widocznym pedem około ośmiu metrów od Arthura. Nad nim
powinien pojawić się stabilny znacznik `SOLO BOT`, a na minimapie przyjazny blip.
Pętla trwa 36 sekund: idle, walk, run, sprint, stop-and-turn, reverse-run oraz
walk-return. Ponowne wybranie tej samej pozycji F9 zatrzymuje stream; bot znika
po około pięciu sekundach.

V10.3 wykrywa utknięcie na ścianie, zachowuje świeży ślad punktów celu i przełącza
puppeta na navmesh RDR2. Niebieski `CEL NAVMESH` pokazuje aktualny punkt śladu,
a `CEL SIECI` nadal pokazuje prawdziwą pozycję otrzymaną przez bridge. Podczas
navmesh bezpośredni velocity catch-up jest wyłączony, aby nie pchał postaci w
przeszkodę. Ślad ma maksymalnie 64 punkty i 12 sekund; po dogonieniu jest
kasowany, a od 15 m różnicy navmesh prowadzi prosto do aktualnego markera.
Pętla akcji zawiera także krótki skok po 36. sekundzie.

V9.5 inicjalizuje outfit/MetaPed natywem wymaganym przez `CREATE_PED`, wymusza
widoczność oraz pełną alfę. Strój bota jest jednak tylko strojem technicznym —
pełna sieciowa synchronizacja wyglądu drugiego gracza jest osobnym etapem.
Punkt prowadzenia tasku ma najwyżej 2 m zamiast 12 m. Jeżeli gra odsunie puppeta
od celu (na przykład po uwolnieniu z lassa), bot nadrabia dystans chodem,
biegiem albo sprintem bez zmiany współrzędnych. V9.5 co klatkę ponawia zarówno
`move rate override`, jak i maksymalny blend sprintu, lecz narastanie prędkości
i tempa animacji jest ograniczone w czasie. Ostatnie polecenie velocity pozostaje
w regulatorze nawet wtedy, gdy gra wyzeruje własny odczyt. Przeładowanie nie
wyłącza doganiania pozycji. Podczas ragdolla/lassa replikacja ekwipunku jest
wstrzymana, a brakującą broń można nadać najwyżej raz na pięć sekund.

Krzyż `CEL SIECI` pokazuje dokładną pozycję otrzymaną przez bridge. Kropki łączą
go z widocznym puppetem, a etykieta podaje różnicę: zielony oznacza do 0,5 m,
żółty 0,5–2 m, czerwony ponad 2 m. Marker istnieje wyłącznie dla syntetycznego
testu i nie pojawi się w prawdziwej sesji dwóch graczy.

Sprzężona pętla akcji trwa 60 sekund. Bot otrzymuje Cattleman Revolver oraz
120 nabojów. Od 10. sekundy celuje i oddaje po jednym zerowym obrażeniowo
strzale kolejno: przód, góra, dół, lewo, prawo i 180 stopni do tyłu. Między
30. a 34. sekundą wykonuje test przeładowania. Na czas tej pełnej animacji
marker i zegar trasy zatrzymują się razem z puppetem, po czym wznawiają ruch
bez przeskoku. Ponieważ pętla ruchu trwa
36 sekund, kolejne kierunki wypadają podczas różnych gaitów i sprawdzają także
celowanie w ruchu.

W V9.5 marker ma pierwszeństwo przed kierunkiem celowania. Jeżeli bot oddali się
na co najmniej 1,25 m, prosty task ruchu i regulator velocity prowadzą go do
markera, a akcja broni nie może już skierować nóg w przeciwną stronę. Wspólny
task ruchu/celowania wraca dopiero poniżej 0,5 m. Najważniejszy fragment próby
to przejście `stop-and-turn → reverse-run` podczas sekwencji celowania.

V9.5 nie czeka już, aż cel tasku oddali się o 3 m: odświeża go po 0,5 m, z
limitem około dwóch zmian na sekundę. Przy różnicy mniejszej niż 1,5 m nie
nadpisuje fizycznej velocity, więc bot powinien iść ciągle zamiast wykonywać
krótkie sprinty. Po związaniu lassem marker nadal wykonuje kurs testowy; ciało
może być ciągnięte w jego stronę korektą X/Y/Z, lecz task chodzenia pozostaje
wyłączony aż do zakończenia ragdolla/wstawania.

## Co obserwować

- czy widać całe ciało bota, a nie tylko ślady w śniegu;
- czy `SOLO BOT` pozostaje przywiązany do postaci;
- czy chód, bieg i sprint przełączają się bez długiego T-pose;
- czy nie występuje dawny skok/teleport do znacznika;
- czy po zatrzymaniu postać nie ślizga się dalej;
- czy obrót i reverse-run nie kasują animacji na długo.
- czy podczas reverse-run bot i marker zawsze przemieszczają się w tym samym
  kierunku, także gdy rewolwer celuje w bok albo do tyłu;
- czy po krótkim związaniu lassem bot płynnie dogania kurs bez teleportu.
- czy marker przechodzi z czerwonego przez żółty do zielonego podczas doganiania;
- czy podczas zwykłego chodu bot nie zatrzymuje się co około metr i nie nadrabia
  krótkim sprintem;
- czy po krótkim lasso różnica X/Y/Z zaczyna maleć jeszcze przed pełnym wstaniem;
- czy bot ma rewolwer, celuje w sześciu kierunkach i oddaje dokładnie jeden
  strzał na kierunek;
- czy każdy strzał puppeta jest słyszalny przestrzennie z miejsca postaci;
- czy strzały nie odbierają zdrowia graczowi ani NPC;
- czy przeładowanie kończy się i nie blokuje dalszego ruchu.

## Po teście

1. Zamknij RDR2.
2. W launcherze kliknij `Zatrzymaj sesję`.
3. Kliknij `Eksportuj diagnostykę` i zapisz ZIP.
4. W ZIP-ie najpierw otwórz `DIAGNOSTICS_INDEX.txt`; pełne logi są w `logs/`.
5. Przed wejściem do Red Dead Online kliknij `Odinstaluj`.

Najważniejsze wpisy bridge'a mają teraz kategorie:

```text
[INFO][PUPPET_SPAWN] ... outfit initialized, visible=1, alpha=255
[INFO][PLAYER_IDENTITY] nickname=SOLO BOT, stable-nameplate=1, ...
[INFO][PUPPET_MOTION] v10.3/fresh-trail-navmesh/5s: ... destination-heading-ticks=..., physics-assist-active=..., physical-interruption-ticks=..., coordinate-corrections=0, solo-marker=1
[INFO][PUPPET_NAV] v10.3/fresh-trail-navmesh/5s: ... queue=..., waypoint-expired=..., trail-resets=..., direct-target-selections=...
[INFO][PUPPET_ACTION] v10.3/fresh-trail-navmesh/5s: ... visual-zero-damage-shots=..., spatial-audio-shots=..., jump-task-starts=..., climb-task-starts=...
[INFO][PUPPET_DESPAWN] completed without native gamer-tag calls
```

Jeżeli `visible=0`, `alpha=0`, `coordinate-corrections` jest inne niż zero albo pojawi się
`[WARN][PUPPET_SPAWN]`, wyślij cały ZIP diagnostyczny. Eksporter zbiera do
32 MB końcówki każdego logu i automatycznie sortuje najważniejsze wpisy.

Paczka moda nie zawiera `ScriptHookRDR2.dll`, `dinput8.dll`, trainera ani SDK.
Script Hook każdy użytkownik pobiera osobno od autora.
