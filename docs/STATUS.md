# RDR2 Coop Story — stan implementacji

Stan na 17 sierpnia 2026. Aktualny build to
`Cutscene Mission Ownership V31.10 Alpha`, protokół 20.

## V31.10 — pełna obsada przed LOAD i host-only Story VM capture

- Skorelowane logi HOST/GUEST z obserwacją obu ekranów wskazały dokładną
  przyczynę pustej, opóźnionej sceny. Guest odebrał definicję
  `cutscene@ODR1_INT` z 22 wymaganymi rolami, lecz wykonał `LOAD` po związaniu
  tylko dwóch graczy (`prebound=2/22`). Pozostałe proxy pojawiły się w kolejnych
  klatkach, ale RDR2 nie dokończyło już ładowania tego uchwytu; po ośmiu
  sekundach następował `GuestRejected`, a później uruchamiała się prywatna Story
  VM guesta z samymi napisami, dźwiękiem i latającymi rekwizytami.
- Guest ponawia teraz rozwiązywanie ról na każdym pollu i nie może wywołać
  `LOAD_ANIM_SCENE`, dopóki wszystkie wymagane encje nie istnieją lokalnie, nie
  mają właściwego modelu i nie są przypięte. Oczekiwana kolejność to
  `CREATE -> WAIT/BIND 22/22 -> LOAD -> resource-loaded -> GuestReady ->
  HostPlayCommit -> START`. Brak aktora pozostaje fail-closed i nie uruchamia
  niepełnej sceny.
- Checkbox nadal można i należy włączyć na obu PC, ale detoury siedmiu handlerów
  Story VM są instalowane wyłącznie u HOST-a. GUEST wykonuje read-only preflight
  RVA/prologów i zachowuje `native-create=on`, lecz ma `capture=off` i nie
  patchuje scen należących do gry. Końcowy crash guesta urwał proces bez wyjątku
  bridge/sidecara; wyłączenie zbędnych detourów usuwa najbardziej ryzykowną
  różnicę po jego stronie bez odbierania exact replica.
- Kwarantanna prywatnej sceny guesta zapisuje teraz konkretny wykryty handle i
  trzyma spectator/freeze aż ten handle naprawdę przestanie prezentować. Krótki
  brak wyniku w rotacyjnym skanerze nie może już fałszywie ogłosić
  `residual scene cleared`, gdy nadal lecą dialogi, napisy lub rekwizyty.
- Eksport potwierdził poprawną retencję diagnostyki: oba ZIP-y obejmowały tylko
  dwie najnowsze sesje. Testy lokalne po zmianie: bridge `58/58`, build Release
  bez ostrzeżeń.

## V31.9 — zachowana obsada drugiej sceny i bezpieczny powrót guesta

- Ponowna korelacja nagrania z logami wykazała, że dystans `761.819 m` nie był
  pierwotnie błędem zwykłego teleportu konia. Konna cutscenka guesta nie
  kończyła się poprawnie, lokalna kara `weather too harsh` zabijała go, a RDR2
  odtwarzało jego prywatny, stary checkpoint. Teleport mounta z V31.8 pozostaje
  zabezpieczeniem, ale nie jest już opisywany jako przyczyna źródłowa.
- Przy wejściu w drugą, konną prezentację guest miał 26 poprawnych hostowych
  węzłów, po czym `HandleRemoteMissionState` kasował je tuż przed wybraniem
  `PROXY_CAST_FALLBACK`. Kamera i animowane rekwizyty zostawały, lecz fallback
  nie miał już Dutch/Hosea/koni do pokazania. Samo wejście w cinematic nie jest
  teraz granicą generacji świata: stabilna obsada zostaje zachowana, a reset
  następuje dopiero przy rzeczywistej zmianie checkpointu/epoki.
- `PrepareResume` nie ufa już staremu anchorowi zapisanemu na początku
  przejścia. Guest wybiera świeżą pozycję hosta z jego repliki, czeka 250 ms na
  jej ustabilizowanie i dopiero wtedy przygotowuje gracza oraz jego konia obok
  hosta. Diagnostyka zapisuje źródło anchoru i różnicę względem wartości wire.
- W trakcie autorytatywnej misji hosta guest przechwytuje nieprzypisaną do
  peda/obiektu/pojazdu lokalną karę granicy lub pogody już przy 30% zdrowia i
  odtwarza 75%. Chroni to przed rozpoczęciem prywatnego checkpoint reload,
  pozostawiając normalne, rozpoznane obrażenia fizyczne pod autorytetem co-opu.
- Pierwsza exact AnimScene nadal zawiera poprawkę V31.8:
  `CREATE -> SET_ENTITY -> LOAD -> START`. V31.9 jej nie cofa; teraz dodatkowo
  naprawia fallback i resume drugiej sceny.
- Testy lokalne: bridge `58/58`, sidecar/protokół `46/46`, launcher `28/28`.

## V31.8 — obsada guesta przed LOAD i poprawny anchor konia

- Logi równoczesnego HOST/GUEST potwierdziły, że Definition, 22 role i stabilne
  ID docierały poprawnie. Guest zatrzymywał się jednak na
  `waiting-resource resolved=0/22 resource-loaded=0`, ponieważ wykonywał
  `CREATE -> LOAD`, a aktorów wiązał dopiero po `IS_LOADED`. V31.8 usuwa ten
  cykl zależności i wykonuje:
  `CREATE -> SET_ENTITY -> LOAD -> START`. Po LOAD wszystkie wymagane role są
  ponownie sprawdzane, więc brak lub zły model nadal kończy się fail-closed.
- Lokalne odsunięcie aktora Story guesta o 180 m podczas spectator/quarantine
  nie jest już publikowane jako pozycja sieciowa. Host widzi zapisany anchor
  misji, a nie techniczny punkt stagingowy, więc nie uruchamia fałszywego
  ostrzeżenia mission bubble.
- Rescue teleport podczas jazdy przesuwa teraz konia jako fizyczny root razem
  z jeźdźcem. Późniejsza analiza wykazała jednak, że zmierzone `761.819 m`
  powstało po nieukończonej konnej scenie i prywatnym checkpoint respawn
  guesta; poprawka mounta jest zabezpieczeniem, a nie pełnym rozwiązaniem tej
  konkretnej awarii.
- Diagnostyka capture zapisuje teraz również `scene-flags`, `create-options`
  oraz liczbę ról przypisanych przed LOAD.
- Testy lokalne: bridge CTest `1/1`, sidecar/protokół `46/46`.

## V31.7 — naprawiony handler ról, exact cast mimo propów i trwały skip

- Nagranie hosta oraz logi V31.6 wskazały jedną wspólną przyczynę pustej
  obsady. Trampolina `SET_ANIM_SCENE_ENTITY` wracała do oryginalnego handlera
  skokiem używającym `RAX`, mimo że skopiowany prefiks nadal przechowywał w tym
  rejestrze tablicę argumentów. Kamera, dialog i wewnętrzne rekwizyty działały,
  ale role pedów dostawały uszkodzone argumenty. Kontynuacja jest teraz
  14-bajtowym, pośrednim skokiem RIP-relative, który nie nadpisuje żadnego
  rejestru gry.
- `cutscene@ODR1_INT / MultiStart` poprawnie zmapowała 14 aktorów, lecz osiem
  zwolnionych już uchwytów papierosów, broni, butelki i krzesła nadal błędnie
  podnosiło `unresolved-required=8`. Kanoniczne role `p_*`/`w_*` bez żywego
  uchwytu są teraz opcjonalnymi obiektami. Nierozwiązany ped, koń, pojazd albo
  gracz pozostaje błędem fail-closed.
- Ukryty lokalny aktor Story guesta jest podczas prezentacji stagingowany poza
  triggerem misji, przy zachowanym streamingu kamery hosta, a po scenie wraca do
  przygotowanego hostowego anchoru. To odcina zaobserwowaną prywatną kopię misji,
  która po dwóch minutach kończyła się `Gang Abandoned`.
- Skip nadal wymaga zgody HOST-a i GUEST-a, ale oba głosy są ważne do końca tej
  samej generacji cutscenki zamiast wygasać po pięciu sekundach.
- Testy lokalne: bridge `58/58`, sidecar/protokół `46/46`, launcher `28/28`.

## V31.6 — nieinwazyjna scena hosta, pełna obsada i jedna misja

- Najnowszy przebieg ON wykazał konkretną regresję: hostowy `PRELOAD_BARRIER`
  pauzował game-owned AnimScene przez około osiem sekund. W tym czasie Story VM
  nie kończył przypisywania aktorów, dlatego również host zaczął widzieć pustą
  obsadę. Bariera jest teraz wyłącznie logiczna — bridge obserwuje scenę hosta,
  ale nigdy jej nie pauzuje, nie przyspiesza ani nie usuwa.
- Przechwycona `cutscene@ODR1_INT / MultiStart` miała 22 role, lecz V31.5
  przesyłała tylko 14 pedów/koni. Protokół 20 dodaje do grafu świata ograniczony
  typ `Object`; host nadaje stabilne ID tylko rekwizytom faktycznie związanym z
  aktywną AnimScene (krzesła, broń, butelka, papierosy). Guest tworzy te proxy
  przed definicją i wiąże pełną obsadę. Rozmiar istniejącego payloadu świata się
  nie zmienia, ale mieszanie paczek protokołu 19 i 20 jest jawnie odrzucane.
- Guest najpierw ładuje zasób, a dopiero po potwierdzonym `resource-loaded`
  wiąże aktorów. Timeout nie może już odebrać zadań proxy i pozostawić fallbacku
  w T-pose. Prywatna scena Story VM guesta nie jest już przyspieszana 4x, a
  kamera kwarantanny jest ponownie przejmowana co klatkę, więc spóźniona scena
  nie powinna pojawić się jako druga, bardzo szybka cutscenka.
- Procesowy `MISSION_FLAG` nie jest dotykany w swobodnej grze. Po rozpoczęciu
  autorytatywnej misji hosta guest utrzymuje go jednak zajętego aż do terminala,
  aby jego lokalny Story VM nie wystartował później i nie wygenerował prywatnego
  `Gang Abandoned`. Każdy override odtwarza wartość przejętą bezpośrednio przed
  nim; koniec sesji nadal odtwarza stan początkowy.
- Detekcja M2/kontekstu obejmuje wszystkie pięć wariantów kontrolek RDR2.
  Wejście rozmowy lub własnego konia nie może już uzbroić ogólnej izolacji walki
  ani wywołać `SET_EVERYONE_IGNORE_PLAYER`, które blokowało rozmowę z NPC.
- Testy lokalne: bridge `58/58`, sidecar/protokół `46/46`, launcher `28/28`.

## V31.5 — swobodne interakcje guesta i rozdzielone konie

- Logi z ostatniego testu wykazały, że `MISSION_FLAG` był utrzymywany przez cały
  lease guesta, a szeroki skan traktował każdego script-owned człowieka w
  promieniu 20 m jak marker misji. W obozie blokowało to M2/rozmowę oraz kontekst
  własnego konia. Flaga procesu jest teraz zajmowana tylko podczas hostowej
  prezentacji, potwierdzonej kwarantanny albo przy zweryfikowanym, blipowanym
  aktorze startu lokalnej misji. Zwykły NPC bez blipa i każdy koń pozostają poza
  strażą; host nadal jest jedynym właścicielem misji i save'a.
- Ten sam test zawierał rzeczywiste `PEER_DISMOUNT` action-id 7 i 13 dokładnie po
  wejściu drugiej osoby na konia. RDR2 współdzieli część kontrolek mount/talk z
  melee/grapple, więc wejście kontekstowe zostało błędnie opublikowane jako
  `Knockdown`. V31.5 odrzuca takie nakładające się wejście, gdy trwa kontekst lub
  własny koń jest blisko, a prawdziwe ściągnięcie z konia dostaje jawny istniejący
  `variantHash = MPUL`. Odbiorca może wywołać `TaskDismountAnimal` na lokalnym
  graczu wyłącznie dla tego wariantu.
- Relacja `PlayerMountState` ma dodatkową ochronę uchwytu: rider musi ponownie
  rozwiązać się do stabilnego ID zdalnej repliki i nigdy nie może być lokalnym
  `PLAYER_PED_ID`. Pomieszany/stary mapping jest odrzucany zamiast animować
  wsiadanie lub zsiadanie niewłaściwej osoby.
- Protokół i rozmiary payloadów pozostają w wersji 19; wykorzystano istniejące
  pola `VariantValid/variantHash`. Testy bridge: `58/58`.

## V31.4 — wspólny start exact scene i diagnostyka dwóch sesji

- Eksport diagnostyczny nie dołącza już całych `.1/.2/.3` z wielu dawnych
  uruchomień. Zdarzenia `runtime.started` wyznaczają okno dwóch najnowszych
  sesji; surowe logi, indeks, timeline, anomalie i markery korzystają z tego
  samego cutoffu. Rotacje są scalone do jednego pliku na źródło, a
  `DIAGNOSTICS_SESSIONS.json` zapisuje dokładnie wybrane początki sesji.
- Guest uruchamia `LOAD_ANIM_SCENE` natychmiast po bezpiecznym CREATE, równolegle
  z oczekiwaniem na world proxy i bindingi. `PREPARE_PROGRESS` rozróżnia
  `waiting-bindings` od `waiting-resource` oraz podaje pierwszą brakującą rolę i
  stabilny `NetEntityId`, więc kolejny timeout nie będzie już niejednoznaczny.
- Po przechwyceniu poprawnej definicji host odwracalnie pauzuje wyłącznie
  aktywną game-owned AnimScene. Pozostaje w `Loading`, aż guest załaduje zasób,
  zwiąże wszystkie wymagane role i odeśle `GuestReady`. Dopiero niezawodny
  `HostPlayCommit` zwalnia hosta i uruchamia bridge-owned scenę guesta. Reject,
  timeout, reconnect, terminal i STOP zawsze zdejmują pauzę.
- Capture guesta rozpoznaje prywatną scenę wiążącą lokalnego gracza już przy
  `LOAD`, zanim jej kamera dostanie pierwszy START. Taka scena wchodzi w
  spectator/quarantine, a po START jest bezpiecznie przyspieszana do 4x bez
  usuwania game-owned handle. Usunięto ślepe trzymanie globalnego skipu przez
  całą misję — w V31.3 nie zatrzymywało ono opóźnionej Story VM i mogło popychać
  ją w stronę `Gang Abandoned/Mission Failed`.
- Stan Downed nie czyta już własnego policy latcha jako dowodu, że nowy ped po
  respawnie nadal umiera. Fizycznie zdrowy respawn może przejść stabilne
  potwierdzenie odzyskania i wyłączyć wymuszony ragdoll.
- Testy lokalne: bridge `57/57`, sidecar/protokół `46/46`, launcher `28/28`.

## V31.3 — exact cast, wspólna oś czasu i pełna dzierżawa misji hosta

- Cztery porównawcze logi OFF/ON potwierdziły, że probe działał poprawnie i
  przechwycił `cutscene@ODR1_INT / MultiStart` z 22 rolami, ale runtime odrzucił
  całą definicję z powodu ośmiu niezmapowanych ról. W rezultacie tryb ON nigdy
  nie wysłał `AnimSceneDefinition` i oba przebiegi pozostały w `SAFE_FALLBACK`.
- Zmapowani gracze, NPC i konie pozostają rolami wymaganymi. Niezmapowane
  rekwizyty/role lokalne sceny są teraz kanonicznymi rolami opcjonalnymi bez
  uchwytu, modelu i binding flags, więc nie unieważniają 14 poprawnie
  zmapowanych aktorów. Nierozwiązana rola gracza nadal zatrzymuje exact path.
  Log `CAPTURE_MAP` podaje mapped/optional/duplicate/unresolved i nazwy
  pominiętych ról, bez pointerów ani lokalnych uchwytów.
- Exact AnimScene wiąże istniejące repliki. V31.2 po udanym bindzie omyłkowo
  ukrywał wszystkie world proxy, czyli również Dutch/Hosea sterowanych przez
  scenę. V31.3 utrzymuje związany cast widoczny, niezamrożony i oddaje jego
  animację/root motion wyłącznie natywnej AnimScene; ukryte zostają tylko
  niezwiązane proxy fallbacku.
- Guest może cold-loadować zasób przez 8000 ms, a host przyjmuje odpowiedź przez
  10000 ms bez blokowania własnego Story VM. Scena startująca po przygotowaniu
  dostaje bounded fast-forward i duration-aware korektę fazy do około 750 ms,
  zamiast odtwarzać początek po zakończeniu sceny hosta.
- Logi pokazały lokalne sceny Story VM guesta 2, 34, 66 i 98 sekund po terminalu
  hosta. Ochrona nie kończy się już po sześciu sekundach ani po pojedynczym oknie
  2500 ms: standardowy kontekst skip pozostaje zarezerwowany przez całą aktywną
  misję hosta i całą kwarantannę. Jest jawnie wyłączany, gdy przygotowuje się lub
  działa bridge-owned exact scene, więc nie może pominąć poprawnej wspólnej
  prezentacji.
- Jedyny zarejestrowany `0xC0000005` pochodził z
  `remote-transform-presentation` w zwykłym gameplayu. Ta natywna ścieżka ma
  teraz własną granicę SEH: fault zwraca błąd do bounded respawnu repliki zamiast
  wyłączać cały bridge/coopa.
- Lasso nie było zmieniane w tej iteracji zgodnie z zakresem testu. Testy lokalne:
  bridge `57/57`, sidecar/protokół `46/46`, launcher `28/28`.

## V31.2 — eksperymentalny capture Story VM i poprawki z testów 1–3

- Sześć nowych archiwów HOST/GUEST potwierdziło trzy odrębne problemy. Guest 1
  zakończył ASI wyjątkiem `0xC0000005` w prezentacji zdalnego gracza, Guest 2
  zakończył cały proces RDR2 podczas aktywnej ścieżki mount, a testy 2–3 nadal
  uruchamiały lokalną cutscenkę guesta dopiero po terminalu hosta. Dźwięk i
  napisy pochodziły z tej spóźnionej lokalnej Story VM, podczas gdy brak
  przypisanych aktorów dawał pustą scenę/T-pose.
- Domyślnie wyłączony checkbox launchera nazywa się teraz `STORY VM CAPTURE`.
  W trybie ON ASI wymaga dokładnie przypiętego `RDR2.exe 1.0.1491.50`,
  `ScriptHookRDR2 1.0.1491.17`, siedmiu zgodnych RVA i pełnych oczekiwanych
  prologów. Dopiero po przejściu całego preflightu zakłada odwracalne hooki
  `CREATE/SET_ENTITY/REMOVE_ENTITY/SET_PLAYBACK/LOAD/START/DELETE`. Każda
  niezgodność wycofuje częściową instalację i pozostawia `SAFE_FALLBACK`.
- Host przechwytuje rzeczywistą nazwę zasobu, playback, flagi tworzenia i role
  Story VM. Definicja nadal przechodzi przez protokół 19 i dwufazowy handshake.
  Guest tworzy wyłącznie własny uchwyt AnimScene, wiąże stabilne encje i startuje
  go dopiero po `HostPlayCommit`. Bridge nigdy nie usuwa sceny należącej do gry.
- Po związaniu roli dokładna AnimScene jest jedynym właścicielem root motion,
  klipu, IK i relacji mount. Zwykły AnimGraph Replica, transform, proxy task oraz
  korekcje konia ustępują, co usuwa konflikt powodujący teleport/T-pose/obrót o
  180 stopni. Definicja przekraczająca 48 ról lub mająca nierozwiązaną wymaganą
  encję jest odrzucana zamiast uruchamiania niepełnej sceny.
- W fallbacku guest prewencyjnie tłumi własną lokalną authored cutscenkę przez
  prezentację hosta i sześć sekund po terminalu. Gdy dokładna bridge-owned scena
  jest przygotowywana lub działa, globalny input jest wyłączony, aby nie pominąć
  jej przypadkiem; sześcioseskundowa ochrona wraca po jej teardown. Nie jest to
  głos skipu hosta; równoczesny skip dwóch graczy nadal ma osobny konsensus. Celem jest
  niedopuszczenie do sekwencyjnej sceny, zamrożonego Arthura i spóźnionego
  `Mission Failed` po odzyskaniu sterowania.
- Ścieżka zdalnego mounta działa najwyżej co 50 ms, ma osobne breadcrumbs
  `remote-mount/remote-animation/remote-transform-presentation` i nie wymusza
  już aktualizacji AI/animacji w każdej klatce. Ma to ograniczyć natywny crash
  bez komunikatu widoczny w Guest 2 i dać jednoznaczny etap w następnym logu.
- Odbiorca lassa nie uruchamia już `TASK_LASSO_PED` przy samym `Begin` ani przy
  samym namierzeniu celu. Natywny rzut powstaje dopiero po potwierdzonym efekcie
  fizycznym nadawcy, więc guest trzymający spust nie powinien samoczynnie rzucać
  u hosta. Gdy natywna lina nie istnieje, autorytatywny fallback powala wyłącznie
  ofiarę; nie może przewrócić obu graczy.
- Capture jest pierwszym eksperymentalnym live buildem, nie gwarancją pełnej
  zgodności każdej sceny. Props/vehicles/pickups i dokładny MetaPed NPC nadal
  mogą wymusić fallback. Test porównawczy musi użyć obu PC z Capture OFF, a potem
  obu PC z Capture ON. Testy lokalne: bridge `57/57`, sidecar/protokół `46/46`,
  launcher `28/28`.

## V31.1 — przełączany probe i bezpieczny fundament transportu AnimScene

- Protokół 19 dodaje niezawodną `AnimSceneDefinition` (ID 39) z nazwą zasobu,
  playback listą, flagami tworzenia oraz maksymalnie 48 nazwanymi rolami
  `NetEntityId`. Kanoniczny payload ma 128-bitowy fingerprint wyprowadzony z
  SHA-256 i limit 8 KiB. Lokalne uchwyty i pointery nadal nie trafiają na wire.
- `AnimSceneControl` (ID 40) realizuje handshake
  `Definition -> GuestReady/GuestRejected -> HostPlayCommit/HostAbort`.
  Definicje i decyzje idą tylko uporządkowanym kanałem TCP/pipe, bez UDP i bez
  coalescingu. Lokalne przygotowanie/decyzja mają limit 2500 ms, a odbiorca
  zachowuje 4000 ms okna z zapasem na RTT/jitter. Host nigdy nie blokuje Story VM
  i przy braku odpowiedzi wybiera istniejący `SAFE_FALLBACK`.
- Sidecar przechowuje wyłącznie najnowszą zwalidowaną definicję. Po peer reconnect
  czeka na uwierzytelniony `ResyncRequest` i odtwarza kolejno `MissionState`,
  `MissionCinematicState`, spawny parent-first oraz dopiero zgodną definicję.
  Zgodny `HostAbort`, koniec cutscenki lub reset sesji czyści cache. Role gracza,
  mounta i znanych replik są mapowane przez stabilne `NetEntityId`; wymaganej
  nierozwiązanej roli nie wolno wysłać.
- `MissionCinematicState`, definicja i decyzje hybrydowe korzystają z jednej
  krytycznej kolejki FIFO. Po `GuestReady` host podejmuje decyzję do 2500 ms, a
  guest czeka 4000 ms wraz z zapasem transportowym; utracona decyzja kończy
  przygotowany uchwyt bezpiecznym abortem. Pełny reconnect pipe/Hello i timeout
  strumienia zachowują hostowy graf oraz stabilne ID ról, ponowione `Ready` jest
  idempotentne, ale każda nowa próba po reconnect/resync dostaje wyższą
  `DefinitionRevision` i nowy fingerprint. Stary `Ready` nie może więc zatwierdzić
  odtworzonej sceny. `PrepareResume` utrzymuje barierę także w `SAFE_FALLBACK`,
  między `HelloAck` i pierwszym świeżym `PlayerState` oraz przez wielokrotne próby
  Hello bez odpowiedzi. Host, który naprawdę nie miał guesta, nie czeka na
  nieistniejący stream. Stan terminalny blokuje każdy spóźniony `Commit`.
- Replay po reconnect jest transakcją związaną z konkretną generacją TCP. Host
  przechwytuje cache dopiero wewnątrz bramki wysyłki, więc live despawn/terminal
  nie może wejść w środek snapshotu, a wymiana peera unieważnia cały stary suffix.
  Guest uznaje lokalny reset dopiero po pełnym zapisie do bridge'a i dopiero wtedy
  prosi hosta o stan. Stare `PlayerAction`/lasso/interaction z poprzedniej
  generacji nie mogą już zmienić autorytetu ani trafić do nowego peera.
- Named pipe ma własny token generacji. Role/motion handshake oznacza jako gotowe
  tylko to samo połączenie, a reset i ruch network→bridge są wysyłane wyłącznie do
  tej gotowej generacji. Zapis oczekujący na pipe A nigdy nie przechodzi na pipe B
  przed jego `Hello/HelloAck`.
- Datagram UDP ma teraz uwierzytelnione `sender InstanceId` bieżącego handshake
  TCP oraz ścisły floor sekwencji. Opóźniony datagram starego procesu/socketu jest
  odrzucany przed przypięciem portu nowej sesji.
- Launcher zapisuje domyślnie wyłączony checkbox `STORY VM PROBE` obok
  `ANIMGRAPH REPLICA`. Ustawienie idzie wyłącznie lokalnym pipe'em do ASI i nie
  bierze udziału w negocjacji HOST–GUEST. Pozwala wykonać porównywalny przebieg
  bez inspektora oraz z nim, bez zmiany silnika ruchu.
- ASI zawiera jednorazowy, read-only `Handler Inspector` przypięty do
  znanego układu PE `RDR2.exe 1.0.1491.50` i `ScriptHookRDR2 1.0.1491.17`.
  Odczytuje adresy,
  właścicieli modułów i 32-bajtowe prologi siedmiu handlerów AnimScene. Nie używa
  `nativeCall`, nie zmienia ochrony pamięci, nie zakłada detoura i niczego nie
  patchuje.
- W tej generacji runtime capture i tworzenie dokładnej sceny guesta pozostają
  celowo wyłączone. Eksporty ScriptHooka widzą wywołania ASI, ale nie wywołania
  Story VM; włączenie detoura bez jednego logu live byłoby zgadywaniem sygnatur.
  Brak kompletnej definicji zachowuje dotychczasowe `ATTACHED`/kamerę/proxy
  `SAFE_FALLBACK`, więc V31.1 jest etapem pomiarowym, a nie obietnicą pełnego
  lipsyncu i authored audio.
- Stabilną tożsamość mają dziś gracze, mounty i encje już należące do rejestrów
  replik. Props, vehicles i pickups istnieją w formacie definicji, ale nie mają
  jeszcze pełnego hostowego lifecycle w World Mirror; scena wymagająca takiej
  nierozwiązanej roli musi pozostać w fallbacku.
- Testy kodeków C++/C#, fingerprintu, cache, kierunków autorytetu, reconnectu,
  zakazu UDP dla Definition/Control i FIFO przechodzą lokalnie: bridge `57/57`,
  sidecar/protokół `46/46`, launcher `28/28`. Pełne uruchomienie exact-path wymaga
  teraz jednego testu HOST/GUEST według `TEST_V31_ANIMSCENE_HYBRID.md`.

## V30.3 — poprawki po pełnym teście HOST/GUEST V30.2

- Logi obu komputerów potwierdziły zdrowy transport stanu i kamery cutscenki,
  ale guest przez cały jej czas nie znalazł lokalnego uchwytu o zgodnym
  dictionary/duration (`SAFE_FALLBACK`, nigdy `ATTACHED`). Fallback pokazuje teraz
  niekolizyjną hostową obsadę proxy ze stabilizowanym korzeniem oraz natywnym
  idle/lokomocją zamiast pustego kadru. Nie jest to kopia gestów, lipsyncu ani
  ścieżek AnimScene; pełna authored prezentacja nadal wymaga `ATTACHED`.
- Po zakończeniu prezentacji hosta guest przez pięć sekund skanuje uruchomione
  lokalne AnimScene z kamerą. Wykryta opóźniona scena save'a trafia do kwarantanny
  i dostaje jedno ograniczone okno zwykłego skipu, aby druga cutscenka nie
  uruchamiała się sekwencyjnie.
- V30.2 dowiodła, że stale ustawiony lokalny `MISSION_FLAG` nie przemalowuje
  wszystkich już zainicjalizowanych żółtych markerów. Pełny lease guesta skanuje
  teraz pobliskich nie-mountowych aktorów Story również we free roam, blokuje
  kontekst rozpoczęcia misji i pokazuje szary komunikat
  `COOP: MISJA ZABLOKOWANA DLA GOSCIA`. Waniliowa kłódka może nadal nie pojawić
  się wizualnie; kryterium testu to brak uruchomienia lokalnej misji guesta.
- Początek lassa potrafi wskazać drugiego gracza z bezpiecznego korytarza promienia
  kamery, gdy silnik jeszcze nie zwrócił target handle. Po wygaśnięciu fizycznej
  dzierżawy sam target-only `Sustain` nie może ponownie tworzyć tasku i powodować
  migania animacji/lina–brak liny. Nie dodano sztucznego ragdolla.
- Protokół pozostaje w wersji 18.

## V30.2 — odzyskiwanie cover/crouch i impuls grafu strzału

- Log SOLO potwierdził, że transportował poprawne `fireSequence`, broń, cel oraz
  wszystkie semantyki cover, ale odbiorca odtwarzał tylko pocisk/dźwięk i krótki
  aim-task. Zdalny strzał stojącej postaci uruchamia teraz ograniczony
  `TASK_SHOOT_AT_COORD`, który przeprowadza natywny graf broni przez gałąź
  strzału/recoil. Na czas tasku amunicja i magazynek proxy są zerowe; widoczny
  pocisk nadal ma obrażenia 0, po czym dokładny stan amunicji jest przywracany.
- Wejście ze stealth do cover nie wyłącza już wspólnej gałęzi crouch w tej samej
  klatce. Fallback pozostaje własnością cover aż do rzeczywistego wyjścia, nawet
  jeżeli silnik później znajdzie natywny cover point.
- Utracony natywny cover jest ponownie pozyskiwany z ograniczeniem częstotliwości.
  Fallback crouch oraz zwykły crouch mają niezależne watchdogy, które reagują na
  zmierzony brak stanu zamiast resetować AnimGraph co klatkę.
- `ANIMGRAPH_REPLICA` raportuje teraz `fire-graph-pulses`, przywrócenia amunicji,
  ponowne pozyskania cover i odzyskania crouch. Protokół pozostaje 18.

## V30.1 — wspólny marker F7 i dowodowe okna diagnostyczne

- `F7` tworzy bezpieczny `Command(DiagnosticMarker)` z dokładnym liczbowym
  identyfikatorem korelacji. Sidecar dopuszcza go symetrycznie tylko wtedy, gdy
  zakodowana rola źródłowa odpowiada uwierzytelnionemu peerowi.
- Jedno naciśnięcie zapisuje marker lokalnie i u odbiorcy. Oba bridge'e wykonują
  natychmiastowe podsumowanie oraz przez 15 sekund próbkują co 500 ms pozycje,
  akcje, lasso, cover, wodę, stan misji, kamerę, AnimScene i rozjazd puppeta.
- Sidecar przy nadaniu i odebraniu zapisuje pełny chwilowy stan liczników
  transportu, heartbeat, kolejki bridge'a, grafu encji i interakcji.
- Eksporter dodaje `MARKER_WINDOWS.md/json`: dla każdego identyfikatora zbiera
  chronologiczny kontekst od 10 sekund przed do 15 sekund po `F7`. Surowe logi,
  timeline i raport anomalii pozostają w paczce. Sekrety i adresy nadal są
  redagowane. Układy istniejących wiadomości nie zmieniają się; protokół to 18.

## V30.0 — kierunkowy AnimGraph Replica bez fałszywych klipów

- Naprawiono podstawową konwencję headingu RDR2. `0°` oznacza `+Y`, a nie
  matematyczne `+X`; stary przelicznik mógł przesuwać kierunek chodu i celu tasku
  o 90 stopni.
- `PlayerState` już zawierał lokalną prędkość przód/bok. V30 klasyfikuje osiem
  kierunków, odświeża natywny task po rzeczywistej zmianie kierunku i zachowuje
  strafe/backpedal obsługiwany przez połączony task celowania.
- Stojąca replika wykonuje ograniczony `TASK_ACHIEVE_HEADING` zamiast skokowej
  zmiany headingu. Na nowym pedzie włączane są natywne arm/head/leg/torso IK.
- Istniejące pole `PlayerAction.normalizedPhase` przenosi teraz uczciwie
  znormalizowany czas transakcji. Nie jest przedstawiane jako faza klipu RAGE.
- Trzy wolne bity protokołu 18 opisują `InWater`, `Swimming` i
  `SwimmingUnderwater`; odbiorca sprawdza, czy lokalny wolumen wody naprawdę
  uruchomił właściwy graf. Pływanie pionowe nie jest już mylone ze spadaniem.
- Pełny odczyt clip ID, warstw, fazy, par tackle/hogtie, lipsyncu i dokładnych
  tasków interakcji nadal wymaga zweryfikowanego readera dla wersji 1491.50.
  V30 nie wpisuje wymyślonych offsetów ani nazw animacji. Protokół pozostaje 18.

## V29.6 — stabilny fallback cutscenki i poprawki po teście V29.5

- Logi HOST/GUEST potwierdziły zdrowy transport UDP AnimScene, ale lokalna scena
  guesta przestawała działać po około 1,8 s. Dopasowanie zachowuje teraz ten sam
  uchwyt i wykonuje najwyżej jedną próbę ponownego startu; diagnostyka rozróżnia
  brak pracy sceny, brak lokalnej kamery i wykorzystaną próbę restartu.
- Podczas każdej prezentacji spectator proxy hostowych NPC i koni pozostają
  ukryte. Kamera fallback nie odsłania już transformowanych kopii pozbawionych
  lokalnego grafu AnimScene, co usuwa źródło T-pose, teleportów i obrotów o 180°.
- Wykrywanie lokalnej misji nie traktuje już samej utraty sterowania podczas
  ładowania/wyniku jako nowej misji. Usuwa to pętlę kwarantanny po cutscence,
  która mogła doprowadzić guesta do `Mission Failed`.
- Waniliowy mission gate jest zajęty przez cały uwierzytelniony lease guesta.
  Test V30.2 później wykazał, że nie gwarantuje to wizualnej kłódki na wcześniej
  zainicjalizowanym markerze; V30.3 rozszerza właściwą straż kontekstu na cały lease.
- Host nie wpuszcza do początkowego grafu niewidocznych, zachowanych przez silnik
  puli koni/aktorów. Cios nie jest już kasowany przez sieciowy terminal tuż przed
  kontaktem. Task lassa zachowuje własność do terminala, a fizyczna lina wyłącza
  korekcję korzenia złapanego proxy, aby nie przewracać rzucającego.
- Minimalna długość hasła launchera wynosi teraz 4 znaki (maksymalnie 64).
  Protokół pozostaje w wersji 18.

## V29.5 — widoczna blokada misji, wspólny cel i natywne lasso

- Guest nie zajmuje już waniliowego `MISSION_FLAG` od chwili uruchomienia
  coopa. Markery Story mogą najpierw normalnie się zainicjalizować, a flaga jest
  ustawiana dopiero podczas aktywnej misji/prezentacji hosta lub kwarantanny.
  To przywraca oczekiwane przejście lokalnych markerów guesta do szarego stanu
  z kłódką. Straż kontekstu aktorów Story w promieniu 20 m nadal blokuje krótki
  wyścig przed dostarczeniem stanu misji hosta.
- Guest ma podczas aktywnego gameplayu misji stały panel
  `AKTYWNA MISJA HOSTA / WSPOLNY CEL FABULARNY` oraz przemianowany żółty marker
  celu. Nie jest to fałszywa kopia tekstu z hostowego mission VM: SDK nie daje
  zweryfikowanego odczytu wiersza typu `Follow gang`, więc dokładna lokalizowana
  treść celu nadal nie jest transportowana.
- Zdalny aktor dostaje i wybiera prawdziwe `weapon_lasso` już z pakietu Aim lub
  Lasso. Celowany `Begin` uruchamia `TASK_LASSO_PED` przed potwierdzeniem trafienia,
  dzięki czemu silnik może pokazać trzymanie, kręcenie, rzut i linę.
- `PhysicalTargetEffect` nadal pojawia się dopiero po potwierdzeniu złapania,
  lecz żadna ścieżka lassa nie zastępuje już nieudanej/późnej liny przez
  `SET_PED_TO_RAGDOLL`. Ragdoll pozostał wyłącznie dla jawnego Knockdown/Downed.
- Protokół pozostaje w wersji 18; istniejące pola `PlayerAction` wystarczają do
  wcześniejszego rozpoczęcia natywnej prezentacji lassa.

## V29.4 — sesje na hasło i save hosta w Ustawieniach

- Widoczny kod R2C1, kopiowanie kodu i pliki `.coopjoin` zniknęły z głównego
  interfejsu. HOST po kliknięciu `HOSTUJ` wpisuje oraz powtarza hasło, a GUEST
  po kliknięciu `DOŁĄCZ` wpisuje IPv4 hosta i dokładnie to samo hasło.
- Launcher wyprowadza z hasła i kanonicznego IPv4 zgodne z protokołem 18 dane
  uwierzytelniające przez PBKDF2-HMAC-SHA256 (600 000 iteracji). Hasło nie jest
  zapisywane jawnie w ustawieniach, konfiguracji sidecara ani logach.
- Po poprawnym przygotowaniu poświadczeń panel sesji pokazuje
  `HASŁO ZAPISANE`. Obliczenie PBKDF2 działa poza wątkiem UI.
- Pole `Lokalny save hosta — SRDR*` zostało przeniesione z ekranu START do
  `Ustawień`; nadal zapisuje wyłącznie lokalną nazwę i hash, bez kopiowania save'a.
- Awaryjny panel F8 nie udostępnia już HOST/JOIN ani kodu ze schowka. Zawiera
  tylko zatrzymanie bieżącej sesji, sterowanie HUD-em i zamknięcie panelu.
- Protokół pozostaje w wersji 18, a transport AnimScene V29.2/V29.3 nie zmienia się.

## V29.3 — lobby launchera i szybsza diagnostyka w grze

- Pełna konfiguracja HOST/JOIN w launcherze uruchamia sieć bez oczekiwania na
  menu w grze. Panel F8 jest domyślnie zamknięty i pozostaje ręcznym panelem
  awaryjnym; nie zasłania Story Mode po jego wczytaniu.
- Launcher pokazuje małe lobby z nickami, czerwonym HOST-em, niebieskim
  GUEST-em, stanem sidecara/bridge'a, adresem peera i pingiem w ms. Przycisk
  `DOŁĄCZ` w V29.3 prosił o IPv4 hosta oraz prywatny kod R2C1; V29.4 zastępuje
  ten etap opisanym wyżej hasłem sesji.
- Zmiana stron launchera tworzy uchwyty kontrolek przed pierwszym rysowaniem i
  przełącza buforowany panel jako jedną operację. Puls przycisku START odświeża
  się rzadziej i tylko wtedy, gdy jest widoczny oraz aktywny.
- F9 ma dwa uporządkowane panele: najczęstsze akcje sesji/ratunku po lewej i
  narzędzia testowe po prawej. Strzałki lewo/prawo zmieniają grupę.
- `F7` zapisuje `USER_MARKER` bez otwierania F9. Krótki komunikat w prawym dolnym
  rogu potwierdza numer markera; przytrzymanie klawisza nie tworzy duplikatów.
- Protokół pozostaje w wersji 18. Transport AnimScene, `ATTACHED`, pełny skan
  `SAFE_FALLBACK`, równoczesny skip i blokada lokalnej misji guesta są bez zmian
  względem V29.2 i nadal wymagają właściwego testu na dwóch komputerach.

## V29.2 — fail-closed wybór AnimScene i ograniczony recovery skip

- Host wybiera do transportu wyłącznie AnimScene, która aktualnie posiada
  aktywną kamerę. Długowieczna scena ambientowa bez kamery nie może już zająć
  cache i zasłonić właściwej cutscenki misji.
- Guest ogłasza `SAFE_FALLBACK` dopiero po pełnym przeglądzie 4096 lokalnych
  uchwytów. Częściowy batch wyszukiwania nie jest już mylony z ostatecznym
  brakiem dopasowania.
- Lokalna scena objęta kwarantanną dostaje jedno okno standardowego
  `INPUT_SKIP_CUTSCENE` o długości najwyżej 2500 ms. Wejście nie pozostaje
  wciśnięte przez cały długi stan kwarantanny i uzbraja się ponownie dopiero po
  jej rzeczywistym zwolnieniu oraz nowym przejściu lokalnej misji.
- Test bridge'a obejmuje teraz transport hosta, `ATTACHED`, zawieszenie kamery
  fallback, powrót do kamery po braku dopasowania/utracie świeżości oraz ponowne
  dołączenie po świeżym stanie AnimScene.

## V29.1 — transport AnimScene i równoczesny skip

- Naprawiono listę typów dozwolonych na UDP: `AnimSceneReplicaState` dociera
  teraz z hosta do guesta zamiast kończyć jako odrzucony datagram. Dzięki temu
  guest może faktycznie dopasować lokalną scenę i użyć jej kamery, NPC, dialogu,
  napisów oraz audio.
- Po zaakceptowaniu głosów obu graczy `SkipPending` zasila standardowy
  `INPUT_SKIP_CUTSCENE` w obu procesach RDR2, a nie tylko u hosta.
- Późna lokalna scena guesta jest pomijana tym samym bezpiecznym wejściem,
  kwarantanna zwalnia się po potwierdzonym odzyskaniu kamery i sterowania, a
  hostowy mission gate pozostaje zajęty.
- Straż lokalnych aktorów Story działa od 20 m, aby odciąć interakcję przed
  wejściem guesta w żółty wolumen aktywacji.
- Test protokołu sprawdza transport UDP AnimScene, a test bridge'a odtwarza
  zatwierdzenie i zwolnienie skipu po stronie guesta.

## V29.0 — dokładny MetaPed i bezpieczne dopasowanie AnimScene

- Protokół 18 dodaje `PlayerAppearanceState` (ID 37) oraz
  `AnimSceneReplicaState` (ID 38) i odrzuca mieszane buildy V28/V29.
- Host i guest wysyłają uporządkowany pełny zestaw shop-componentów swojego
  Story MetaPed. Proxy o zgodnym modelu odtwarza strój dopiero po zmianie
  fingerprintu; różny model nie jest resetowany.
- Host wykrywa aktywną AnimScene i publikuje przenośną sygnaturę: dictionary,
  duration, phase, rate, origin i aktywne kamery. Żaden uchwyt nie trafia na sieć.
- Guest dopasowuje wyłącznie już istniejącą lokalną scenę. Po dopasowaniu
  korzysta z jej oryginalnej kamery, animacji aktorów, rozmów, napisów i audio,
  a drift fazy koryguje rate/pause.
- Brak dopasowania jest bezpiecznym fallbackiem: pozostaje dokładna kamera V28,
  hostowe rooty aktorów i pełny teardown bez uruchamiania lokalnej misji guesta.
- `SESSION_HEALTH` zapisuje teraz stan strumienia AnimScene, tryb native/fallback,
  wiek snapshotu oraz rewizję i liczbę komponentów stroju.

## V28.0 — kamera przed Playing i cinematic root lane

- Host wysyła pierwszą poprawną klatkę kamery już w `Loading`; `Playing`
  czeka na kamerę i potwierdzenie prezentacji guesta.
- Guest odtwarza filmowe cięcia bez gameplayowego wygładzania, ponawia
  aktywację kamery i zachowuje ostatnią klatkę przez krótki ubytek UDP.
- Hostowi aktorzy sceny mają osobny stan `Cinematic`, próbkowany co 33 ms.
  Receiver nie zastępuje go już zadaniem `TASK_STAND_STILL`.
- Bramka wszystkich lokalnych misji guesta działa przez całą sesję. Lokalna,
  pozbawiona właściciela kara lethal ze starego save'a nie tworzy już
  autorytatywnego `Downed` podczas misji hosta.
- Protokół 17 zapisuje źródło kamery i celowo odrzuca starsze paczki.

V29 nie potrafi utworzyć brakującej sceny z samego hasha, ponieważ native
`CREATE_ANIM_SCENE` wymaga oryginalnej nazwy dictionary i nazw ról. Dlatego
oryginalne klipy/dialog/audio są dostępne tylko po bezpiecznym dopasowaniu
lokalnej sceny; w pozostałych przypadkach obowiązuje kamera V28.
Protokół 16 zachowuje niezawodny `MissionState`, odrzuca mieszane buildy i dodaje
host-authoritative `InteractionIntent/Result`, `RestraintState` oraz wersjonowany
FSM cutscenki. `MissionCameraState` przenosi pozycję, obrót, FOV i generację sceny. Guest utrzymuje
przez cały lease sesji zajęty vanilla mission gate, więc lokalny marker nie może
uruchomić konkurencyjnego mission VM. Oryginalna flaga jest przywracana dopiero po
jawnym zatrzymaniu sesji lub unloadzie moda.

## V27.0 — stabilizacja kamery, końca sceny i głosowania

Diagnostyka V26.1 wykazała zapętlenie hostowego FSM po skipie: ta sama scena
przechodziła `Playing → PrepareResume → Completed → Playing` mniej więcej co
0,8 s. V27.0 traktuje `PrepareResume` jako jednokierunkowy commit i blokuje
ponowne otwarcie sceny krótkim terminalnym latchem. Guest potwierdza
`PresentationReady` przed uruchomieniem strumienia kamery, a cinematic cut
przeskakuje bez sztucznego opóźnienia interpolacji.

Skip wymaga dwóch głosów w ciągu 5 s. Pojedynczy host albo guest nie może
przypadkowo przerwać sceny. Lokalny aktor Story guesta jest wykrywany w pełnej
puli pedów w promieniu 8 m, więc kontrolki kontekstowe są zamykane przed
uruchomieniem konkurencyjnego skryptu. AnimGraph ma dodatkowo płynne przejście
crouch, 500 ms lease wejścia do cover, fallback cover oraz watchdog slidingu.

V25.0 jest buildem synchronizacji kampanii przygotowanym do następnego testu 2 PC. Każda
sesja ma wspólny, zanonimizowany fingerprint. Bridge publikuje co 1 s
`SESSION_HEALTH`, a zmiany i przekroczenia progów jako `MISSION_TIMELINE`,
`PLAYER_DIVERGENCE`, `ENTITY_DIVERGENCE`, `COMBAT_LIFECYCLE`, `LASSO_LIFECYCLE`
i `MOUNT_LIFECYCLE`. F9 może zapisać ręczny `USER_MARKER` i wymusić bieżące
snapshoty, żeby porównać oba komputery w tym samym momencie.

Nowa ścieżka interakcji nie ufa lokalnym efektom RDR2. Klient wysyła zamiar, a
sidecar hosta sprawdza stabilne ID, rolę, stan nie starszy niż 2 s, odległość oraz
lifecycle. Dopiero `InteractionResult` jest wykonywany przez oba bridge'e. Revive
wymaga ciągłego trzymania interakcji przez 4 s w promieniu 2 m i przywraca 35%
zdrowia. Lasso/hogtie ma jawny, wersjonowany stan oraz czyszczenie po uwolnieniu,
STOP, reconnect i unloadzie. F9 ma hostowo zatwierdzany ratunek awaryjny.

World Mirror nadal ma limit 48 aktywnych węzłów, lecz wybór nie jest kolejnością
przypadkowego skanu: ScriptOwned, Combat i Interactive wygrywają z ambientem,
istniejący węzeł ma histerezę, a aktor misji zachowuje ID przez krótką przerwę
streamingu. Guest rozróżnia `pending model`, `pending dependency`, retryable spawn
i trwały błąd modelu, więc samo ładowanie modelu nie wywołuje migotania encji.

Downed jest obecnie natywną ochroną best-effort: bridge przechwytuje stan krytyczny,
utrzymuje fizyczny ragdoll, odporność oraz ignorowanie przez NPC. Gwarancja
przechwycenia każdego jednostrzałowego lethal hitu nadal wymaga potwierdzenia w
teście 2 PC; jeśli tick silnika okaże się zbyt późny, potrzebny będzie minimalny
hook zdarzenia obrażeń przypięty wyłącznie do obsługiwanego hasha gry.

Sidecar prowadzi ograniczoną telemetrykę per kierunek i typ wiadomości: observed,
delivered, dropped, coalesced, średni/P95/maksymalny odstęp oraz brak pierwszej
oczekiwanej ramki. Zdarzenia `diagnostics.transport-gap` i
`diagnostics.transport-recovered` są emitowane tylko przy zmianie stanu. Logi są
rotowane na dysku jako bieżący plik plus trzy segmenty po 8 MiB. Eksporter scala
je i wybiera tylko dwie najnowsze sesje do skorelowanych `TIMELINE` i `ANOMALIES`
w JSON oraz Markdown bez adresów i sekretów sesji.

Kotwica misji nie jest już zamrażana przy starcie: host publikuje jej kolejne
wersje podczas ruchu. Guest otrzymuje syntetyczny żółty znacznik/status celu hosta;
nie jest to nieprawdziwa kopia tekstu vanilla. W cutscence lokalny gracz, nicki,
konie oraz wadliwe proxy są ukrywane, a świeże snapshoty kamery hosta sterują
odwracalną kamerą guesta. Brak snapshotu przechodzi do follow-camera, a reliable
`MissionCinematicState` prowadzi handshake `PrepareResume/ResumeReady/Completed`.
Timeout 5 s, utrata hosta przez 3 s, STOP i unload wykonują ten sam idempotentny
teardown, który przywraca sterowanie, widoczność, kolizję i kamerę.

Aktorzy `ScriptOwned` są próbkowani do 300 m i zachowywani 15 s podczas krótkich
przerw streamingu. Skryptowy desired move blend zasila lokomocję w obozach, dzięki
czemu custom speed nie powinien już wyglądać jak sliding w pozycji idle.

Walka V22 nie odświeża autonomicznej kombinacji AI: jeden zaakceptowany `Begin`
uruchamia najwyżej jeden ograniczony cios, a `End`, preemption lub deadline czyści
task natychmiast. Action epoch odrzuca obce zakończenia. Zrzucenie z konia jest
victim-owned: komputer ofiary wykonuje jeden `TaskDismountAnimal`, najwyżej jedną
ponowną próbę i potwierdza rezultat lokalnym stanem mounta. Nadal nie gwarantujemy
identycznego klipu ręki/nogi bez odczytu konkretnej warstwy AnimGraph.

V21 dodał niezawodny `MissionState`: hostową epokę misji, rewizję, generację
checkpointu, fazę oraz bezpieczną kotwicę. Sidecar odrzuca stan stary lub sprzeczny,
przechowuje ostatnią wersję i po reconnect odtwarza ją przed grafem świata. Guest
używa spokojnego lokalnego save'a wyłącznie jako powłoki; host jest jedynym
właścicielem prawdziwych skryptów misji i postępu.

Naprawiono krytyczną sprzeczność, przez którą odbiorca odrzucał `EntitySpawn` i
`EntityUpdate` dokładnie po wejściu hosta w `InMission`. World Mirror działa teraz
w fazie Active, zatrzymuje mutacje tylko podczas Cutscene/Loading/Recovery lub
SoloOverride. Hostowi ludzie mają zapasowy klasyfikator `GET_PED_TYPE` (4/5),
aktorzy script-owned są oznaczani w protokole i mają priorytet. Sticky mask guesta
nie powinna naprzemiennie przywracać ukrytych pedów.

Cutscenka i loading hosta sterują odwracalnym spectator mode guesta. Potwierdzony
checkpoint czyści historię interpolacji oraz graf i zwiększa generację. Nowe
kategorie diagnostyczne obejmują `MISSION_FSM`, `MISSION_TX`, `MISSION_RX`,
`MISSION_WORLD`, `MISSION_PREFLIGHT`, `MISSION_SPECTATOR`, `MISSION_CHECKPOINT`,
`MISSION_CAMERA`, `MISSION_OBJECTIVE` i `MISSION_DAMAGE`.

V20.0 dodaje protokół 10 i niezawodny komunikat `PlayerAction` dla celowania,
ataku, bloku, grapple, lassa, hogtie i powalenia. Każda akcja ma stabilne ID,
rewizję, autorytet, cel oraz fazę `Begin/Sustain/End/Cancel`; odbiorca odrzuca
duplikaty i stare rewizje. Opadająca krawędź oraz watchdog 2,5 s jawnie kończą
autonomiczny task walki, więc proxy nie powinno dalej bić po puszczeniu wejścia.

Lasso korzysta z jednej natywnej transakcji `TASK_LASSO_PED` na actionId.
Wykonawca potwierdza przejęcie celu przez silnik, wykonuje najwyżej jedną ponowną
próbę, oddaje taskowi lassa kontrolę nad puppet motorem i sprząta więź przy
End/Cancel/timeout. Jednorazowy ragdoll pozostaje wyłącznie fallbackiem.

Rejestr mountów rozpoznaje `BorrowedPeerMount`: dosiadany koń drugiego gracza
zachowuje oryginalny NetEntityId i lokalny handle zamiast tworzenia duplikatu.
Relacja mount/dismount jest uzgadniana co 250 ms, brak stanu ma debounce 8 s,
a miękka korekcja konia zmienia tylko XY, pozostawiając Z podłożu i IK kopyt.
Maska populacji guesta nie przełącza już ukrytych NPC z powrotem na widoczne.

Eksport launchera tworzy `DIAGNOSTICS_SUMMARY.json`, indeks błędów, ostrzeżeń oraz
podsumowań runtime. Nowe kategorie obejmują `ACTION_TX`, `ACTION_FSM`,
`ACTION_APPLY`, `AIM_POSE`, `LASSO_ROPE`, `VICTIM_CONSTRAINT`, `MOUNT_LOCAL`,
`REMOTE_MOUNT_RX`, `REMOTE_MOUNT_RELATION`, `WORLD_POOL` i
`WORLD_PROXY_PHYSICS`. F9 nadal zawiera lokalny pistolet testowy z maksymalną
amunicją.

V18.1 rozdziela zatrzymanie bieżącej sesji od wyłączenia całego sidecara. STOP
w menu F8 lub F9 wysyła uwierzytelnione `Goodbye`, kończy tylko aktywny transport,
czyści kolejki, role, Entity Graph i repliki po obu stronach, po czym ponownie
otwiera wybór HOST/JOIN. Można utworzyć nową sesję i świeży kod bez restartowania
RDR2. Przycisk `WYŁĄCZ COOP` w launcherze pozostaje pełnym wyłączeniem procesu.

Pierwszy test 2 PC wersji V17 potwierdził płynny transport ruchu, ale ujawnił
trzy niezależne problemy: brak jawnego celu walki/lassa, lokalną populację klienta
przywracaną przez skrypty jego save’a oraz awaryjny freeze po wspólnej pauzie.
V18 wysyła flagi celu peer i lassa w istniejącym stanie protokołu 9, uruchamia
bezobrażeniowe powalenie celu, na krótko izoluje reakcje NPC i przestępstwa,
ponawia maskę lokalnej populacji oraz usuwa niebezpieczny fallback twardej pauzy.
Sesję można teraz zatrzymać i ponownie uruchomić z menu F8/F9 bez wyłączania RDR2.

Protokół 9 dodaje osobne komunikaty `PlayerAnimationState` i lokalny
`MotionReplicationConfig`. Launcher zachowuje dwa całkowicie rozdzielone tryby:

- domyślny `Task/Navmesh`, czyli niezmieniony Marker Lock V13.1;
- opcjonalny `AnimGraph Replica`, który w każdej klatce przypisuje replice
  interpolowaną pozycję i heading, a niezależnym taskiem wizualnym utrzymuje
  działający natywny graf lokomocji bez pathfindingu i navmeshu.

Pierwszy test V14 potwierdził dokładne i płynne odwzorowanie pozycji, ale zdalny
ped pozostawał w T-Pose przez około 99% czasu. Przyczyną było połączenie taskless
ped z `FORCE_PED_MOTION_STATE` oraz korekcji współrzędnych kasującej taski i IK.
V15 zachowuje taski/IK podczas bezpośredniej korekcji korzenia i utrzymuje długi
task chodu/biegu/sprintu wyłącznie jako sterownik grafu wizualnego. Diagnostyka
porównuje teraz oczekiwany ruch z rzeczywistymi `IS_PED_*`/move-blend zamiast
uznawać zwrot z samego wymuszenia motion state za dowód działającej animacji.

Test V15 potwierdził 0 T-Pose oraz płynne stanie, chodzenie, trucht i sprint.
W zdrowych oknach diagnostyka raportowała m.in. 308/308 i 351/351 faktycznie
zaobserwowanych klatek lokomocji, zwykle przy średnim błędzie pozycji 1–5 cm.
Transakcje `PlayerTraversal` dochodziły do bridge'a, ale wczesne rozdzielenie
trybu AnimGraph omijało dotychczasowy wykonawca `TASK_JUMP`/`TASK_CLIMB`.
V16 dodaje osobnego wykonawcę traversal do direct-root: startuje niezawodną akcję
przy jej kotwicy, chwilowo oddaje szkielet natywnej fizyce, a po lądowaniu wraca
do sprawdzonego visual gait drivera. Zwykły upadek bez transakcji dostaje
jednorazowo pozycję i velocity hosta, aby uruchomić natywny stan falling.

Test V16 potwierdził natywne animacje skoku, przeskoku i wspinania bez powrotu
T-Pose. Jedyny wyraźny rozjazd pojawił się przy spamowaniu skoku: 1000 ms pełnego
oddania korzenia fizyce dla kolejnych akcji narosło do maksymalnie 7,62 m, po
czym replika sama wróciła do błędu liczonego w centymetrach. V17 skraca ochronę
zwykłego skoku do 400 ms i dodaje fizyczną smycz 1,5 m. Smycz koryguje wyłącznie
współrzędne; nie zmienia tasku, headingu, IK ani stanu animacji. V17 dodaje też
flagę skradania, inicjację ragdolla oraz lokalne wyszukanie celu dla natywnej
walki wręcz. Diagnostyka raportuje je osobno jako `physical-root-leash-corrections`,
`stealth-*`, `ragdoll-task-starts` i `melee-*`.

Nowy tryb odczytuje przez zweryfikowane native’y stan idle/walk/run/sprint.
Gdy działa skryptowy MoveNetwork, V16.0 nie dereferencjonuje surowego wskaźnika
z SDK i kończy próbkę fail-closed. Protokół ma przygotowane, ściśle walidowane
pola na identyfikator grafu, clipy, fazy, playback rate, blend weights i
przejścia, ale V16.0 nie oznacza ich jako dostępne i nie używa wymyślonych
offsetów pamięci.
Pełny reader wewnętrznych warstw AnimGraphu dla 1491.50 wymaga jeszcze osobnego,
read-only reverse engineeringu. Tryb nie wykonuje cichego fallbacku do
Task/Navmesh, więc oba kontrolery nigdy nie walczą o korzeń tej samej postaci.
Diagnostyka zapisuje osobno `ANIMGRAPH_REPLICA`, `ANIMGRAPH_SAMPLE` i
`NETWORK_MOTION_MODE`.

Poprzedni build `Marker Lock V13.1` zachowuje `Entity Graph V10.1` i używa
protokołu 8 dla semantycznej
synchronizacji gracza. Snapshot przenosi osobno body/movement heading, lokalną prędkość
przód/bok, desired move blend, tryb kontrolera, epokę lokomocji oraz transakcję
skoku/wspinania z ID i punktem startu. Bufor odbiorcy mapuje tick nadawcy na
lokalną oś czasu, mierzy jitter, dobiera 75–160 ms opóźnienia i używa ograniczonej
interpolacji Hermite'a. Grounded puppet nie jest już co klatkę przepychany przez
`SET_ENTITY_VELOCITY`; move-rate ma limit 1,15, a punkt ruchu pochodzi z
krzywiznowo zależnego postępu po trasie. Jawny broker trybów i behawioralny
watchdog pilnują, aby locomotion, aim, traversal, physics, mount i navmesh nie
nadpisywały równocześnie głównego tasku. V13 dodaje osobny, niezawodny komunikat
`PlayerTraversal`: powstaje na krawędzi wejścia jeszcze przed takeoffem, zawiera
prędkość podejścia, rewizję oraz przybliżoną geometrię przeszkody. Odbiorca
sprawdza ją lokalnym capsule probe, chroni ostatnie 3 m podejścia przed navmesh,
czeka na wynik najwyżej 350 ms i dopiero wtedy wykonuje climb albo fallback.

Test V13.0 trwał około 95 sekund. Maksymalny błąd pozycji wyniósł 28,41 m,
a w 11 z 20 pięciosekundowych okien średnia przekraczała 6 m. Pięć timeoutów
navmesh zakończyło się teleportem o 13,30–28,26 m i każdy zmniejszył pozostały
błąd do około 0,96–1,11 m. Przyczyną jakościową nie był transport: apply gap
pozostawał zwykle w granicach 17–26 ms, bez błędów aplikacji. V13.1 wykonuje tę
samą skuteczną korektę wcześniej: po 600 ms utrzymanego błędu 6 m albo po 200 ms
błędu 12 m. Po marker lock czyści historyczną trasę i przeterminowane traversal,
ma dwusekundowy cooldown i nie przerywa fizyki ragdoll/climb ani mounta.

Log czterech replayów V12.0 pokazał zdrowy transport (jitter 4–6 ms i zero
błędów aplikacji), ale także 441 klatek przedwczesnego trybu traversal oraz
awaryjne korekty do starych punktów po 1–3 m przy kolejce przekraczającej 100
punktów. V12.1 nie uznaje samej zdalnej flagi skoku za lokalnie wykonaną akcję,
zachowuje trasę dojścia przy epoce traversal i po wyczerpaniu navmesh wraca do
najnowszego nagranego punktu, czyszcząc zaległy prefiks trasy.

Test V12.1 zawierał pięć transakcji climb. Dwie pierwsze zostały wykonane, ale
następne wygasły po 3 sekundach albo zostały wyczyszczone przez kolejną epokę,
gdy błąd puppeta urósł do 17,81 m. V12.2 zachował kolejkę i wydłużył ważność do
12 sekund. Najnowszy log miał 1017 klatek i cztery rzeczywiste akcje climb;
wszystkie ostatecznie uruchomiły task, lecz część zbyt późno, po wzroście błędu
do 19 m. Transport 20 Hz był zdrowy, więc V13 skraca ważność do 3,5 s i zamiast
dodawać zwykłe checkpointy wysyła reliable pre-takeoff transaction. Próba
move-rate 1,75 z V12.2 nie przyspieszała widocznie animacji, dlatego kod wraca
do udokumentowanego maksimum 1,15 i pozostawia duży catch-up navmeshowi.

Poprzedni build `Ghost Route V11.2` zachowywał `Entity Graph V10.1` i rozszerzał
podsystem ruchu oraz akcji.

V10.2 wykrywa, że widoczny ped przestał zbliżać się do autorytatywnego celu,
buforuje ślad odebranych pozycji i awaryjnie prowadzi puppeta po navmeshu RDR2.
W trybie recovery dozwolone są climb-over i drabiny, a bezpośrednia korekta
velocity jest wyłączona, żeby nie pchała peda przez ścianę. Po dogonieniu na
1,25 m sterowanie wraca do precyzyjnego player puppet motor. Protokół przenosi
również stany skoku i wspinania; test solo wykonuje skok i pokazuje osobny
niebieski marker `CEL NAVMESH` oraz logi `PUPPET_NAV`.

Test IMPORTANTONE w Valentine potwierdził działanie recovery, również po
wejściu puppeta przez drzwi sklepu, lecz ujawnił kolejkę 128 historycznych
punktów i maksymalny błąd 59,12 m po lokalnej interwencji szeryfa. V10.3
ogranicza ślad do 64 punktów i 12 sekund, zeruje go po dogonieniu oraz od 15 m
kieruje navmesh bezpośrednio do aktualnego celu. Recovery pozostaje zapauzowane
podczas ragdolla zamiast wykonywać fałszywy exit/re-enter. Diagnostyka świata
rozróżnia teraz combat-host/combat-guest, shooting i uciekających ludzi/koni.

V10.0 dodał hostowy rejestr świata ze stabilnym `NetEntityId`, rewizjami,
relacjami rodzic–dziecko i deterministyczną kolejnością. Koń jest wysyłany i
tworzony przed jeźdźcem, a usuwany po jeźdźcu. Klient odkłada encję zależną do
czasu pojawienia się rodzica, kaskadowy tombstone blokuje jej wskrzeszenie przez
opóźniony UDP, a sidecar zachowuje desired-state do odtworzenia po reconnect.
Próbkowanie hosta nie obcina już jeźdźca bez jego konia i zapisuje szczegółowe
liczniki odrzuceń w `[ENTITY_GRAPH_SAMPLE]`.

V10.1 utrzymuje twardy budżet 48 węzłów także podczas 750 ms grace period,
więc host nie może już chwilowo wysłać 49–50 encji do rejestru klienta. Host
skanuje do 96 kandydatów i wybiera 48 według ważności: własność skryptu/misji,
walka, blip lub scenario/interakcja, zależność jeździec–koń, a następnie
odległość. Już zsynchronizowana encja otrzymuje 12 m histerezy, świeżo przyjęta
dodatkowe 6 m przez 3 sekundy, a encje interaktywne i skryptowe mogą zachować
stan do 5 sekund, jeśli budżet ma wolne miejsce. Gdy graf jest pełny, najpierw
wysyłany jest child-first `despawn` najmniej ważnej brakującej encji, dopiero
potem `spawn` nowej. Diagnostyka zapisuje `capacity-evictions`,
`selection-deferred`, `grace-retained`, rozkład typów populacji oraz osobne
liczniki script-owned, global-mission, scenario, blip i combat.

V9.5 tworzy zdalnego gracza jako niezależny `CREATE_PED` z bezpiecznym
fallbackiem, inicjalizuje jego outfit/MetaPed, wymusza widoczność i pełną alfę,
używa stabilnego nameplate'u bez awaryjnego natywu MP gamer-tag oraz usuwa
`auto-rebase` wywoływany przez zwykły lokalny drift. Korekcje są ograniczone
prędkością, a utknięty task zostaje zastąpiony bez natychmiastowego kasowania
grafu animacji i bez przestawienia współrzędnych na cel. Lokalny syntetyczny
peer wykonuje ciągły kurs idle/walk/run/sprint/turn/reverse opisany w
[TEST_SOLO_V8.md](TEST_SOLO_V8.md).

Po teście V8.2 wycofano wszystkie korekty `SET_ENTITY_COORDS` podczas zwykłego
ruchu. Log potwierdził płynny transport, lecz około 40 lokalnych korekt na pięć
sekund sprintu było bezpośrednią przyczyną widocznych teleportów. V8.3 zostawia
transform ciągły taskowi lokomocji; snap pozostaje wyłącznie dla spawnu albo
rzeczywistego skoku autorytatywnego celu o co najmniej 25 metrów. Natywny
gamer-tag wycofano, ponieważ jego sprzątanie powodowało wyjątek ScriptHooka
dokładnie pięć sekund po zatrzymaniu testu.

Prawdziwy test V8.3 przez ponad trzy minuty potwierdził zero warpów, zero
korekt współrzędnych, zero błędów zastosowania snapshotu i bezpieczny despawn.
Puppet reagował na strzały, dawał się związać lassem, a po uwolnieniu wracał
do kursu. Log ujawnił jednak lokalny dryf do 9,57 m: dwunastometrowy punkt
wyprzedzenia tasku był zbyt odległy na zakrętach, a zatrzymany cel wybierał
`Idle` nawet wtedy, gdy proxy pozostawało kilka metrów za nim. V8.4 skraca
predykcję do 0,35 s i maksymalnie 2 m oraz dodaje piesze nadrabianie trzema
strefami chód/bieg/sprint z ograniczeniem tempa ruchu do 1,35x. Nie przywraca
to żadnych ciągłych wywołań `SET_ENTITY_COORDS`.

Test V8.4 potwierdził, że samo `SET_PED_MOVE_RATE_OVERRIDE` nie wystarcza:
po lasso albo ostrym zakręcie różnica potrafiła utrzymywać się w okolicy 7–8 m,
mimo że sieć nadal miała zero błędów i zero warpów. V9.1 dodaje widoczny tylko
dla syntetycznego peera marker `CEL SIECI`, kolorową linię do puppeta i dystans
w metrach. Dodatkowy regulator ustawia płynną prędkość fizyczną proporcjonalną
do wektora błędu z limitem 12 m/s; ragdoll, upadek i wstawanie zawsze pozostają
pod kontrolą gry. Log z 1 sierpnia wykazał, że silnik potrafił wyzerować velocity
przed kolejną klatką, więc stary limiter przyspieszenia zatrzymywał pomoc na
0,3–1,2 m/s mimo błędu 7–15 m. V9.1 ponawiał bezpośrednio ograniczoną korektę,
`SET_PED_MOVE_RATE_OVERRIDE` i `SET_PED_MAX_MOVE_BLEND_RATIO` co klatkę. Log V9.1
potwierdził skuteczność, ale również skok do 12 m/s, tempo 3× i 6–10 restartów
tasku na pięć sekund. V9.2 zachowuje poleconą velocity między klatkami, ogranicza
jej przyspieszenie do 30 m/s², filtruje narastanie move-rate i odświeża task
rzadziej. Grant broni jest blokowany podczas ragdolla/lassa oraz ma pięć sekund
cooldownu, co zapobiega tworzeniu stosu rewolwerów. Osobny kurs akcji nadaje
stan rewolweru, amunicji,
celowania, sześciu kierunków strzału i przeładowania niezależnie od kursu ruchu.

Log V9.2 ujawnił ostatni konflikt: podczas `stop-and-turn` i `reverse-run`
połączony task chodzenia z celowaniem potrafił skierować nogi przeciwnie do
markera, mimo poprawnego strumienia pozycji. V9.3 domyka rozdzielenie ruchu i
akcji. Gdy lokalny błąd wynosi co najmniej 1,25 m, korzeń postaci podąża wyłącznie
do celu sieciowego, a jego heading pochodzi z wektora do tego celu. Połączone
celowanie odzyskuje sterowanie dopiero poniżej 0,5 m. Histereza zapobiega
przełączaniu tasku co klatkę; zdarzenia broni, strzału i przeładowania nadal są
obsługiwane niezależnie. Punkt 3 ma kompletną ścieżkę implementacyjną i testy
regresji, ale wymaga jeszcze potwierdzenia wizualnego w grze i później na 2 PC.

Log V9.3 z 1 sierpnia pokazał poprawny transport (`target-gap` zwykle poniżej
0,15 m i `apply-gap` 15–25 ms), lecz lokalny błąd cyklicznie utrzymywał się na
7–10 m. Przyczyną był nieruchomy cel natywnego tasku: następne miejsce było
zadawane dopiero po zmianie o 3 m, więc przy chodzie proxy dobiegało do starego
punktu, stawało i dopiero później sprintowało. V9.4 aktualizuje cel po 0,5 m i
nie częściej niż co 450 ms. Małe błędy pozostają pod kontrolą animowanej
lokomocji; fizyczny regulator ma histerezę 1,5/0,5 m, łagodniejsze przyspieszenie
i maksymalne tempo animacji 2,25×. Podczas ragdolla, upadku albo lassa nie jest
uruchamiany task chodzenia, ale autorytatywna korekta velocity obejmuje X/Y/Z,
aby lokalna fizyka nie pozostawiła ciała na innej krawędzi niż u źródła.

Log V9.4 potwierdził, że transport i cel sieciowy działały poprawnie nawet
podczas jednorazowego błędu 14,39 m (`apply-gap` 18–19 ms). Źródłem był test:
syntetyczny marker nadal sprintował, gdy pełny task przeładowania przez około
cztery sekundy blokował nogi puppeta. V9.5 sprzęga oba zegary — przeładowanie
zamraża marker i jego velocity, a po zakończeniu trasa jest kontynuowana bez
skoku. Zdarzenie `fireSequence` uruchamia również przestrzenny, lokalny odgłos
strzału z wbudowanego soundsetu RDR2, nadal obok zerowo-obrażeniowego pocisku.

Launcher ma kartę `Test solo` i jeden kontekstowy, okrągły `START`. Sidecar
tworzy bezpieczny peer loopback, ale nie emituje bota do chwili wybrania
`Test solo: start / stop` w menu F9. Bot publikuje identyfikator `SOLO BOT`.
Eksport diagnostyki scala rotacje, ale obejmuje wyłącznie dwie najnowsze sesje
oraz dodaje `DIAGNOSTICS_INDEX.txt` z kategoriami błędów, ostrzeżeń, ruchu
puppeta, sieci, świata i testu solo.

To nadal nie jest coop kampanii. Host jest źródłem wybranych elementów
free-roam, ale oba komputery uruchamiają osobny save i osobne skrypty Story
Mode. Nie ma technicznej podstawy do twierdzenia, że guest siedzi na save'ie
hosta albo ma identyczne triggery misji.

## Potwierdzone w prawdziwym teście dwóch PC

Poprzednia próba HOST + GUEST przez prywatną sieć Hamachi potwierdziła:

- poprawne uwierzytelnienie sesji i stan `REMOTE STREAMING`;
- spawn zdalnego proxy po obu stronach;
- guest → host: `20 492 / 20 492` dostarczonych snapshotów;
- host → guest: `19 150 / 19 150` dostarczonych snapshotów;
- brak odrzuceń polityki UDP;
- typowe tempo około 20 snapshotów gracza na sekundę;
- około 17 minut i 33 sekundy aktywnego gameplayu;
- działający handshake TCP oraz dwukierunkowy strumień UDP przez Hamachi IPv4.

Test V4 ponownie potwierdził transport około 20 Hz. Animacje ruchu były
wyraźnie lepsze, czas hosta działał, głosowanie pauzy działało, a odrodzenie
zostało wykryte po obu stronach. Logi ujawniły jednak do 20 restartów tasku
lokomocji na 5 sekund sprintu. Host w ostatniej sesji wybrał tylko 4 encje,
choć klient utworzył ich proxy; blanket `IS_ENTITY_A_MISSION_ENTITY` pomijał
zwykłych mieszkańców zarządzanych przez population manager i nie ukrywał ich
lokalnych odpowiedników. Test nie potwierdził kampanii, wspólnego save'a ani
hostowego AI.

## Przyczyny problemów poprzedniej wersji

- Stary proxy używał zadania AI i wykonywał twardą korektę przy błędzie około
  8 m, dlatego postać okresowo stała, a następnie przeskakiwała.
- Callback odbioru sieci czekał na zapis do Named Pipe. Gdy RDR2 przestawało
  opróżniać pipe podczas pauzy albo loadingu, blokował również heartbeat i
  następne snapshoty.
- Każdy komputer nadal symulował własny czas, pogodę, ambientowe NPC i skrypty
  misji.
- Proxy gracza było klonem lokalnego peda, dlatego wygląd drugiego Arthura nie
  odpowiadał rzeczywistemu outfitowi znajomego.

## Host World / Mount / Semantic Tasks Alpha V6 — zaimplementowane do walidacji

Poniższe funkcje są przygotowane w kandydacie V6, ale do czasu nowego testu
obu komputerów mają status **NOT TESTED IN 2PC GAMEPLAY**:

- oddzielenie odbioru sieci od Named Pipe przez ograniczoną kolejkę, która
  zachowuje zdarzenia krytyczne, a dla szybkich stanów trzyma najnowszy wpis;
- ruch proxy ma rzadsze restarty tasków, korektę pozycji ograniczoną
  cooldownem i połączony task chodzenia/celowania;
- nick korzysta z autorytatywnej pozycji bez head-bob i znacznie mocniejszego
  filtra osi pionowej, a blip pozostaje przyjazny;
- replikacja aktualnej broni, kierunku celowania i wizualnych strzałów;
- wizualne PvP gracz kontra gracz z zerowymi obrażeniami;
- host jako źródło godziny, daty i pogody guesta;
- wymuszanie rzeczywistego tempa podczas aktywnego streamu, aby lokalny
  Dead Eye/focus nie spowalniał peerowi napływu stanów;
- wspólne głosowanie pauzy i wznowienia pod `Escape`: po zgodzie bridge
  wstrzykuje natywną kontrolkę frontendową, aby otworzyć menu pauzy RDR2;
- eksperymentalna flaga `MeleeCombat` uruchamia wizualny task walki proxy,
  a jego grupa relacji nie może zadawać obrażeń lokalnemu graczowi;
- World Mirror skanuje do 96 kandydatów i utrzymuje maksymalnie 48 hostowych
  ludzi i mountable koni w promieniu 80 m, aktualizowanych do 10 Hz;
- priorytetowa selekcja chroni encje skryptowe/misyjne, walczące, oznaczone
  blipem i używające scenario, a 12 m histerezy ogranicza churn na granicy;
- próbkowanie wysokopoziomowej kategorii tasku: idle, locomotion, scenario,
  fleeing, combat, mounted i dead, wraz z celem ruchu/walki;
- osobny dwukierunkowy `PlayerMountState` własnego konia hosta i guesta:
  model, transform, prędkość, heading, zdrowie, śmierć oraz mounted;
- zdalny gracz jest natywnie osadzany na swojej replice konia i zsiada po
  zmianie stanu; hostowi jeźdźcy NPC wskazują stabilny `parentEntityId`;
- lokalny własny koń guesta nie jest ukrywany i nadal może być wołany przez
  vanilla grę, a jego stan jest publikowany peerowi po wykryciu właściciela;
- stabilne `NetEntityId`, spawn/update/despawn proxy oraz sprzątanie po
  rozłączeniu, resyncu i wyjściu z dozwolonego free-roam;
- World Mirror guesta jest sterowany stanem misji hosta, a nie jego własnymi
  lokalnymi markerami; odbiór hostowych spawnów nie jest przez nie odrzucany;
- aktywna misja hosta ustawia na gueście odwracalną flagę „mission busy”,
  aby lokalnie nie uruchamiał konkurencyjnej misji; po rozłączeniu lub końcu
  misji bridge przywraca flagę;
- launcher hosta wybiera dowolny lokalny plik `SRDR*`, liczy jego SHA-256 i
  zapisuje wyłącznie metadane. Nie kopiuje, nie wysyła i nie nadpisuje save'a;
- wykrycie powrotu vanilla gracza po śmierci/checkpoincie odtwarza lifecycle
  `Alive` i ponownie publikuje widoczne proxy;
- eksperymentalny `DamageIntent`: guest może zgłosić trafienie proxy
  ambientowego NPC, ale host waliduje encję i żądanie przed zmianą zdrowia
  prawdziwego NPC;
- lokalny `Teleport do gracza` obu ról i hostowe `Przywołaj guest (host)`;
- ponowna publikacja tożsamości, ekwipunku i stanów po reconnect/resync.

World Mirror nie replikuje pełnych task trees. Proxy odtwarza model, transform,
zdrowie, broń, relację z koniem i wybraną kategorię zachowania. `Scenario`
jest obecnie bezpiecznym stanem postoju, a `Combat` replikuje wybrany cel i
wizualne celowanie/strzały, nie dokładny harmonogram AI, dialog ani AnimScene.

## Granice autorytetu hosta w V6

Host jest autorytetem:

- czasu, daty i pogody;
- listy wybranych ambientowych encji objętych World Mirror;
- ich stabilnych identyfikatorów, kategorii tasku, relacji mount–rider,
  podstawowego stanu, zdrowia i śmierci;
- zatwierdzenia albo odrzucenia `DamageIntent` guesta;
- hostowych poleceń sesji, resyncu i przywołania guesta.

Host nie jest jeszcze autorytetem wspólnej instancji skryptów kampanii.
Lokalny marker, dostępność misji, checkpoint i stan cutscenki mogą nadal
różnić się między komputerami. Flaga „host ma aktywną misję” jest blokadą
startu, nie synchronizacją grafu misji.

## Czego nadal nie ma

- **Wspólny runtime save'a:** host wybiera plik źródłowy, ale nie przesyła
  guestowi save'a ani działającego stanu postępu kampanii.
- **Skrypty misji:** brak wspólnego graphu triggerów, celów, blipów,
  checkpointów, dialogów i cutscenek.
- **Outfit / MetaPed:** brak pełnego stroju, twarzy, włosów, budowy ciała
  i pozostałych danych wyglądu gracza oraz dokładnych outfitów NPC.
- **Task trees NPC:** są kategorie semantyczne, ale brak pełnej synchronizacji
  AI, dokładnego scenario, harmonogramów, animacji, decyzji bojowych i
  AnimScene.
- **Konie:** jest własny koń gracza i relacja mount–rider, lecz brak shared
  mount, więzi, stajni, bagażu i pewnego odtworzenia wszystkich końskich
  animacji/checkpointów.
- **Pełna walka:** `DamageIntent` nie jest kompletnym hit detection; brak
  wszystkich rodzajów obrażeń, hostowego AI walki oraz gotowego revive/retry.
- **Cały świat:** brak pełnej synchronizacji drzwi, obiektów, wozów, pociągów
  i wszystkich pojazdów.
- **Kampania:** prolog, misje i epilog nie mają statusu grywalnego coop.

V6 należy najpierw testować w spokojnym free-roam. Typ populacji 7 oraz
nieznane typy niemisyjne nie są przejmowane przez World Mirror. Test misji
ma ocenić wyłącznie encje, blokadę lokalnego startu i stabilność — nie jest
potwierdzeniem wspólnej kampanii.

## Launcher, instalacja i ScriptHook

Launcher obsługuje Steam i Rockstar Games Launcher, wybór własnego `RDR2.exe`,
lokalnego pliku `SRDR*` hosta, osobny folder ScriptHooka, nick, LAN/Hamachi,
instalację, start HOST/JOIN, bezpieczne odinstalowanie i eksport diagnostyki.
Wybór save'a nie jest natywnym loaderem: host nadal wybiera odpowiadający slot
w menu RDR2.

Paczka projektu nie zawiera:

- `ScriptHookRDR2.dll`;
- `dinput8.dll`;
- `NativeTrainer.asi`;
- SDK, assetów Rockstar ani innych cudzych modów.

Każdy tester pobiera Script Hook RDR2 osobno. Launcher może skopiować
zweryfikowane pliki runtime tylko z folderu wskazanego przez użytkownika.

Jeśli instalacja albo deinstalacja zwróci `Access denied`:

1. zamknij RDR2 i launcher;
2. uruchom `CoopStory.Launcher.exe` jako administrator;
3. wykonaj tylko instalację albo deinstalację;
4. zamknij podniesiony launcher;
5. do gry uruchom launcher ponownie normalnie przez `URUCHOM_COOP.bat`.

## Historyczny snapshot kryteriów V6 (nieaktualny)

Poniższa tabela i bramka są zachowane wyłącznie jako zapis etapu V6. Aktualny
stan V31.4 oraz bieżące ograniczenia znajdują się na początku tego dokumentu.

| Kryterium | Stan |
|---|---|
| Bezpieczny build, version gate i blokada RDO | **PASS LOKALNY** |
| IPC bridge ↔ sidecar | **PASS** |
| Prywatna sesja dwóch PC przez Hamachi | **PASS** |
| Dwukierunkowe snapshoty gracza 20 Hz | **PASS TRANSPORTU** |
| Spawn zdalnego proxy | **PASS** |
| Stary ruch z korektą około 8 m | **FAIL JAKOŚCIOWY — PRZYCZYNA ZNANA** |
| Ruch proxy / aim podczas ruchu | **IMPLEMENTED / NOT TESTED 2PC** |
| Kolejka bez backlogu Named Pipe | **IMPLEMENTED / NOT TESTED 2PC** |
| Nick / nametag / blip | **IMPLEMENTED / NOT TESTED 2PC** |
| Broń / aim / wizualne strzały | **IMPLEMENTED / NOT TESTED 2PC** |
| Wizualne PvP bez obrażeń | **IMPLEMENTED / NOT TESTED 2PC** |
| Hostowy czas / data / pogoda | **IMPLEMENTED / NOT TESTED 2PC** |
| Głosowanie i natywne menu pauzy RDR2 | **IMPLEMENTED / NOT TESTED 2PC** |
| World Mirror 48 ludzi/koni / 80 m / do 10 Hz | **ALPHA / NOT TESTED 2PC** |
| Semantyczne taski NPC | **ALPHA / NOT TESTED 2PC** |
| DamageIntent guest → ambientowy NPC hosta | **ALPHA / NOT TESTED 2PC** |
| Teleport lokalny obu ról i host pull | **IMPLEMENTED / NOT TESTED 2PC** |
| Outfit / MetaPed | **NOT IMPLEMENTED** |
| Pełne task trees NPC | **NOT IMPLEMENTED** |
| Lokalny koń guesta pozostaje callable | **IMPLEMENTED / NOT TESTED 2PC** |
| Sieciowy własny koń i mount–rider | **ALPHA / NOT TESTED 2PC** |
| Shared mount / stajnia / więź | **NOT IMPLEMENTED** |
| Wybór i hash lokalnego save'a hosta | **IMPLEMENTED / NIE ŁADUJE AUTOMATYCZNIE** |
| Wspólne skrypty misji i save hosta | **NOT IMPLEMENTED** |
| Kampania prolog–epilog | **NOT TESTED / NIE JEST CELEM V6** |

## Historyczna bramka V6 (zastąpiona testem V31)

Następna próba ma wykonać scenariusz z [TESTING.md](TESTING.md):

1. oba komputery instalują ten sam świeży build i osobno wskazują ScriptHook;
2. sesja dochodzi do `REMOTE STREAMING`;
3. testerzy sprawdzają ruch, broń/aim, PvP bez obrażeń, czas/datę/pogodę,
   blokadę Escape, Dead Eye, World Mirror, `DamageIntent` i teleporty;
4. po free-roam host może aktywować jedną prostą misję wyłącznie w celu
   sprawdzenia flagi startu i mirrorowanych encji;
5. host i guest osobno eksportują diagnostykę;
6. dopiero analiza obu ZIP-ów może nadać funkcjom V6 status `PASS 2PC`.

## V26.1 — hotfix ruchu i zakleszczenia lassa

- Automatyczna synchronizacja zegara nie była przyczyną awarii ruchu: transport
  używa monotonicznego TickCount, a widoczne resety osi czasu zostały odzyskane.
- Usunięto wywoływanie `_SET_PED_CROUCH_MOVEMENT(FALSE)` w każdej klatce. Native
  działa teraz wyłącznie na semantycznym przejściu crouch on/off, dzięki czemu nie
  resetuje lokomocji ani celowania zdalnego proxy.
- `Lasso/End` jest rozwiązywane po actor + actionId również z targetEntityId=0 i
  bez PhysicalTargetEffect, czyli dokładnie tak, jak emituje je RDR2 po utracie celu.
- `Lasso/Sustain` wysyła heartbeat tej samej rewizji. Oba bridge'e mają niezależny
  fail-safe 3,5 s, który zwalnia ragdoll po utracie heartbeatów.
- Test protokołu odtwarza targetless terminal i potwierdza przejście Lassoed → Free.
  Pełny test dwóch PC nadal jest wymagany.

## V26.0 — poprawki po diagnostyce HOST/GUEST z 2026-08-06

- Korelacja zegarów wykazała około 32,5 s różnicy czasu systemowego, ale około
  70 ms czasu dostarczenia sterowania cutscenki. Nie był to backlog sieciowy.
- Usunięto zerowanie timera `PrepareMissionCinematicResume`: anchor wznowienia
  nie jest już współdzielony z ruchomym focusem kamery spectator.
- Host używa bariery `PrepareResume` i czeka na `ResumeReady` aktywnego guesta;
  nie wymusza już skażonego wznowienia po pięciu sekundach.
- W czasie sceny World Mirror hosta pozostaje aktywny do 10 Hz. Guest maskuje
  lokalną obsadę save'a, ale utrzymuje hostowe proxy pod replikowaną kamerą.
- Crouch korzysta z natywnego grafu kucania RDR2, a cover ma pojedyncze wejście,
  jedną kontrolowaną próbę ponowną i niedestrukcyjne podtrzymanie.
- Zdalny downed, semantyczny ragdoll oraz jawny stan Lassoed/Hogtied są
  podtrzymywane aż do potwierdzonego zwolnienia, zamiast jednego wywołania.
- SEH zapisuje teraz etap ticka. Po fatalnym native AV pomijane jest zarówno
  sprzątanie runtime, jak i natywny destruktor facade, aby drugi AV nie usunął
  diagnostyki ani nie uciekł do ScriptHooka.
- Walidacja lokalna: bridge 53/53, protokół/sidecar 40/40, launcher 28/28,
  CTest 1/1. Test dwóch PC pozostaje wymagany.
