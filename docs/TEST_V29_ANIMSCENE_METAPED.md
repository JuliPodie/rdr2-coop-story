# Test V29.6 — AnimScene, blokada misji, lasso, walka i mount

## Przygotowanie

1. Zainstalujcie tę samą paczkę V29.6 na obu komputerach. Protokół obu stron
   musi mieć numer `18`; mieszanie V29 ze starszym buildem zostanie odrzucone.
2. Każdy gracz wybiera wyraźnie inny strój w Story Mode — najlepiej zmienić
   kilka części, na przykład kapelusz, płaszcz, kamizelkę i spodnie.
3. Host wybiera save SRDR* w `Ustawieniach`, tworzy sesję przyciskiem `HOSTUJ`
   i wpisuje hasło dwa razy. Guest wybiera `Dołączam`, naciska `DOŁĄCZ` i podaje
   IPv4 hosta oraz to samo hasło. Obie strony powinny zobaczyć `HASŁO ZAPISANE`,
   a lobby oba nicki, role i ping w ms. Panel F8 ma pozostać zamknięty po wejściu
   do Story Mode i nie powinien zawierać HOST/JOIN.
4. Guest własnego markera misji nie uruchamia.

## Test blokady misji i wspólnego celu

1. Po JOIN, ale przed uruchomieniem misji hosta, guest sprawdza co najmniej dwa
   lokalne markery Story. Już teraz powinny być szare i mieć kłódkę.
2. Tylko host uruchamia misję. Guest podchodzi do dwóch lub więcej dostępnych
   lokalnych markerów, ale nie próbuje ich uruchamiać na siłę.
3. Każdy sprawdzony marker powinien być szary, mieć kłódkę i pokazać waniliowy
   komunikat `because of recent activities this mission is currently locked`.
4. W aktywnym gameplayu misji guest powinien widzieć panel
   `AKTYWNA MISJA HOSTA / WSPOLNY CEL FABULARNY` oraz żółty marker celu.
   Dokładny tekst hosta, np. `Follow gang`, nie jest jeszcze kopiowany z jego VM.

## Test lassa

1. W obie strony sprawdźcie 2–3 sekundy samego celowania i kręcenia lassem.
2. Wykonajcie osobno: chybienie, trafienie, przyciąganie, hogtie i uwolnienie.
3. Odbiorca powinien widzieć lasso w dłoni, wind-up, rzut i najwyżej jedną linę.
4. Chybienie nie może wywołać ragdolla. Trafienie ma przejść w natywną linę,
   animację/fizykę ofiary i nie może jednocześnie przewrócić rzucającego.
5. Naciśnijcie F7 po obu stronach natychmiast, jeśli zniknie lina, zabraknie
   animacji albo wystąpi ragdoll bez natywnego złapania.

## Test wyglądu

1. Stańcie obok siebie przez co najmniej 3 sekundy.
2. Sprawdźcie po obu stronach kapelusz, okrycie, kamizelkę, spodnie i pozostałe
   widoczne części stroju.
3. Zmieńcie jedną część garderoby, odczekajcie 3 sekundy i sprawdźcie zmianę
   na drugim komputerze. W razie potrzeby użyjcie raz `F9 -> Resync ekwipunku`.

Oczekiwane logi to `METAPED_APPEARANCE TX` po stronie właściciela i
`METAPED_APPEARANCE APPLIED` po stronie odbiorcy. `WAIT_MODEL` oznacza, że
proxy używa innego modelu postaci i bridge celowo nie wykonał ryzykownego
resetu komponentów.

## Test cutscenki

1. Guest stoi blisko hosta i nie wchodzi sam w marker misji.
2. Tylko host uruchamia misję i cutscenkę.
3. Nie używajcie pomijania sceny w pierwszej próbie.
4. Guest obserwuje kamerę, pozycje i animacje aktorów, napisy, dialogi oraz
   audio, a po zakończeniu sprawdza odzyskanie sterowania i HUD-u.

W diagnostyce guesta wystąpi jedna z dwóch poprawnie rozróżnionych ścieżek:

- `ANIMSCENE_REPLICA ATTACHED` oraz późniejsze `PHASE` — znaleziono zgodną,
  już załadowaną lokalną AnimScene. Powinna ona dostarczyć oryginalną kamerę,
  aktorów, napisy, dialogi i audio.
- `ANIMSCENE_REPLICA SAFE_FALLBACK` — guest nie posiadał zgodnej lokalnej
  sceny po pełnym przeglądzie lokalnych uchwytów. Mod pozostaje przy bezpiecznej
  kamerze hosta z V28 i nie próbuje tworzyć niepełnej AnimScene, co mogłoby
  uszkodzić misję lub wywołać crash.

Zapiszcie, która ścieżka wystąpiła. Po scenie nie powinno być okna błędu
ScriptHooka, lokalnego `Mission Failed`, migotania świata ani trwałego
spectatora.

W drugiej próbie naciśnijcie skip po jednym razie na obu komputerach w odstępie
nie większym niż 5 sekund. Obie lokalne sceny powinny zakończyć się razem. Guest
nie może uruchomić swojej sceny dopiero po końcu sceny hosta ani pozostać na
ekranie ładowania. Marker lokalnej misji może być nadal widoczny, ale guest nie
wchodzi w niego i nie powinien dostać lokalnego `Mission Failed`.
Jeżeli kwarantanna wykryje późną lokalną scenę, log
`MISSION_SKIP QUARANTINE` powinien wystąpić raz dla tego przejścia; wejście skip
jest ograniczone do 2500 ms i nie może pozostać stale aktywne.

## Test free-roam, konia i walki

1. Zaraz po JOIN guest dosiada własnego konia i zsiada. Obie operacje muszą
   działać bez wejścia w spectator i bez zablokowanego klawisza kontekstu.
2. Guest sprawdza, czy nie pojawiły się losowe martwe lub nieruchome konie,
   których host nie widzi.
3. W obie strony wykonajcie pojedynczy cios, serię ciosów, blok oraz sprintowe
   powalenie. Atak zdalnego gracza nie może urwać się tuż przed kontaktem;
   powalenie powinno zachować widoczny atak i reakcję ofiary.
4. Jeżeli wystąpi rozjazd, każda strona natychmiast naciska `F7`.

## Diagnostyka

Po jednej pełnej próbie obie osoby wybierają eksport diagnostyki. Prześlijcie
dwa oryginalne pliki ZIP bez wypakowywania, nazwane:

- `AnimSceneTransportV29.6-HOST.zip`
- `AnimSceneTransportV29.6-GUEST.zip`

W opisie podajcie: nazwę misji, czy strój był zgodny, którą ścieżkę AnimScene
wybrano i co dokładnie zobaczył oraz usłyszał guest.
