# RDR2 Coop Story — prosta instrukcja

Aktualny build to **Cutscene Mission Ownership V31.9 Alpha**. Host jest jedynym
właścicielem save'a i skryptów kampanii. Protokół 20 przesyła stan misji, kamerę,
hostowy graf NPC/koni oraz ograniczony graf rekwizytów aktywnej AnimScene. Poza
misją bridge nie zapisuje procesu-globalnego `MISSION_FLAG`, więc M2/rozmowa i
zwykłe prompty pozostają własnością gry. Od pierwszego stanu autorytatywnej misji
hosta do jej terminala guest zajmuje lokalny slot Story, aby spóźniona prywatna
misja nie uruchomiła drugiej cutscenki ani `Gang Abandoned`.

Przed testem cutscenki guest sprawdza M2 i rozmowę z dwoma zwykłymi NPC, potem
wsiada na własnego konia, gdy host siedzi na swoim. Żadna z tych czynności nie
może zrzucić drugiego gracza. W poprawnym logu nie ma `PEER_DISMOUNT` przy samym
wejściu na własnego konia; możliwy jest wpis `MOUNT_INPUT_ISOLATION`, oznaczający
odrzucenie nakładającego się inputu kontekst/melee.

V31.9 zachowuje komplet 7/7 handlerów potwierdzony przez poprzedni probe i
używa kontynuacji, która nie niszczy `RAX` oryginalnego
`SET_ANIM_SCENE_ENTITY`. To naprawia pustą obsadę hosta widoczną na nagraniu,
gdy kamera, napisy i lewitujące rekwizyty działały bez aktorów.
Domyślnie wyłączony checkbox `STORY VM CAPTURE` obok `ANIMGRAPH REPLICA` może na
przypiętym buildzie przechwycić prawdziwy zasób, playback, flagi i role Story VM
hosta. Pedy, konie i scene-local rekwizyty dostają stabilne tożsamości; w
`cutscene@ODR1_INT` oczekiwane jest 14 wymaganych aktorów oraz opcjonalne,
zwolnione już role `p_*`/`w_*`. Definicja przechodzi przez
`GuestReady -> HostPlayCommit`, a guest tworzy
wyłącznie własną bridge-owned AnimScene. Każdy niezgodny adres/prolog, brak roli,
modelu albo timeout wraca do `SAFE_FALLBACK`; nie są usuwane sceny należące do
gry. Związani aktorzy oddają root motion/IK dokładnej scenie, więc AnimGraph i
World Mirror nie mogą równocześnie teleportować Dutch/Hosea/koni. Bound cast
pozostaje widoczny, a spóźniona faza dogania hosta w bounded oknie.

Najnowsze logi wykazały, że V31.7 poprawnie przesłała 22 role, lecz guest
utknął na `resource-loaded=0`, ponieważ próbował załadować pustą scenę i wiązał
aktorów dopiero po LOAD. V31.9 nie mutuje game-owned sceny hosta i odtwarza
bezpieczną kolejność zależności: CREATE, przypisanie znanej obsady, LOAD, ponowna
weryfikacja wszystkich wymaganych ról i dopiero START. Nadal nigdy nie
przyspiesza prywatnej Story VM i
co klatkę utrzymuje nad nią kamerę kwarantanny. Mount działa w rytmie
50 ms z osobnymi breadcrumbami crashu, a lasso odbiorcy nie startuje od samego
Begin/namierzenia. Test wykonaj dwa razy: oba PC z Capture OFF, potem oba PC z
Capture ON, dokładnie według `TEST_V31_ANIMSCENE_HYBRID.md`. W V31.9 lasso nie
jest częścią tego testu — pełny fokus to cutscenki i przepływ wspólnej misji. Exact capture jest
eksperymentalny i nadal wymaga potwierdzenia w żywej grze.

V30.3 dodaje trzy poprawki z pełnych logów dwóch komputerów. Po `SAFE_FALLBACK`
guest widzi stabilizowaną, niekolizyjną obsadę hostowych proxy zamiast pustego
kadru; dokładne gesty i lipsync istnieją wyłącznie przy `ATTACHED`. Pięciosekundowy
sondaż po końcu sceny hosta wykrywa i pomija opóźnioną lokalną AnimScene guesta.
Lasso wcześniej wskazuje peer z promienia kamery i nie restartuje starego tasku
od samego spóźnionego `Sustain`.

V30.1 zachowuje korelowaną diagnostykę obu komputerów. Bridge zapisuje co sekundę
stan sesji, misji, graczy i grafu encji oraz pełny cykl walki, lassa i mounta.
Sidecar liczy osobno dla każdego rodzaju wiadomości ramki odebrane, dostarczone,
odrzucone i scalone, opóźnienia między ramkami, percentyle oraz moment utraty i
odzyskania strumienia. `F7` zapisuje znacznik problemu bez otwierania menu. Od V30.1
wystarczy nacisnąć go na jednym komputerze: ten sam identyfikator dociera do drugiej
strony, a oba bridge'e przez 15 sekund zapisują próbki co 500 ms. Jeżeli obaj gracze
zauważyli różne objawy, mogą nacisnąć F7 osobno. Krótki komunikat w prawym dolnym
rogu potwierdzi zapis lub odebranie markera. Po markerze nie kończ gry przez co
najmniej 15 sekund. Eksport tworzy `TIMELINE.md/json`, `ANOMALIES.md/json` oraz
`MARKER_WINDOWS.md/json` z kontekstem 10 sekund przed i 15 sekund po F7. Surowe
logi i wszystkie analizy obejmują wyłącznie dwie najnowsze sesje wskazane w
`DIAGNOSTICS_SESSIONS.json`; adresy, tokeny i prywatne ścieżki nadal są usuwane.

V30.2 poprawia dwa problemy wykazane przez nagranie SOLO, które dotyczą tej samej
warstwy odbiorczej prawdziwego co-opu. Stojący zdalny strzał uruchamia teraz
bezpieczny natywny impuls grafu broni, a cover/crouch odzyskują stan po tasku,
który go skasował. Przejście stealth→cover nie wyłącza już crouch w tej samej
klatce. W logu `ANIMGRAPH_REPLICA` sprawdzaj liczniki `fire-graph-pulses`,
`fire-ammo-restores`, `cover-reacquires`, `cover-fallback-recoveries` oraz
`stealth-recoveries`.

V29.2 zachowuje kamerę V28 i dodaje dwa kanały. Pełny, uporządkowany zestaw
shop-componentów MetaPed odtwarza rzeczywisty strój drugiego gracza. Host wysyła
też sygnaturę aktywnej AnimScene. Jeżeli guest ma zgodną lokalną scenę, mod
wyrównuje origin/fazę/rate i używa jej oryginalnej kamery, animacji, rozmów,
napisów oraz audio. Jeżeli zgodnej sceny nie ma, działa bezpieczny fallback V28;
diagnostyka zapisze `ANIMSCENE_REPLICA SAFE_FALLBACK` zamiast ryzykować crash.
V29.1 naprawiła dostarczenie snapshotów AnimScene przez UDP oraz wykonuje
zaakceptowany przez dwie osoby skip jednocześnie w obu lokalnych scenach RDR2.
Lokalny aktor Story guesta ma straż interakcji od 20 m; marker może pozostać
widoczny, ale nie powinien uruchomić drugiej kopii misji.
V29.2 wybiera po stronie hosta tylko scenę posiadającą aktywną kamerę, ogłasza
`SAFE_FALLBACK` dopiero po pełnym skanie i ogranicza recovery skip późnej sceny
guesta do jednego okna 2500 ms.

V29.3 przenosi podstawowe HOST/JOIN do launchera. Małe lobby pokazuje nicki,
czerwonego HOST-a, niebieskiego GUEST-a oraz ping w ms. Panel F8 nie otwiera się
już po starcie Story Mode i służy tylko awaryjnie. F9 ma dwie kolumny — częste
akcje po lewej i narzędzia testowe po prawej.

V29.4 usuwa widoczny kod R2C1. Host po kliknięciu `HOSTUJ` wpisuje i powtarza
hasło, a guest po kliknięciu `DOŁĄCZ` podaje IPv4 hosta oraz to samo hasło.
Komunikat `HASŁO ZAPISANE` potwierdza przygotowanie uwierzytelniania; samo hasło
nie trafia jawnie do ustawień ani logów. Save hosta wybiera się teraz w
`Ustawieniach`. F8 nie zawiera już HOST/JOIN.

V29.5 próbowała wykorzystać waniliowy stan markera jako widoczną blokadę; logi
V30.2 wykazały, że nie jest on wiarygodnym wskaźnikiem. Panel wspólnego celu
pozostaje, a lasso nie używa sztucznego ragdolla. Zdalna postać wybiera prawdziwe
lasso już podczas celowania, a celowany Begin uruchamia natywny task przed
potwierdzeniem złapania.

V29.6 poprawia błędy z pełnego testu V29.5. Zgodna AnimScene może zostać raz
wznowiona, a fallback nigdy nie pokazuje hostowych proxy NPC bez lokalnego grafu
sceny. Po cutscence utrata sterowania podczas ładowania nie uruchamia już pętli
kwarantanny. Niewidoczne konie z hostowej puli nie są wysyłane przy JOIN. Cios
pozostaje aktywny do kontaktu, lasso zachowuje task do terminala i fizyczna lina
ma pierwszeństwo przed korekcją pozycji. Hasło może mieć od 4 do 64 znaków.

V30.0 naprawia przesuniętą o 90° konwencję kierunku ruchu, rozpoznaje osiem
kierunków lokalnych, odświeża graf przy strafe/backpedal i uruchamia natywny
obrót stóp zamiast skokowego headingu stojącej repliki. Włącza też lokalne IK
kończyn/głowy/tułowia oraz przesyła stan wody, pływania i zanurzenia. Dokładny
clip ID i faza klipu nadal nie są odczytywane; `normalizedPhase` akcji jest
zegarem transakcji, nie fałszywą fazą animacji. Użyj checklisty
`TEST_V30_ANIMGRAPH.md`, a potem wykonaj obowiązkową regresję cutscenki V29.6.

Podczas aktywnej misji guest dostaje żółty znacznik towarzysza/celu hosta i prosty
status moda. To bezpieczny, uniwersalny odpowiednik celu — nie kopia tekstu z
lokalnego mission VM. Podczas cutscenki lub ładowania guest jest automatycznie
ukrywany i zamrażany, host przesyła pozycję/obrót/FOV renderowanej kamery, a po
zakończeniu guest wraca obok hosta. Brak świeżej klatki kamery uruchamia bezpieczny
fallback śledzący hosta zamiast pozostawienia zablokowanego widoku.

V22 zachowuje poprawkę V21, przez którą host widzi i prawidłowo klasyfikuje pulę
NPC. Ludzie są klasyfikowani drugim, niezależnym
native'em `GET_PED_TYPE`, a script-owned aktorzy misji mają najwyższy priorytet,
zasięg 300 m i dłuższą retencję podczas streamingu. Lokalne NPC guesta są maskowane,
a hostowy graf wraca automatycznie po cutscence. Stan misji jest buforowany przez
sidecar i po reconnect wraca przed grafem świata. Diagnostyka rozdziela m.in.
`MISSION_ISOLATION`, `MISSION_CAMERA`, `MISSION_OBJECTIVE`, `MISSION_WORLD`,
`MISSION_SPECTATOR`, `ACTION_EPOCH`, `MELEE_VISUAL` i `PEER_DISMOUNT`.

W `Ustawieniach` można wybrać
drugi, eksperymentalny silnik ruchu `ANIMGRAPH REPLICA`. Po zaznaczeniu zdalna
postać nie korzysta z pathfindingu ani awaryjnego navmeshu: jej korzeń odwzorowuje
interpolowany transform sieciowy, a osobny, lokalny task wizualny utrzymuje natywny
graf chodu/biegu/sprintu. Korekcja pozycji zachowuje taski i IK zamiast zerować
szkielet przy każdej klatce.

V20 przesyła atak, blok, grapple, celowanie, lasso i powalenie jako niezawodne,
wersjonowane transakcje `Begin/Sustain/End/Cancel`. Koniec ciosu jawnie przerywa
autonomiczny task walki, a watchdog czyści akcję, gdy zaginie jej zakończenie.
Lasso używa jednej natywnej liny RDR2 na `actionId`, czeka na potwierdzenie silnika
i nie konkuruje już z kontrolerem ruchu. Współdzielony koń zachowuje ten sam
identyfikator po obu stronach, więc dosiadanie konia drugiego gracza nie powinno
tworzyć dwóch koni w jednym miejscu. Korekcja konia pozostawia wysokość Z lokalnej
fizyce i IK kopyt. Diagnostyka tworzy dodatkowe podsumowania akcji, mountów,
świata i fizyki.

V18 zachowuje potwierdzoną lokomocję i traversal V17 oraz dodaje jawny cel
walki między graczami, replikowane bezobrażeniowe złapanie lassem, krótką izolację
reakcji NPC/przestępstwa, pewniejsze ukrywanie lokalnej populacji guesta i
bezpieczne głosowanie pauzy bez twardego freeze. V18.1 dodaje w menu F8 przycisk
`ZATRZYMAJ SESJE`: kończy tylko bieżące połączenie, czyści repliki i po obu
stronach wraca do HOST/JOIN. Można od razu zhostować ponownie bez restartu gry.
Menu F9 ma to samo polecenie. `WYŁĄCZ COOP` w launcherze wyłącza cały sidecar;
po nim trzeba ponownie użyć START.

V16 zachowuje potwierdzoną lokomocję V15 i dodaje fizyczny hand-off skoku,
przeskoku/wspinania oraz spadania. Transakcja traversal uruchamia natywny
`TASK_JUMP` albo `TASK_CLIMB` przy nagranej kotwicy, a zejście z krawędzi
jednorazowo zasila lokalną fizykę pozycją i prędkością nadawcy.
Odznaczenie zachowuje dotychczasowy **Marker Lock V13.1** bez zmian. Ghost Record
pozwala porównać oba warianty na dokładnie tej samej własnej trasie.
Przez okres bez dostępu do drugiego komputera użyj instrukcji:

[docs/TEST_SOLO_V8.md](docs/TEST_SOLO_V8.md)

Test dwóch komputerów korzysta teraz z hostowego rejestru Entity Graph V11.0. Pełna
instrukcja dla obu graczy znajduje się w:

[packaging/PRZECZYTAJ_MNIE.txt](packaging/PRZECZYTAJ_MNIE.txt)

Scenariusz stroju oraz native/fallback AnimScene jest opisany w
[docs/TEST_V29_ANIMSCENE_METAPED.md](docs/TEST_V29_ANIMSCENE_METAPED.md).

Najważniejsze:

1. Każdy gracz pobiera Script Hook RDR2 osobno od autora. Paczka moda nie
   zawiera `ScriptHookRDR2.dll` ani `dinput8.dll`.
2. Rozpakuj świeży ZIP moda do nowego folderu i uruchom `URUCHOM_COOP.bat`.
3. Wskaż `RDR2.exe`, osobno rozpakowany Script Hook i wybierz platformę.
4. Do próby nowego kontrolera wejdź w `Ustawienia`, zaznacz
   `ANIMGRAPH REPLICA`, następnie wybierz kartę `Test solo`, platformę i jeden
   okrągły przycisk `START`. Wczytaj Story Mode, otwórz `F9` i wybierz
   `Test solo: start / stop`.
   Aby sprawdzić własną trasę, wybierz `Ghost Record: start / stop`, przejdź ją,
   zatrzymaj nagranie, wróć w pobliże startu i wybierz
   `Ghost Replay: start / stop`.
5. Host w `Ustawieniach` wybiera lokalny plik `SRDR*` z prologu lub innego etapu. Guest nie kopiuje
   tego save'a: przed JOIN wczytuje własny spokojny zapis poza aktywną misją i
   cutscenką. Preflight V22 odrzuci JOIN, jeśli save guesta nie jest bezpieczny.
6. W launcherze host wybiera `Host`, używa przycisku `HOSTUJ`, wpisuje hasło
   dwukrotnie i przekazuje guestowi tylko IPv4 oraz hasło. Guest wybiera
   `Dołączam` i przyciskiem `DOŁĄCZ` podaje IPv4 hosta oraz to samo hasło. Na obu
   komputerach ma pojawić się `HASŁO ZAPISANE`, a lobby ma pokazać nicki, role
   i ping w ms.
7. Wybierz wyłącznie Story Mode. Host wczytuje wybrany save, a guest spokojny
   save techniczny. Nie wybierajcie HOST/JOIN w F8 — kompletna sesja została już
   uruchomiona z launchera. Poczekajcie na `REMOTE STREAMING`, zanim host uruchomi
   marker misji.
8. W F9 wybierzcie `Daj pistolet + max ammo (test)` i `Daj lasso (test)` po obu stronach. Sprawdźcie
   celowanie stojąc i idąc, schowanie broni, pojedynczy cios i natychmiastowe
   odejście, serię ciosów, blok R, grapple, powalenie oraz puszczenie każdej akcji.
   Po puszczeniu przycisku zdalny gracz nie może dalej sam walczyć.
9. Lasso przetestujcie osobno: najpierw 2–3 sekundy samego celowania/kręcenia,
   potem jeden rzut, przyciągnięcie, związanie, uwolnienie i osobne chybienie.
   Druga strona ma widzieć lasso w dłoni, wind-up, rzut oraz najwyżej jedną linę.
   Chybienie nie może przewrócić żadnego gracza; złapanie ma używać natywnej
   liny/fizyki, bez dźwięku strzału, stosu broni i wymuszonego ragdolla.
10. Na koniu hosta kolejno jedzie host, guest, potem obaj próbują interakcji.
    Sprawdźcie wejście, zejście, próbę zrzucenia, chód/kłus/sprint oraz zbocze.
    Nie mogą powstać dwa konie w jednym miejscu; po zejściu zdalny jeździec nie
    może zostać na siodle. Logi `MOUNT_LOCAL`, `REMOTE_MOUNT_RX`,
    `REMOTE_MOUNT_RELATION` i `REMOTE_MOUNT` opisują każdy etap.
11. Test misji wykonajcie w tej kolejności: guest porównuje NPC przed i po JOIN;
    przed startem hosta podchodzi do własnego aktora/markera Story i próbuje
    interakcji. Marker może pozostać żółty, ale szary komunikat moda musi się
    pojawić, a lokalna misja guesta nie może ruszyć.
    **Tylko host uruchamia misję**. Guest czeka 8–10 m dalej, aż
    automatycznie włączy się spectator; nie uruchamiajcie vanilla markera
    jednocześnie na obu save'ach. Po scenie guest sprawdza panel
    `AKTYWNA MISJA HOSTA / WSPOLNY CEL FABULARNY`, żółty znacznik celu,
    aktorów i wrogów z grafu oraz strzał do jednego hostowego wroga. W cutscence
    sprawdźcie jednoczesny start kamery. `ATTACHED` ma pełną lokalną scenę;
    `PROXY_CAST_FALLBACK` ma stabilną obsadę bez gwarancji gestów/lipsyncu.
    Po końcu odczekajcie 7 s i potwierdźcie, że guest nie uruchomił drugiej sceny,
    następnie wykonajcie jeden checkpoint/retry. Guest nie może uruchomić własnego markera.
    Zapis i prawdziwy cel misji należą wyłącznie do hosta.
12. Escape jest głosem: pierwszy gracz prosi o pauzę, drugi ją zatwierdza.
   Wznowienie również wymaga obu głosów.
13. F8 jest domyślnie zamkniętym panelem awaryjnym i nie ma już HOST/JOIN.
    `ZATRZYMAJ SESJE` jest też w lewej kolumnie F9, natomiast pełne zatrzymanie sidecara wykonuje
    `WYŁĄCZ COOP` w launcherze i wymaga ponownego HOSTUJ/DOŁĄCZ.
14. Gdy zauważycie błąd, obie osoby naciskają `F7`, sprawdzają komunikat
    `MARKER ERROR #... ZAPISANY`, grają jeszcze 10–15 sekund i dopiero zatrzymują
    sesję. Po teście obie osoby
    eksportują diagnostykę, zamykają grę i klikają
   `Odinstaluj` przed uruchomieniem Red Dead Online.

V25.0 jest nadal Campaign Shell, nie wspólną maszyną skryptów. Guest widzi
hostowych aktorów/wrogów, syntetyczny żółty cel i kamerę sceny, ale nie ma gwarancji
identycznego tekstu vanilla, lokalnego AnimScene/audio, drzwi, pojazdów i przedmiotów
fabularnych. W razie softlocka host używa `Solo override`; postęp zapisuje się
wyłącznie w save'ie hosta.
