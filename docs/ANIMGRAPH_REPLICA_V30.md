# AnimGraph Replica V30 — kierunek, obrót, IK i granice dokładności

V30 rozwija istniejący silnik `ANIMGRAPH REPLICA` bez zmiany protokołu 18.
Nie jest to własny zamiennik AnimGraphu RAGE. Bridge przesyła bezpiecznie
odczytywalną semantykę, utrzymuje autorytatywny korzeń sieciowy i zleca lokalnemu
RDR2 uruchomienie jego natywnych grafów ruchu, broni, wody oraz fizyki.

## Zaimplementowane w V30

- Konwersja world velocity do natywnej konwencji headingu RDR2: `0°` wskazuje
  `+Y`, a dodatni obrót prowadzi w stronę `-X`. Poprzednia konwencja matematyczna
  mogła przesuwać kierunek o 90°.
- Projekcja prędkości na lokalne osie peda oraz rozróżnienie: przód, tył, lewo,
  prawo i cztery kierunki ukośne.
- Zmiana kierunku odświeża task wizualny po minimalnym czasie ochronnym, więc
  szybki zwrot, backpedal i strafe przy celowaniu nie pozostają w starym tasku.
- Postać stojąca nie dostaje już natychmiastowego obrotu headingu. Bounded
  `TASK_ACHIEVE_HEADING` pozwala natywnemu grafowi wykonać obrót stóp i bioder.
- Na replice są jawnie włączone natywne solvery arm/head/leg/torso IK. Jest to
  lokalne dopasowanie do broni i podłoża, nie kopiowanie kości drugiego gracza.
- `PlayerAction.normalizedPhase` niesie znormalizowany zegar transakcji
  Begin/Sustain/End. Nie jest oznaczany jako dokładna faza klipu RAGE.
- Wolne bity istniejącego `PlayerState` przenoszą `InWater`, `Swimming` i
  `SwimmingUnderwater`. Odbiorca mierzy osobno oczekiwany i faktycznie
  uruchomiony lokalny stan wodny. Pionowa prędkość pływaka nie uruchamia już
  błędnie obsługi spadania z krawędzi.
- Diagnostyka raportuje `direction-transitions`, `turn-in-place-task-starts`,
  `ik-preparations`, `water-expected/observed` oraz
  `swimming-expected/observed`.
- V30.2 uruchamia dla stojącego strzału krótki natywny fire-task przy lokalnie
  pustej broni. Task przesuwa graf broni do gałęzi strzału/recoil, lecz nie może
  utworzyć szkodliwego pocisku; osobny pocisk prezentacyjny nadal ma obrażenia 0,
  a amunicja i stan magazynka są przywracane po 170 ms.
- V30.2 usuwa kolizję przejścia stealth→cover i mierzy utratę crouch/cover.
  Watchdog ponawia stan z cooldownem, bez per-frame resetowania AnimGraphu.
- V30.3 wcześniej identyfikuje peer jako cel lassa z wąskiego promienia kamery,
  jeżeli silnik nie zwrócił jeszcze żadnego target handle. Stary target-only
  `Sustain` nie może ponownie uruchomić tasku po zwolnieniu constraintu.
- Fallback cutscenki używa hostowego grafu encji do stabilizowanej, niekolizyjnej
  obsady proxy. Nie jest to dokładna replika AnimScene: gesty, lipsync, dialogowe
  klipy i faza pozostają dostępne wyłącznie przy zgodnym lokalnym `ATTACHED`.

## Stan pozostałych grup

| Grupa | Stan po V30 |
|---|---|
| Walk/run/sprint, start–stop | Natywny task i dokładny desired blend; progi gait mają histerezę. |
| Strafe/backpedal | Kierunek jest poprawnie przesyłany. Natywny aim-task potrafi dobrać strafe/backpedal; zwykły niecelowany `TASK_GO_STRAIGHT` nadal może obrócić ciało zamiast wykonać dokładny klip boczny. |
| Cios | Jedna ograniczona natywna akcja na Begin; V29.6 nie kasuje jej przed kontaktem. Konkretny klip/combo wybiera lokalny RDR2. |
| Blok i grapple/tackle | Stan i wspólny zegar są przesyłane, a ofiara ma autorytatywny knockdown. Brak bezpiecznego native’a wybierającego identyczną parę klipów atakujący–ofiara. |
| Lasso/hogtie | Prawdziwe `weapon_lasso`, natywny task i constraint mają pierwszeństwo; brak sztucznego ragdolla. Dokładny klip hogtie nie jest odczytywany. |
| Broń | Wybór broni, amunicja, celowanie, strzał i reload są replikowane. V30.2 dodaje bezpieczny natywny impuls grafu strzału/recoil dla stojącej repliki; SDK nadal nie podaje stabilnego identyfikatora konkretnego klipu draw/holster/recoil. |
| Koń | Wspólna tożsamość, jazda i zejście używają native’ów; wejście na replikę nadal wymaga awaryjnego osadzenia, bo przypięty SDK nie ma zweryfikowanego `TASK_MOUNT_ANIMAL`. |
| Woda | Semantyka i weryfikacja grafu są gotowe; właściwy graf wybiera lokalny wolumen wody RDR2. |
| Drabiny/scenariusze/loot/carry | Nie mają jeszcze wystarczającego, jednoznacznego stanu źródłowego ani identyfikatora tasku/obiektu. |
| Mimika/lipsync/spojrzenie | Brak strumienia fonemów i eye targetu. Lokalny head/torso IK jest tylko bezpiecznie włączony. |
| Dokładna faza klipu | Payload jest przygotowany, lecz pola clip/phase pozostają nieważne bez wersjonowanego read-only readera dla dokładnego hasha `RDR2.exe`. |

## Zasada bezpieczeństwa

Kod nie wpisuje wymyślonych hashy klipów, nazw move-networków ani offsetów
pamięci. `VersionedMemoryReader` pozostaje wyłączony. Dokładne pary tackle,
hogtie, mount i lipsync wymagają najpierw potwierdzonego źródła klipu i fazy;
w przeciwnym razie pozornie „dokładna” animacja byłaby bardziej niestabilna niż
natywny wybór lokalnego silnika.
