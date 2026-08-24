# RDR2 Coop Story — architektura

## Cel i status

Projekt zapewnia prywatny coop dla dwóch graczy w PC Story Mode. Host jest jedynym autorytetem kampanii, save’a, NPC, celów oraz ostatecznych wyników walki. Guest jest dodatkowym, niemisyjnym bohaterem.

Architektura jest celowo uniwersalna. Nie tworzymy adapterów dla pojedynczych misji. Fragment, którego nie da się bezpiecznie przeprowadzić w coop, host kończy przez `Solo override`, a guest ogląda go jako spectator.

Pełna zgodność kampanii pozostaje hipotezą do zweryfikowania. Pierwszym mierzalnym etapem jest syntetyczny peer na jednym PC; końcowe potwierdzenie wymaga dwóch komputerów i przejścia kampanii.

## Granice komponentów

### `CoopStoryBridge.asi`

Natywny bridge C++20 x64 działa w procesie gry i ma możliwie mały zakres:

- odczyt lokalnego stanu gracza i świata;
- wykonywanie poleceń sidecara na wątku skryptowym gry;
- spawn i sterowanie replikami pedów/koni;
- kamera spectator, menu F9, downed/revive;
- bezwarunkowy guard Story Mode/RDO;
- telemetryka techniczna bez sekretów, IP i pointerów.

Bridge preferuje oficjalne nativy ScriptHooka. Hook pamięci jest dopuszczalny dopiero wtedy, gdy native-only PoC dowiedzie, że nie da się niezawodnie przechwycić śmierci hosta. Taki hook musi być ograniczony do jednego hasha `RDR2.exe` i domyślnie wyłączony dla każdego innego builda.

### `CoopStory.Sidecar.exe`

Sidecar C# `net10.0-windows` działa poza katalogiem gry:

- zarządza sesją Host/Guest i handshake’iem;
- obsługuje TCP, UDP, reconnect i heartbeat;
- mapuje stabilne `NetEntityId`;
- przechowuje profil guesta i wykonuje atomowe zapisy JSON;
- buforuje snapshoty i przekazuje bridge’owi wyłącznie dane domenowe;
- udostępnia syntetycznego peera do testów bez drugiego PC.

Sidecar nie dostaje pointerów ani lokalnych uchwytów RDR2. Nie wstrzykuje kodu do gry.

### `CoopStory.Launcher.exe`

Launcher jest właścicielem konfiguracji i uruchomienia sesji. HOST wybiera swój
IPv4, ustawia hasło po kliknięciu `HOSTUJ`, a GUEST podaje IPv4 hosta oraz to samo
hasło w małym oknie dołączania. Lokalny save hosta wybiera się w `Ustawieniach`.
Po starcie launcher pokazuje nicki, role oznaczone
osobnymi kolorami, stan połączenia i bezpieczny pomiar ICMP w ms. Dane lobby
pochodzą ze statusu sidecara na lokalnym stdout; są lokalnym kanałem UI i nie
dodają wiadomości do protokołu 20.

Hasło nie jest zapisywane jawnie. PBKDF2-HMAC-SHA256 wyprowadza z hasła oraz
kanonicznego IPv4 wewnętrzny identyfikator sesji i 256-bitowy sekret HMAC w
formacie używanym przez protokół 20. Obie strony muszą podać identyczne
hasło o długości 4–64 znaków; zmiana hasła tworzy inne poświadczenie.

Kompletna konfiguracja z launchera uruchamia sieć od razu. Panel F8 w grze jest
domyślnie zamkniętym narzędziem awaryjnym, a nie podstawową ścieżką HOST/JOIN.

### Named Pipe IPC

IPC jest dwukierunkowym Named Pipe ograniczonym do bieżącego użytkownika. Ramka binarna zawiera:

- magic i wersję protokołu;
- typ wiadomości;
- sequence i tick;
- długość payloadu;
- payload o ograniczonym rozmiarze.

Parser odrzuca złą wersję, nieznany typ, przepełnienie długości, niepełną ramkę i duplikat poza dozwolonym oknem. Zamknięcie sidecara nie może zawiesić wątku gry; bridge wraca do stanu rozłączonego i usuwa/ukrywa repliki w kontrolowany sposób.

## Sieć i autorytet

- TCP `43120`: handshake, zdarzenia niezawodne, checkpoint, resync i kontrola sesji.
- UDP `43121`: snapshoty ruchu i szybko zmieniający się stan.
- Prywatny LAN albo Hamachi IPv4 `25/8` w protokole 20; brak IPv6, NAT traversal,
  relaya i automatycznego otwierania portów.
- Każdy datagram UDP zawiera skrócony HMAC oraz uwierzytelnione `InstanceId`
  procesu z bieżącego handshake TCP. Odbiorca wymaga też sekwencji nowszej od
  ramki handshake, zanim przypnie port źródłowy; stary socket nie może więc
  przejąć bindingu po reconnect.
- Transform gracza: 20 Hz.
- Bufor interpolacji guesta: 100 ms.
- Broń, koń, animacja i zdrowie: na zmianę stanu lub z niższą częstotliwością.
- Uporządkowany zestaw komponentów MetaPed: niezawodny snapshot co 2 s.
- Sygnatura aktywnej AnimScene hosta: snapshot UDP do 20 Hz, bez lokalnego
  uchwytu sceny.
- Kanoniczna definicja zasobu/odtwarzania/ról AnimScene i handshake startu:
  niezawodny TCP/pipe, FIFO, bez coalescingu i bez UDP.

Host nadaje stabilne `NetEntityId` i utrzymuje mapę do lokalnych uchwytów. Uchwyt nigdy nie trafia na sieć. Klient tworzy lokalną replikę i interpoluje transform między potwierdzonymi snapshotami.

AI działa u hosta. Klient może pokazać przewidywane FX trafienia, lecz host rozstrzyga obrażenia, zdrowie i śmierć. Friendly fire jest wyłączony. Resync po reconnect odtwarza `MissionState`, `MissionCinematicState`, a następnie rejestr encji, pozycje i ekwipunek; zakładany cel to powrót do aktywnej sesji w ciągu 10 sekund.

### AnimGraph Replica V30

Eksperymentalny silnik nie kopiuje kości i nie zastępuje wewnętrznego AnimGraphu
RAGE. `PlayerState` dostarcza autorytatywny transform, desired blend, heading
ruchu oraz prędkość w lokalnych osiach peda. V30 przelicza te osie zgodnie z
natywną konwencją RDR2, rozpoznaje osiem kierunków i utrzymuje lokalny task
wizualny bez navmeshu. Stojący ped wykonuje natywny obrót w miejscu, a arm/head/
leg/torso IK pozostają włączone dla lokalnego podłoża i broni.

V30.2 dodaje obserwacyjne watchdogy dla crouch i cover. Przejście stealth→cover
przekazuje własność wspólnej gałęzi crouch bez pośredniego `FALSE`; utracony stan
jest ponawiany dopiero po zmierzonym oknie i z cooldownem. Strzał stojącej repliki
używa krótkiego natywnego tasku grafu broni przy wyzerowanym magazynku. Raport,
FX i pocisk pozostają osobną prezentacją z obrażeniami 0, a amunicja proxy wraca
po zakończeniu tasku. Host nadal jest jedynym właścicielem obrażeń.

Transakcje akcji mają wspólny znormalizowany zegar, ale nie jest on fazą klipu.
Pola clip ID, warstw, playback rate i exact phase pozostają kanonicznie zerowe,
dopóki wersjonowany reader dla wspieranego hasha gry nie potwierdzi ich źródła.
Analogicznie kod nie wymyśla nazw move-networków dla tackle, hogtie, mount,
lootowania ani lipsyncu. Szczegółowa macierz znajduje się w
`docs/ANIMGRAPH_REPLICA_V30.md`.

## Model kampanii

- Tylko host inicjuje markery, dialogi, cele i przedmioty fabularne.
- Save hosta jest jedynym źródłem postępu kampanii.
- Guest walczy, podróżuje, używa wyposażenia i konia, ale nie posiada fabularnych triggerów.
- Free-roam nie ma twardej smyczy.
- W aktywnej misji guest dostaje ostrzeżenie od 200 m. Po 250 m rdzeń stosuje skonfigurowany fallback: teleport na bezpieczne podłoże albo spectator.
- Cutscenkę i mission VM uruchamia wyłącznie host. Guest najpierw próbuje
  bezpiecznie dopasować już istniejącą lokalną `AnimScene` po hash dictionary
  oraz czasie trwania i koryguje jej origin/fazę/rate. Tylko wtedy lokalna scena
  dostarcza oryginalną kamerę, aktorów, rozmowy, napisy i audio. Brak dopasowania
  automatycznie zachowuje odwracalną kamerę hosta/follow-camera.
- F9 dzieli częste akcje sesji/ratunku od narzędzi testowych i udostępnia m.in.
  `Głosuj: pomiń cutscenkę`, `Solo override`, teleport guesta, resync
  encji/ekwipunku, retry i diagnostykę. `F7` zapisuje korelowany `USER_MARKER`
  bez otwierania menu. Marker jest symetrycznym, uwierzytelnionym poleceniem
  diagnostycznym bez uprawnień gameplayowych: druga strona zapisuje ten sam
  identyfikator, a oba bridge'e uruchamiają ograniczone 15-sekundowe próbkowanie.
  Eksporter buduje dla niego osobne okno 10 sekund przed i 15 sekund po zdarzeniu.
- `Solo override` zapisuje stan guesta, ukrywa jego proxy i przełącza kamerę na hosta. Po zakończeniu przywraca guesta obok hosta na zweryfikowanym podłożu.

Nie ma kodu rozpoznającego konkretną nazwę lub ID misji. Powtarzalny problem naprawiamy globalnie; problem unikalny dla misji dokumentujemy jako moment użycia F9.

### Blokada lokalnych misji i cel guesta

Guest utrzymuje odwracalną dzierżawę izolacji Story przez całą sesję. V31.7 nie
zapisuje procesu-globalnego `MISSION_FLAG` w swobodnej grze, ponieważ RDR2 używa
go także do dostępności zwykłych rozmów i promptów konia. Od pierwszego
autorytatywnego `MissionState` hosta do terminala flagę utrzymuje jednak jako
zajętą, aby spóźniona lokalna Story VM nie wystartowała po cutscence i nie
utworzyła prywatnego `Gang Abandoned`. Straż przed startem wymaga jednocześnie: lokalnej encji
mission-owned, człowieka bez możliwości dosiadania, braku sieciowej tożsamości
oraz rzeczywistego blipa przypiętego do aktora. Dopiero wtedy w promieniu 20 m
wyłącza przyciski startu i rysuje szary komunikat. Zwykłe NPC rozmowy, konie,
hostowe repliki i obaj gracze pozostają interaktywni. Każde przejęcie zapamiętuje
bezpośrednią wartość do odtworzenia, a STOP przywraca flagę sprzed sesji.

Po wejściu w hostową prezentację niewidoczny lokalny ped guesta jest odwracalnie
stagingowany pionowo poza triggerem Story, podczas gdy streaming focus pozostaje
na kamerze hosta. Po `ResumeReady` wraca do przygotowanej kotwicy hosta. Dzięki
temu lokalna geometria/proximity VM nie może uzbroić prywatnej kopii tej samej
misji tylko dlatego, że zamrożony guest stał wewnątrz żółtego markera.

`PlayerMountState` identyfikuje osobno gracza, konia, slot i generację. Natywna
relacja mount może dotknąć tylko uchwytu, który w obie strony mapuje się na ID
zdalnego gracza; `PLAYER_PED_ID` jest bezwarunkowo odrzucany. Ponieważ kontrolki
RDR2 dla talk/mount i grapple częściowo się nakładają, wejście kontekstowe przy
własnym koniu nie może stworzyć `Knockdown`. Celowe ściągnięcie gracza z siodła
używa istniejącego `VariantValid` z wariantem `MPUL`; tylko taki wariant może
uruchomić lokalny, victim-owned `TaskDismountAnimal`.

`MissionState` nie zawiera lokalizowanego tekstu celu z hostowego mission VM.
Guest dostaje więc jawny panel wspólnego celu fabularnego oraz żółtą kotwicę,
bez podszywania się pod dokładny wiersz typu `Follow gang`. Dodanie dokładnego
tekstu wymaga najpierw zweryfikowanego, stabilnego źródła po stronie hosta.

### Lasso między graczami

Equipment/Aim uzbraja zdalnego peda w prawdziwe `weapon_lasso`. Gdy niezawodna
akcja lassa po raz pierwszy wskazuje drugiego gracza, odbiorca od razu uruchamia
natywne `TASK_LASSO_PED`; silnik RDR2 jest właścicielem wind-upu, rzutu, liny,
złapania i ciągnięcia. Późniejsze `PhysicalTargetEffect` i `RestraintState`
potwierdzają semantykę oraz zatrzymują transform motor, ale nie tworzą drugiej
liny ani zastępczego ragdolla. Jeżeli RDR2 jeszcze nie zwrócił target handle,
V30.3 może wskazać peer tylko wtedy, gdy leży w wąskim korytarzu promienia kamery
i nie istnieje inny cel silnika. Po zwolnieniu fizycznego constraintu sam stary
target-only `Sustain` nie restartuje tasku. Brak złapania jest poprawnym chybieniem.

### FSM cutscenki i powrót do coopa

Host publikuje niezawodne fazy `Playing`, `Loading`, `PrepareResume`,
`Completed` i `Aborted`. Łańcuch cutscenka–loading–cutscenka zachowuje jedną
generację. Dopiero 750 ms stabilnego odzyskania kontroli uruchamia
`PrepareResume`; guest pozostaje wtedy na czarnym ekranie, ładuje kolizję przy
świeżej pozycji hosta i potwierdza `ResumeReady`.

`PrepareResume` jest przejściem jednokierunkowym. Po `Completed` host utrzymuje
terminalny latch do 1500 ms stabilnego gameplayu i ignoruje krótkie ponowne
zgłoszenie kamery przez RDR2. Zapobiega to ponownemu wejściu do `Playing`,
cyklicznemu resetowaniu grafu świata oraz migotaniu HUD-u po scenie.

Kamera jest snapshotem UDP do 30 Hz. Guest najpierw odsyła
`PresentationReady`; do tego czasu generacja pozostaje w `Loading`. Ostatnia
poprawna klatka kamery zachowuje ważność przez 1000 ms, aby pokryć debounce
odzyskania kontroli bez przełączania na follow-camera. Duże zmiany pozycji,
rotacji lub FOV są traktowane jako celowe cięcie filmowe i nie są wygładzane.
Host kończy handshake dopiero po `ResumeReady` aktywnego guesta.
Utrata stanu hosta przez 3 s, STOP, unload i wyjątek korzystają z tego samego
idempotentnego teardownu, który zawsze przywraca sterowanie, widoczność,
kolizję, kamerę oraz czyści focus i HD-area.

### MetaPed i warunkowa prezentacja AnimScene

Bridge odczytuje do 64 aktywnych shop-componentów MetaPed w kolejności, liczy
fingerprint razem z modelem i przesyła tylko stabilne hashe. Odbiorca resetuje i
odtwarza komponenty proxy wyłącznie przy identycznym modelu. Nieznany model lub
niepełny stan nie powoduje częściowego resetu.

Host nie wysyła uchwytu AnimScene. Do transportu wybiera wyłącznie scenę, która
aktualnie posiada aktywną kamerę, po czym wysyła dictionary hash, duration, phase,
rate, origin i liczbę aktywnych kamer. Guest skanuje wyłącznie pulę lokalnych
uchwytów przez `DOES_ANIM_SCENE_EXIST` i podłącza się dopiero po zgodności hash +
duration. Mod nie przejmuje własności i nie usuwa cudzej sceny. Gdy guest jest
z przodu, scena jest krótko pauzowana; gdy jest z tyłu, ograniczony regulator
rate pozwala ją dogonić. `SAFE_FALLBACK` jest ogłaszany dopiero po pełnym skanie
lokalnych uchwytów. Dopasowany, załadowany uchwyt może dostać jedną próbę
ponownego startu po utracie pracy/kamery. Brak zgodnej sceny jest stanem
oczekiwanym, nie błędem. Od V30.3 fallback pokazuje niekolizyjną hostową obsadę
proxy. Korzenie aktorów są wygładzane, duże korekty teleportowane, a lokalny RDR2
dobiera idle lub lokomocję. Usuwa to pusty kadr bez ponownego snapowania NPC 30 Hz,
ale nie odtwarza gestów, lipsyncu ani audio AnimScene. Tylko `ATTACHED` oznacza
pełną authored scenę; `PROXY_CAST_FALLBACK` oznacza ograniczoną prezentację obsady.

### Hybrydowy exact-path V31

Warstwa hybrydowa rozdziela szybki stan fazy od definicji sceny. Snapshot UDP
`AnimSceneReplicaState` może działać z revision `0` jako dotychczasowy fallback.
Kompletna definicja ma niezerową revision, nazwę zasobu, playback listę, flagi
tworzenia i role przypięte do stabilnych `NetEntityId`. Fingerprint SHA-256
uniemożliwia przypięcie snapshotu albo commitu do innej zawartości pod tą samą
revision.

Guest najpierw tworzy bridge-owned scenę i ładuje jej zasób bez dotykania proxy.
Po `resource-loaded` wiąże wszystkie wymagane role. Gracze/NPC/mounty używają
istniejących ID, a przechwycone rekwizyty dostają ograniczone ID obiektowe.
Nierozwiązana wymagana rola odrzuca całą definicję.
Dopiero `GuestReady` pozwala hostowi wysłać `HostPlayCommit` z bieżącą fazą i
rate. Brak zasobu, brak bindingu, różnica modelu albo timeout kończą się
`GuestRejected/HostAbort`. Cold-load guesta jest ograniczony do 8000 ms, a host
przyjmuje odpowiedź przez 10000 ms wraz z zapasem na RTT/jitter. Decyzja po
`GuestReady` nadal ma osobne krótkie okno. Log postępu rozróżnia I/O zasobu i
brak bindingu. Hostowy marker prepare jest wyłącznie logiczny: game-owned handle
nie jest pauzowany, przyspieszany ani usuwany, a Story VM zawsze kontynuuje
własną scenę. Cache
sidecara przeżywa chwilowy reconnect, ale przechowuje wyłącznie zwalidowane dane
wire — nigdy lokalne uchwyty. Po peer reconnect host czeka na uwierzytelniony
`ResyncRequest`, a następnie odtwarza `MissionState`, `MissionCinematicState`,
spawny parent-first i dopiero zgodną `AnimSceneDefinition`.
Stan cinematic, definicja oraz control idą jedną kolejką krytyczną FIFO. Guest po
`Ready` czeka najwyżej 4000 ms na pasujący commit/abort, a host może ponowić
idempotentny commit po ponowionym `Ready`. Resync w aktywnej prezentacji zachowuje
hostowy graf encji i jego `NetEntityId`. Pełny reconnect pipe zachowuje epokę oraz
generację cinematic i odtwarza stabilne spawny parent-first przed definicją;
również timeout strumienia nie unieważnia ID aktywnych ról. Terminal
`Completed/Aborted` czyści przygotowany uchwyt i odrzuca wszystkie opóźnione
decyzje. Jeśli reconnect przypada na `PrepareResume`, hostowa bariera pozostaje
podniesiona przez kolejne próby bez `HelloAck`; zmiana roli jest odrzucana i
wykonuje pełny cleanup zamiast pozostawić trwały freeze.

Sidecar serializuje hostowy snapshot reconnectu z live spawn/despawn/terminal i
wiąże cały batch z jednym `ControlPeerToken`. Wymiana TCP unieważnia zaległe ramki
we wszystkich kolejkach network→bridge. Resync guesta jest potwierdzony dopiero po
pełnym, nieanulowanym zapisie resetu do gotowej generacji named pipe; dopiero potem
wysyłany jest request do hosta. Named pipe ma osobny token generacji, dlatego zapis
oczekujący na stare połączenie nie może zostać skierowany do nowego bridge'a przed
jego `Hello/HelloAck`. Te same granice obejmują autorytatywne lasso, grapple i
pozostałe interakcje graczy.

Ponowne prepare nie używa starego klucza decyzji: host zwiększa
`DefinitionRevision`, ponownie liczy fingerprint i dopiero po skutecznym replayu
misji, cinematic oraz stabilnych spawnów wysyła nową definicję. `GuestReady` sprzed
reconnectu nie pasuje do nowego klucza, nawet jeśli dotrze po świeżym
`PlayerState`. `HelloAck` nie jest jeszcze dowodem powrotu guesta; w
`PrepareResume` bariera czeka na jego pierwszy świeży strumień i późniejsze
`ResumeReady`. Oczekiwanie jest uzbrajane tylko wtedy, gdy guest rzeczywiście był
obecny przed zerwaniem, więc sesja hosta bez guesta zachowuje zwykłe zakończenie.

Eksporty `nativeInit/nativeCall` ScriptHooka widzą tylko wywołania ASI, dlatego
V31.4 ma domyślnie wyłączony `STORY VM CAPTURE` dla rzeczywistych handlerów gry.
Preflight wymaga jednocześnie przypiętego PE RDR2 1491.50, ScriptHooka 1491.17,
siedmiu dokładnych RVA i pełnych prologów. Dopiero wtedy u HOST-a instalowane są
małe, odwracalne trampoline dla CREATE/SET/REMOVE/PLAYBACK/LOAD/START/DELETE.
GUEST wykonuje identyczną walidację read-only, dzięki której może wywołać native
create dla własnego bridge-owned handle, lecz nigdy nie patchuje handlerów Story
VM. Częściowa instalacja hosta jest wycofywana w odwrotnej kolejności; przy
dowolnej niezgodności capture i native create pozostają wyłączone fail-closed.

Kontynuacja trampoline używa 14-bajtowego `jmp [rip+0]` z adresem osadzonym po
instrukcji. Nie wykorzystuje `RAX`: prefiks handlera
`SET_ANIM_SCENE_ENTITY` przechowuje tam wskaźnik argumentów aż do następnej
instrukcji oryginalnego kodu. Skok `mov rax, target; jmp rax` uszkadzał role
aktorów hosta mimo poprawnego odczytu diagnostycznego.

Hook przechwytuje wyłącznie bounded ASCII, flagi i lokalne uchwyty ról. Lokalny
uchwyt nigdy nie trafia na wire: host mapuje peda/konia na istniejący
`NetEntityId`, a obiekt związany z aktywną sceną przyjmuje do ograniczonej
`WorldEntityKind::Object` lane. Ambientowych obiektów nie skanujemy ani nie
replikujemy. Zwolnione przed drainem kanoniczne role propów `p_*`/`w_*` mogą
pozostać opcjonalne bez ID; nie odrzucają poprawnie zmapowanego castu.
Nierozwiązany gracz albo wymagany ped/koń/pojazd nadal odrzuca exact path.
Guest tworzy osobny, należący do bridge'a uchwyt AnimScene i używa kolejności
zależności `CREATE -> WAIT/SET_ENTITY(all required) -> LOAD`. Każdy poll ponawia
rozwiązanie brakujących world proxy; żądanie LOAD jest zabronione nawet przy
`21/22`. Po `resource-loaded` cała wymagana obsada jest sprawdzana ponownie, a
START następuje dopiero po commit. Brak, zły model albo timeout pozostaje błędem
fail-closed.
Cleanup może usunąć tylko ten bridge-owned handle. Po związaniu aktora pozostałe
motory ruchu, proxy tasks, mount tasks i transform corrections oddają mu pełną
własność, aby nie nakładać T-pose/teleportów na authored clip. Związane world
proxy pozostają widoczne; ukrycie całej obsady po bindzie dawało pustą scenę mimo
poprawnego zasobu. Początkowe opóźnienie jest nadrabiane duration-aware do około
750 ms zamiast odtwarzania sceny od fazy zero.

Standardowe `INPUT_SKIP_CUTSCENE` i pozycja F9 oddają głos za skipem. Guest
wysyła `SkipRequest`; host wstrzykuje zwykłą akcję wejściową najwyżej 2,5 s
dopiero po głosie obu graczy w tej samej generacji. Głos pozostaje ważny do
terminala tej cutscenki, więc gracze nie muszą trafić w krótkie wspólne okno.
Nie wymuszamy końca `MissionState` ani surowego zakończenia bez uchwytu
`AnimScene`; o końcu sceny nadal rozstrzyga silnik gry hosta.
Niezależnie od głosowania guest utrzymuje dzierżawę wejść Story, ale nie trzyma
ślepo globalnego skipu przez całą misję. Bounded native scanner wykrywa działającą
prywatną authored scenę, zapisuje jej konkretny handle i uzbraja
spectator/quarantine. Handle pozostaje przypięty do chwili, gdy scena rzeczywiście
przestanie działać; rotacyjny skaner nie może zwolnić kwarantanny tylko dlatego,
że w jednej klatce badał inny zakres. Game-owned handle nie jest pauzowany,
przyspieszany ani usuwany; kamera kwarantanny jest reassertowana, a lokalny Story
script zachowuje własną oś czasu. Globalny skip działa dopiero po pozytywnym
wykryciu kwarantanny i pozostaje wyłączony dla bridge-owned exact scene. Zapobiega
to sekwencyjnej drugiej cutscence bez popychania niewidocznej Story VM w stronę
`Gang Abandoned/Mission Failed` przez cały czas trwania misji.

## Downed, revive i checkpoint

Stan żywego gracza przechodzi do `Downed` zamiast natychmiastowej śmierci:

- brak timera wykrwawienia w v1;
- revive wymaga ciągłego przytrzymania przez 4 sekundy w odległości do 2 m;
- przerwanie odległości, wejście w spectator albo obrażenia przerywają akcję;
- podniesiony gracz wraca z 35% zdrowia;
- policy latch wymuszający pozę Downed nie jest wejściowym dowodem śmierci;
  zdrowy ped po respawnie może więc potwierdzić odzyskanie i wyłączyć ragdoll;
- gdy obaj gracze są powaleni, host uruchamia oryginalny retry/checkpoint;
- po checkpoint restore guest jest odtwarzany dopiero po resyncu hosta.

## Profil guesta

Wersjonowany JSON przechowuje pieniądze, loadout, amunicję i konia guesta. Zapis jest atomowy: nowy plik tymczasowy, flush, a następnie kontrolowana zamiana. Uszkodzony profil nie jest nadpisywany; trafia do kwarantanny i sesja startuje z bezpiecznym profilem domyślnym po potwierdzeniu w logu.

Dodatnią nagrodę pieniężną hosta podczas aktywnej misji można skopiować guestowi. Przedmioty fabularne, flagi i postęp nigdy nie są kopiowane.

## Kontrakt plików

W katalogu gry projekt posiada wyłącznie:

```text
CoopStoryBridge.asi
CoopStory.config.json
```

Sidecar pozostaje w `workspace\dist\sidecar`. Manifest wdrożenia pozostaje w `workspace\artifacts\deploy`. ScriptHook jest zależnością użytkownika i nie jest częścią paczki, instalatora ani uninstallera.

Każdy zapis operacyjny ma jawny, wąski target. Installer nie nadpisuje plików. Uninstaller usuwa tylko dozwolone ścieżki z poprawnego manifestu, po wykonaniu i zweryfikowaniu backupu.

## Safety i awarie

- Hash/version gate zatrzymuje nieobsługiwany build gry.
- Wykrycie online/RDO natychmiast wyłącza funkcje bridge’a; brak opcji bypass.
- Timeout IPC/sieci nie blokuje głównego wątku gry.
- Nieznane lub zbyt duże ramki są odrzucane przed alokacją payloadu.
- Token sesji nie trafia do logów i jest porównywany w stałym czasie tam, gdzie jest to praktyczne.
- Logi nie zawierają pointerów, pełnych lokalnych ścieżek ani publicznego IP.
- Crash sidecara nie może zapisać postępu kampanii guesta do save’a hosta.
- Operacje sieciowe i syntetyczny peer nie kontaktują usług Rockstar.
# V26.0 resume barrier and cinematic entity lane

Protocol 16 carries this V26.0 behavioral hardening without a wire-version bump. The cutscene state machine treats
`PrepareResume` as a real two-endpoint barrier: an active host never advances
to `Completed` until `ResumeReady` arrives, while a disconnected guest releases
the barrier through the normal replica-loss path. Resume streaming owns a
dedicated immutable anchor; replicated camera focus cannot reset its warm-up.

During `Playing`, `Loading` and `PrepareResume`, the host World Mirror continues
sampling mission actors at 10 Hz. The guest applies those actors while its own
local population remains reversibly masked. Camera replication and entity
replication therefore share the same cinematic generation without copying or
running the host's Story script VM on the guest.
