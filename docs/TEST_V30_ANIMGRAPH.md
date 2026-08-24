# Test V30.3 — cutscenka, blokada misji, lasso i AnimGraph Replica

Oba komputery muszą używać tej samej paczki V30.3 i protokołu 18. W launcherze
włączcie `ANIMGRAPH REPLICA`. Nie zmieniajcie pozostałych ustawień między HOST i
GUEST. Przy każdym rozjeździe jedna osoba naciska `F7` od razu. Druga powinna
zobaczyć komunikat o odebraniu tego samego markera. Nie kończcie gry przez
co najmniej 15 sekund po ostatnim markerze. Jeżeli obie strony widzą różne
objawy, każda może zapisać osobny marker.

## 1. Kierunek i przejścia

Na płaskim terenie wykonajcie kolejno po obu stronach:

1. stanie i obrót o około 45°, 90° oraz 180° bez ruszania;
2. start–stop w chodzie, biegu i sprincie;
3. walk–run–sprint–run–walk bez zatrzymania;
4. szybki zwrot przód–tył oraz dwa zakręty o 90°;
5. podczas celowania: lewo, prawo, do tyłu i cztery kierunki ukośne.
6. wejście w crouch, wejście z crouch bezpośrednio do cover, celowanie zza osłony,
   strzał, wyjście i ponowne wejście do tej samej osłony.

Odbiorca nie powinien obracać się skokowo o 90°, ślizgać bokiem w pozie chodu do
przodu ani pozostawać w starym kierunku po zwrocie. Obrót w miejscu powinien
poruszać stopami zamiast natychmiast przestawić całe ciało.

## 2. Walka, grapple i lasso

W obie strony wykonajcie pojedyncze ciosy, krótkie combo, blok, sprintowe
powalenie i grapple. Następnie sprawdźcie lasso: celowanie/kręcenie, chybienie,
trafienie, ciągnięcie, hogtie i uwolnienie. Zapiszcie dokładnie, czy obie strony
widziały odpowiadające sobie fazy. Chybienie lassa nie może tworzyć ragdolla, a
constraint nie może przewracać rzucającego.

## 3. Broń, koń i woda

1. Wyciągnijcie i schowajcie rewolwer oraz broń długą, oddajcie strzały podczas
   stania/ruchu i przeładujcie pustą broń.
   Przy strzale na stojąco odbiorca powinien zobaczyć ruch broni/recoil, nie tylko
   usłyszeć dźwięk. Po serii zdalna broń nie może pozostać bez amunicji.
2. Każda strona wsiada i zsiada z własnego konia, rusza stępem, galopuje i
   wykonuje zakręty.
3. Wejdźcie kolejno do płytkiej wody, zacznijcie pływać oraz na krótko zanurzcie
   postać. Drugi komputer nie powinien pokazywać grafu spadania ani T-pose.

## 4. Cutscenka i misja — najważniejsza regresja V30.3

1. Przed misją guest podchodzi do swojego żółtego markera/aktora Story i próbuje
   użyć wszystkich podpowiadanych przycisków. Marker może wizualnie pozostać
   żółty, ale ma pojawić się szary komunikat blokady i lokalna misja nie może
   wystartować. Guest następnie wsiada na własnego konia — ta interakcja ma działać.
2. Tylko host uruchamia misję i cutscenkę. Obie osoby naciskają `F7` w pierwszych
   sekundach obrazu. Kamera guesta ma ruszyć razem z hostem, nie dopiero po jego
   zakończeniu. Po końcu hostowej sceny nie może zacząć się druga lokalna scena
   guesta; odczekajcie co najmniej 7 sekund przed dalszą jazdą.
3. Wynik `ANIMSCENE_REPLICA][ATTACHED` oznacza zgodną lokalną authored scenę:
   oczekujemy jej aktorów, gestów i audio. Wynik
   `ANIMSCENE_REPLICA][PROXY_CAST_FALLBACK` oznacza brak zasobu: guest ma zobaczyć
   stabilizowaną obsadę hosta bez T-pose/ciągłego teleportu, ale gesty i lipsync
   nie są w tym trybie gwarantowane. Sam `SAFE_FALLBACK` bez widocznej obsady jest
   błędem regresji.
4. Po cutscence sprawdźcie sterowanie, brak loading/Mission Failed, brak widmowych
   koni i możliwość jazdy guesta. Potem powtórzcie scenę ze wspólnym skipem.

## 5. Logi

Po teście wyeksportujcie dwa niezmienione ZIP-y:

- `AnimGraphReplicaV30.3-HOST.zip`
- `AnimGraphReplicaV30.3-GUEST.zip`

W logu co pięć sekund powinien występować wpis
`ANIMGRAPH_REPLICA v30.3/cutscene-mission-lasso-recovery` z licznikami kierunku, obrotu,
IK i wody. Dla stojących strzałów sprawdźcie `fire-events`, `fire-graph-pulses`
i `fire-ammo-restores`; dla osłony `cover-reacquires`,
`cover-fallback-recoveries` i `stealth-recoveries`. Napiszcie też, czy cutscenka
zakończyła się jako `ATTACHED`, czy
`PROXY_CAST_FALLBACK`. Dla późnej sceny guesta szukajcie
`source=authored-animscene` oraz `MISSION_SKIP][QUARANTINE`. Dla lassa sprawdźcie,
czy po `victim-restraint-lease-expired` sam `Sustain` nie tworzy nowego tasku.
W ZIP-ie powinny istnieć `MARKER_WINDOWS.md/json`; każdy marker
ma zawierać kontekst 10 sekund przed i 15 sekund po naciśnięciu F7.
