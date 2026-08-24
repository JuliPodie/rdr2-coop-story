# RDR2 Coop Story — zasady prawne i bezpieczeństwa

To dokument projektowy, nie porada prawna. Projekt pozostaje prywatnym, niekomercyjnym eksperymentem technicznym.

## Dozwolony zakres projektu

- PC Story Mode, dwóch graczy w prywatnym LAN albo prywatnej sieci Hamachi.
- Dwie legalne kopie gry i dwa legalne konta podczas testu na dwóch komputerach.
- Własny kod bridge’a, sidecara, konfiguracja, testy i dokumentacja.
- Natywy udostępnione przez osobno pobrane SDK ScriptHooka, zgodnie z jego warunkami.
- Logi i profile zawierające wyłącznie dane techniczne projektu.

Poza zakresem:

- Red Dead Online, RedM i publiczne serwery;
- obchodzenie DRM, anti-cheat, banów albo kontroli dostępu;
- przechwytywanie usług Rockstar;
- dekompilowane skrypty, assety, mapy, audio lub inne pliki Rockstar;
- trainer i funkcje oszustw;
- publiczna dystrybucja bez osobnej analizy prawnej i licencyjnej.

## Polityka Rockstar

Polityka Rockstar dotycząca części modów single-player nie jest zgodą na multiplayer i wyraźnie nie obejmuje narzędzi mogących wpływać na usługi online. Prywatny LAN lub Hamachi zmniejsza powierzchnię ryzyka, ale nie daje licencji, wyjątku ani ochrony przed działaniem wydawcy.

Materiały do ponownej oceny przed jakąkolwiek dystrybucją:

- [Rockstar — PC Single-Player Mods](https://support.rockstargames.com/articles/5NVOAYjcTomO8v6SX2k76k/pc-single-player-mods)
- [Rockstar — Legal Terms](https://www.rockstargames.com/legal?country=pl)

Jeśli warunki albo polityka ulegną zmianie, wstrzymujemy testy i ponownie oceniamy projekt. Brak reakcji wydawcy nie jest zgodą.

## ScriptHook RDR2

Runtime i SDK użytkownik pobiera osobno z oficjalnej strony autora. Repozytorium i prywatna paczka projektu nie mogą zawierać:

- `ScriptHookRDR2.dll`;
- `dinput8.dll`;
- `NativeTrainer.asi`;
- biblioteki lub nagłówków SDK, jeśli warunki nie pozwalają ich redystrybuować.

Launcher może poprosić użytkownika o wskazanie folderu samodzielnie pobranego
i rozpakowanego runtime'u. Dopiero po sprawdzeniu przypiętych hashy może
skopiować z tego folderu `ScriptHookRDR2.dll` i `dinput8.dll` do katalogu gry.
Nigdy nie kopiuje `NativeTrainer.asi`.

Manifest rozróżnia pliki utworzone przez bieżącą instalację od runtime'u
obecnego wcześniej. Uninstaller usuwa `ScriptHookRDR2.dll` lub `dinput8.dll`
tylko wtedy, gdy dana instalacja sama je skopiowała i ich hash nadal odpowiada
manifestowi; zweryfikowane pliki obecne przed instalacją pozostają
nienaruszone. Przy braku pewności operacja zatrzymuje się zamiast zgadywać.

`Verify-Prerequisites.ps1` może sprawdzić obecność i strukturę lokalnego
pakietu. Archiwum pobrane przez użytkownika należy przeskanować Defenderem;
autor nie publikuje oficjalnego checksumu, więc lokalny hash służy tylko do
wykrywania późniejszych zmian, nie do dowodu autentyczności.

## Bezwzględny guard offline/RDO

Bridge ma zasadę fail-closed:

1. przed aktywacją sprawdza obsługiwany hash gry i stan Story Mode;
2. przy braku pewności pozostaje nieaktywny;
3. po wykryciu online/RDO zatrzymuje IPC i sieć projektu, ukrywa/usuwa repliki i nie wykonuje poleceń gameplay;
4. zapisuje wyłącznie zanonimizowany powód odmowy;
5. nie posiada flagi, skrótu ani ustawienia pozwalającego ominąć guard.

Test guardu nie wymaga wejścia do publicznej sesji. Używamy testu jednostkowego stanu i lokalnej odmowy aktywacji.

## Dane, sieć i logi

- v1 używa prywatnego IPv4 przez direct LAN albo osobno zainstalowane Hamachi;
  sam projekt nie ma chmury, matchmakingu, telemetryki zewnętrznej ani NAT
  traversal.
- TCP `43120` i UDP `43121` dopuszczamy tylko w profilu sieci prywatnej.
- Token sesji jest sekretem lokalnym: nie trafia do repo, logów ani raportów błędów.
- Zaproszenie `.coopjoin` zawiera adres LAN hosta i sekretny token sesji.
  Należy przekazać je znajomemu kanałem prywatnym, nie publikować i usunąć po
  zakończeniu próby.
- Logi nie zawierają pełnych ścieżek użytkownika, nazw kont, IP publicznego, pointerów ani uchwytów gry.
- Eksport diagnostyczny redaguje token sesji i dane lokalne. Może zachować
  techniczny adres LAN potrzebny do analizy połączenia, dlatego ZIP nadal jest
  prywatnym artefaktem i należy sprawdzić go przed wysłaniem.
- Profile guesta i backupy save’ów pozostają lokalne.
- Narzędzie verify raportuje statusy, nie lokalizacje instalacji.
- Nie wysyłamy save’ów i plików gry do zgłoszeń.

## Bezpieczna praca z plikami

- Baseline zapisuje metadane top-level i hashe jawnych plików krytycznych; nie skanuje/hashuje 120 GB danych gry.
- Backup save’ów jest copy-only, odrzuca reparse pointy, weryfikuje SHA-256 i nie nadpisuje celu.
- Deweloperski installer pozostaje dry-runem bez `-Apply`. Launcher GUI
  wykonuje instalację dopiero po sprawdzeniu dokładnego hasha gry, paczki
  projektu i osobno pobranego runtime'u.
- Launcher zapisuje atomowy manifest własności poza katalogiem gry, zanim
  zatwierdzi pierwszy własny plik w katalogu gry.
- Uninstaller usuwa wyłącznie pliki oznaczone jako własność bieżącej
  instalacji i tylko wtedy, gdy ich hash odpowiada manifestowi.
- Plik zmieniony, obcy albo niepewny nie jest automatycznie nadpisywany ani
  usuwany. Manifest pozostaje do bezpiecznej diagnostyki/odzyskania.
- Żaden skrypt nie wykonuje rekurencyjnego delete/move.

## Dystrybucja i publikacja

Aktualny wynik może pozostać prywatnym kodem źródłowym i prywatną paczką
wyłącznie własnych binariów projektu. Taka paczka nie może zawierać
ScriptHooka, SDK, trainera ani plików Rockstar. Przed publicznym repozytorium,
buildem, testem z szerszą grupą lub filmem zawierającym dystrybucję plików
należy:

1. ponownie przeczytać aktualne warunki Rockstar i ScriptHooka;
2. uzyskać profesjonalną ocenę prawną dla funkcji multiplayer;
3. w razie potrzeby uzyskać zgodę autora ScriptHooka;
4. przeprowadzić audyt paczki pod kątem cudzych binariów, nagłówków, assetów i sekretów;
5. usunąć dane lokalne z logów i manifestów.

Do tego czasu projekt nie obiecuje publicznego wydania.
