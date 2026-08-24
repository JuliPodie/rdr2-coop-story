# RDR2 Coop Story — test dwóch PC „Host World / Mount Alpha V6”

Ten test sprawdza techniczne podstawy coopa w spokojnym free-roam na dwóch
komputerach. Nie jest testem kampanii. Każdy komputer nadal uruchamia własny
save oraz własne skrypty misji RDR2.

## Zakres tej wersji

V6 ma sprawdzić:

- ruch zdalnego gracza z rzadszymi restartami tasków oraz aim podczas ruchu;
- stabilniejszy nick oraz powrót proxy po śmierci/checkpoincie;
- nick, blip, aktualną broń, kierunek celowania i wizualne strzały;
- wizualne PvP gracz kontra gracz, zawsze bez obrażeń;
- czas, datę i pogodę narzucone guestowi przez hosta;
- World Mirror maksymalnie 48 hostowych ludzi i koni
  w promieniu 80 m od hosta, aktualizowany do 10 Hz;
- semantyczne zadania NPC i relację hostowy jeździec–koń;
- własnego konia obu graczy, jego replikę, dosiadanie i zsiadanie;
- eksperymentalny tor obrażeń: guest zgłasza `DamageIntent` dotyczący proxy
  ambientowego NPC, a host sprawdza żądanie i dopiero wtedy może uszkodzić
  odpowiadającego mu prawdziwego NPC;
- brak zaległego strumienia po loadingu albo chwilowym zatrzymaniu bridge'a;
- teleport lokalny obu ról oraz hostowe przywołanie guesta;
- wspólne głosowanie oraz natywne menu pauzy RDR2 pod `Escape`;
- lokalną dostępność własnego konia guesta;
- wybór lokalnego pliku `SRDR*` hosta i odwracalną blokadę startu lokalnej
  misji guesta, gdy host ma aktywną misję.

World Mirror może próbkować hostowe encje misyjne, ale nie przejmuje skryptów
celów, triggerów, checkpointów, cutscenek ani działającego save'a.

## Zasady bezpieczeństwa

- Wyłącznie Story Mode. Nie uruchamiaj RDO ani RedM z zainstalowanym modem.
- Używamy prywatnego LAN albo prywatnej sieci Hamachi IPv4.
- Nie wyłączamy zapory i nie otwieramy publicznych portów routera.
- Każdy gracz ma legalną kopię gry, własne konto i lokalny backup save'ów.
- Nie używamy trainerów ani innych modów podczas tej próby.
- Wyniku „nie sprawdzono” nie zapisujemy jako „działa”.
- Gdy gra crashuje, sterowanie zostaje zablokowane albo mnożą się proxy,
  kończymy próbę i zachowujemy diagnostykę obu stron.

## ScriptHook pobiera każdy tester

ZIP projektu nie zawiera `ScriptHookRDR2.dll`, `dinput8.dll`,
`NativeTrainer.asi` ani SDK.

Każdy tester:

1. samodzielnie pobiera oficjalny Script Hook RDR2;
2. rozpakowuje go do osobnego folderu;
3. wskazuje ten folder w launcherze;
4. nie instaluje `NativeTrainer.asi`.

Launcher może skopiować tylko zweryfikowane pliki runtime ze wskazanego
folderu. ScriptHook nie jest częścią paczki moda.

## Instalacja na obu komputerach

1. Zamknij RDR2.
2. Odinstaluj poprzedni build jego własnym launcherem.
3. Rozpakuj ten sam świeży ZIP `Host World Alpha V6` do nowego folderu.
4. Uruchom `URUCHOM_COOP.bat`.
5. Wskaż własny `RDR2.exe`.
6. Wskaż osobno pobrany i rozpakowany ScriptHook.
7. Wpisz nick w polu `Nick w coop`.
8. Host klika `Wykryj Hamachi/LAN` i sprawdza własny IPv4. Dla Hamachi
   powinien to być adres `25.x.x.x`, a następnie wybiera lokalny save `SRDR*`.
9. Kliknij `Weryfikuj`, a potem `Zainstaluj`.
10. Zamknij launcher, jeżeli był uruchomiony jako administrator.
11. Do normalnej gry uruchom `URUCHOM_COOP.bat` bez podnoszenia uprawnień.
12. Kliknij `HOST multiplayer` albo `JOIN multiplayer` dla Steam/Rockstar.
    Wybierz wyłącznie Story Mode. Host wczytuje slot odpowiadający wskazanemu
    plikowi; guest wczytuje lokalny save techniczny w spokojnym free-roam.

### Gdy pojawi się „Access denied”

1. Zamknij RDR2 i launcher.
2. Kliknij prawym przyciskiem `CoopStory.Launcher.exe` i wybierz
   `Uruchom jako administrator`.
3. Użyj podniesionego launchera tylko do `Zainstaluj` albo `Odinstaluj`.
4. Zamknij go po zakończeniu operacji.
5. Do grania ponownie uruchom `URUCHOM_COOP.bat` normalnie, bez administratora.

Nie zmieniaj ręcznie uprawnień całego katalogu gry i nie wyłączaj zabezpieczeń
Windows.

## Ważne: wybór save'a nie jest sieciowym loaderem

Launcher hosta identyfikuje wybrany plik `SRDR*` nazwą i SHA-256, lecz go nie
modyfikuje, nie wysyła guestowi i nie może automatycznie wczytać danego slotu.
Host robi to zwykłym menu RDR2. Test nie zapisuje postępu guesta w save'ie
hosta. World Mirror ogranicza rozjazd wybranych encji, lecz nie zmienia dwóch
instancji Story Mode w jedną sesję skryptów kampanii.

## Zestawienie sesji

HOST:

1. W `Ustawieniach` wybierz lokalny save, potem wybierz `Host`, ustaw IPv4 i
   kliknij `HOSTUJ`. Wpisz oraz powtórz hasło sesji.
2. Po komunikacie `HASŁO ZAPISANE` przekaż guestowi IPv4 i hasło, uruchom
   wyłącznie Story Mode i wczytaj wskazany
   slot. F8 pozostaje zamknięty.

GUEST:

1. W launcherze wybierz `Dołączam` i kliknij `DOŁĄCZ`.
2. Podaj IPv4 hosta oraz dokładnie to samo hasło i sprawdź komunikat
   `HASŁO ZAPISANE`.
3. Uruchom wyłącznie Story Mode i wczytaj spokojny save techniczny.

Test zaczyna się dopiero, gdy:

- oba komputery pokazują właściwą rolę `COOP HOST` / `COOP GUEST`;
- oba pokazują `IPC CONNECTED`;
- oba pokazują **`REMOTE STREAMING`**.

Sam napis `IPC CONNECTED` potwierdza tylko lokalne połączenie ASI z sidecarem.

## Klawisze podczas aktywnej sesji

- `F7` — zapisz marker błędu i pokaż krótkie potwierdzenie;
- `F8` — pokaż/ukryj domyślnie zamknięty panel awaryjny bez HOST/JOIN;
- `F9` — pokaż/ukryj dwukolumnowe menu narzędzi;
- strzałki góra/dół wybierają akcję, lewo/prawo grupę, a `Enter` ją wykonuje;
- `F10` — pokaż/ukryj górny pasek statusu.

Podczas `REMOTE STREAMING` pierwszy `Escape` oddaje głos i pokazuje status
drugiemu graczowi. Dopiero `Escape` drugiej osoby powinien otworzyć natywne
menu pauzy RDR2 na obu PC. Wznowienie działa tak samo: obaj muszą nacisnąć
`Escape`, a bridge zamyka oba frontendy. Do menu moda używajcie `F8` i `F9`.

## Scenariusz testowy

Pierwsze punkty testujcie na otwartym, możliwie płaskim terenie, bez aktywnej
misji, markera fabularnego i cutscenki. Prostą misję uruchamia dopiero host
w osobnym punkcie. Najlepiej nagrać krótki materiał z obu ekranów albo
zanotować dokładną godzinę systemową wystąpienia problemu.

### 1. Połączenie i ustawienie graczy

1. Zaczekajcie 30 sekund po pojawieniu się `REMOTE STREAMING`.
2. Jeżeli jesteście daleko, host używa `F9` i `Przywołaj guest (host)`.
3. Potwierdźcie na ekranie guesta, że przeniesiona została jego prawdziwa
   sterowana postać, a nie tylko lokalne proxy na ekranie hosta.

`PASS`: obie osoby widzą zdalne proxy w tej samej okolicy i niezależnie
sterują własnymi postaciami.

### 2. Płynny ruch graczy

Najpierw porusza się tylko guest, potem tylko host:

1. marsz po prostej przez około 30 m;
2. bieg po okręgu;
3. sprint przez około 50 m;
4. szybki obrót o 180 stopni i powrót;
5. kilka krótkich startów i nagłych zatrzymań;
6. stanie bez ruchu przez 15 sekund.

Zanotujcie:

- czy ruch jest ciągły i czy kierunek postaci jest wiarygodny;
- czy proxy ślizga się, zawisa albo przeskakuje;
- czy jedna strona widzi bieg, gdy druga widzi stojącą postać;
- każdy twardy warp i jego przybliżoną odległość;
- zachowanie po teleporcie oraz po chwilowym loadingu.

V6 zalicza ten punkt dopiero wtedy, gdy postać porusza się z animacją i nie
wraca regularny skok około 8 m.
Pojedynczą korektę po prawdziwym teleporcie opisujemy osobno.

### 3. Nick, blip, broń, celowanie i wizualne PvP

1. Podejdźcie do siebie na mniej niż 20 m i sprawdźcie nick oraz przyjazny blip.
2. Każdy kolejno wybiera pięści, rewolwer i broń długą.
3. Celujcie kolejno w ziemię, w bok i nad zdalnym graczem.
4. Oddajcie pojedyncze strzały w bezpiecznym kierunku.
5. Oddajcie po jednym strzale w proxy drugiego gracza.
6. Celujcie podczas marszu i biegu.
7. Wykonajcie po kilka uderzeń w walce wręcz.

Oczekiwany wynik:

- zdalne proxy pokazuje odpowiednią broń i przybliżony kierunek celowania;
- widoczne są wizualne strzały;
- proxy można lokalnie namierzyć i trafić bez szarej blokady friendly;
- proxy gracza nie traci zdrowia — PvP w V6 ma zerowe obrażenia;
- lasso, jeśli zadziała, jest tylko lokalnym efektem i nie replikuje jeszcze
  związania ani hogtie do drugiej gry.

Nie oceniamy jeszcze dokładnej animacji przeładowania, odrzutu, task tree ani
pełnego hit detection.

### 4. Czas, data i pogoda hosta

1. Przed połączeniem zanotujcie godzinę, datę i pogodę obu komputerów.
2. Po `REMOTE STREAMING` guest czeka co najmniej 15 sekund.
3. Porównajcie godzinę, dzień, miesiąc, rok i widoczną pogodę.
4. Jeśli host może bezpiecznie zmienić porę przez normalną mechanikę free-roam,
   porównajcie ponownie.
5. Obserwujcie naturalne przejście pogody.

Host jest jedynym źródłem tych danych. Zapiszcie, czy guest dopasował się,
oscylował między stanami albo pozostał przy swoim lokalnym świecie.

### 5. Głosowanie pauzy i Dead Eye / focus

1. Guest biegnie, a host naciska `Escape`.
2. Gra ma działać dalej, a obie strony mają zobaczyć oczekiwanie na drugi głos.
3. Guest naciska `Escape`; dopiero teraz na obu PC ma otworzyć się prawdziwe
   menu pauzy RDR2.
4. Host naciska `Escape`; pauza ma nadal trwać i czekać na guesta.
5. Guest naciska `Escape`; oba menu mają się zamknąć i gry wznowić działanie.
6. Zamieńcie kolejność głosujących i powtórzcie.
7. Host uruchamia Dead Eye/focus, gdy guest biegnie.
8. Guest uruchamia Dead Eye/focus, gdy host biegnie.
9. Otwórzcie i zamknijcie menu moda przez `F8` oraz `F9`.

Druga postać nie powinna stanąć, zwolnić ani po wznowieniu dostać serii starych
snapshotów. Ten punkt nie potwierdza poprawności walki w Dead Eye.

### 6. World Mirror ludzi i koni hosta

1. Podejdźcie razem do małej grupy ludzi i koni hosta.
2. Pozostańcie w promieniu 80 m od hosta.
3. Porównajcie modele, liczbę, pozycje, kierunek, broń, stan życia oraz
   kategorię zachowania.
4. Host odchodzi i wraca, aby sprawdzić spawn/despawn proxy.
5. Znajdźcie jednego hostowego NPC na koniu i sprawdźcie relację z siodłem.
6. Nie wchodźcie od razu do zatłoczonego miasta.

Oczekiwany limit to maksymalnie 48 hostowych encji, aktualizowane do 10 Hz.
V6 replikuje kategorię tasku i cel wysokiego poziomu, a nie pełne task trees,
harmonogram AI, dialogi, AnimScene ani dokładne outfity.

### 7. Własne konie obu graczy

1. Guest odchodzi na otwarty teren i używa zwykłego vanilla call horse.
2. Koń guesta powinien przybiec lokalnie i pojawić się jako replika u hosta.
3. Guest wsiada, jedzie około 50 m, zatrzymuje się, zsiada i woła go ponownie.
4. Host wykonuje ten sam zestaw, obserwowany przez guesta.
5. Powtórzcie po jednym reconnect.

V6 celowo nie ukrywa własnego konia guesta. Oczekujemy repliki modelu,
transformu, zdrowia i relacji mount–rider, ale nie więzi, stajni, bagażu,
shared mount ani identycznej każdej animacji.

### 8. Hostowa walidacja obrażeń ambientowego NPC

1. Wybierzcie pojedynczego niemisyjnego NPC widocznego po obu stronach.
2. Guest oddaje jeden strzał w jego proxy.
3. Host sprawdza reakcję i zdrowie odpowiadającego prawdziwego NPC.
4. Powtórzcie najwyżej kilka razy, zachowując dokładną godzinę testu.

Guest wysyła jedynie `DamageIntent`. Host sprawdza identyfikator encji,
dozwolony stan i parametry żądania; klient nie ustala sam zdrowia ani śmierci
hostowego NPC. Jest to eksperymentalny tor dla ambientowych encji, a nie pełna
walka AI lub kampanii.

### 9. Teleporty

1. Host odchodzi co najmniej 20 m i wybiera `F9` → `Teleport do gracza`.
2. Guest wykonuje ten sam test.
3. Guest ponownie odchodzi, a host wybiera
   `F9` → `Przywołaj guest (host)`.
4. Każdą ścieżkę powtórzcie trzy razy na bezpiecznym podłożu.

Sprawdzamy prawdziwe lokalne postacie, nie samo przestawienie proxy.

### 10. Prosta misja uruchamiana wyłącznie przez hosta

1. Po zaliczeniu free-roam host aktywuje jedną krótką misję.
2. Guest nie dotyka własnego markera. Sprawdza, czy nie może rozpocząć
   konkurencyjnej misji, gdy host ma aktywną flagę.
3. Porównajcie hostowych ludzi, konie i relacje jeźdźców.
4. Zapiszcie różnice celów, blipów, dialogów i checkpointów jako oczekiwane
   ograniczenia. Ten punkt nie potwierdza wspólnej kampanii.
5. Przy softlocku host używa Solo override albo kończy próbę.

### 11. Śmierć/checkpoint i stabilność

1. Po zapisaniu dokładnej godziny pozwólcie najpierw zginąć hostowi.
2. Po vanilla odrodzeniu/checkpoincie zaczekajcie 10 sekund i sprawdźcie, czy
   guest ponownie widzi i śledzi hosta.
3. Powtórzcie dla guesta.

Po wykonaniu poprzednich punktów poruszajcie się razem przez 10 minut bez misji.
Zanotujcie:

- zniknięcie albo zdublowanie proxy gracza lub NPC;
- powrót skoków i chwilę utraty `REMOTE STREAMING`;
- brak aktualizacji broni, celu, czasu, daty albo pogody;
- NPC widoczne tylko po jednej stronie;
- crash, zawieszenie albo brak możliwości zamknięcia menu moda.

## Poza zakresem V6

Nie uznajemy za zaimplementowane:

- wspólnego save'a hosta ani wspólnej listy dostępnych misji;
- synchronizacji skryptów misji, triggerów, celów, blipów i checkpointów;
- cutscenek, dialogów, AnimScene i pełnego audio;
- stroju, twarzy, włosów i pozostałych danych Outfit/MetaPed;
- pełnych task trees, harmonogramów oraz wszystkich stanów AI NPC;
- shared mount, więzi/stajni/bagażu i pełnej animacji konia;
- pełnej walki, wszystkich rodzajów obrażeń, downed/revive i retry kampanii;
- wszystkich obiektów świata, drzwi, wozów i pojazdów.

Prosta misja ma sprawdzić stabilność hostowych encji i blokadę startu guesta,
nie „zaliczyć kampanię” V6. Dwa różne zestawy markerów misji są spodziewanym
skutkiem dwóch lokalnych instancji skryptów, nie wspólną sesją kampanii.

## Obowiązkowa diagnostyka

Po każdej próbie, także nieudanej:

1. Zamknijcie RDR2 na obu komputerach.
2. Kliknijcie `Zatrzymaj test`.
3. Host eksportuje własny ZIP przez `Eksportuj diagnostykę`.
4. Guest eksportuje własny ZIP.
5. Nazwijcie pliki `HostWorldV6-HOST.zip` i `HostWorldV6-GUEST.zip`.
6. Wyślijcie oba ZIP-y wraz z krótkim opisem i godziną problemu.
7. Nie dołączajcie hasła, save'ów ani danych konta.
8. Na koniec odinstalujcie mod na obu komputerach. Jeżeli katalog gry wymaga
   uprawnień, użyjcie procedury `Access denied` opisanej wyżej.

Eksporter redaguje sekret sesji, nazwę komputera i prywatne ścieżki.

## Krótki szablon wyniku

```text
HOST: Steam/Rockstar, nick:
GUEST: Steam/Rockstar, nick:
REMOTE STREAMING na obu: TAK/NIE
Ruch guest widziany przez hosta:
Ruch hosta widziany przez guesta:
Nick i blip:
Broń / celowanie / wizualne strzały:
Aim podczas ruchu / walka wręcz:
PvP bez obrażeń:
Czas / data / pogoda:
Głosowanie pauzy i wznowienia:
Natywne menu pauzy otwarte/zamknięte:
Dead Eye/focus:
World Mirror NPC:
Semantyczne taski NPC / hostowy jeździec:
Własny koń hosta / mount-rider:
Własny koń guesta / mount-rider:
Blokada lokalnej misji guesta:
Prosta misja hosta — encje / różnice celów:
DamageIntent guest -> NPC hosta:
Teleport host:
Teleport guest:
Przywołaj guest:
Crash/zniknięcie proxy:
Powrót proxy po śmierci hosta/guesta:
Godzina wystąpienia problemu:
```

## Walidacja deweloperska przed ZIP-em

```powershell
dotnet build .\CoopStory.slnx -c Release
dotnet run --project .\tests\CoopStory.SelfTest\CoopStory.SelfTest.csproj `
  -c Release
dotnet run --project .\tests\CoopStory.Launcher.SelfTest\CoopStory.Launcher.SelfTest.csproj `
  -c Release

cmake --preset bridge-asi-vs2026
cmake --build --preset bridge-asi-vs2026-release
ctest --preset bridge-asi-vs2026-release
```

Build `.asi` wymaga lokalnego SDK ScriptHooka i aktywnych native bindings.
Generator paczki musi odrzucić nieświeży albo stubowy bridge oraz potwierdzić
brak ScriptHooka, loadera, trainera, SDK, sekretów i plików Rockstar w ZIP-ie.
