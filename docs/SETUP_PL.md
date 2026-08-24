# RDR2 Coop Story — przygotowanie środowiska

Ten dokument opisuje bezpieczne przygotowanie projektu i późniejsze uruchamianie wersji deweloperskiej. Obecność kodu lub poprawny build nie oznaczają jeszcze, że pełna kampania działa w coop. Do czasu testu na dwóch komputerach potwierdzamy wyłącznie testy lokalne i symulator peera.

## Zasady przed rozpoczęciem

- Projekt działa wyłącznie w Story Mode i w prywatnej sieci LAN albo Hamachi
  IPv4 `25.x.x.x`; IPv6 nie jest obsługiwane.
- Nie uruchamiaj Red Dead Online z plikami moda w katalogu gry.
- Bridge ma bezwarunkowo blokować działanie po wykryciu sesji online/RDO. Nie dodajemy przełącznika omijającego tę blokadę.
- Zamknij RDR2 przed baseline, instalacją i odinstalowaniem.
- Skrypty uruchamiaj w Windows PowerShell 5.1 jako zwykły użytkownik. Uprawnienia administratora nie są potrzebne do operacji w workspace ani do gry zainstalowanej w katalogu zapisywalnym przez użytkownika.
- Skrypty nie pobierają oprogramowania i nie instalują reguł zapory.

## Wymagane składniki

Jeśli któregoś składnika brakuje, użytkownik pobiera go ręcznie z oficjalnego źródła. Obecnego, kompletnego toolchainu VS 2026 nie trzeba zastępować ani pobierać ponownie:

1. Visual Studio z workloadem C++:
   - aktualnie wykryty i używany lokalnie: Visual Studio 2026 (generator 18) z toolsetem MSVC `14.51.36231` i CMake `4.3.1`;
   - pierwotnie planowany kanał Build Tools 2022 `17.14` pozostaje obsługiwanym wariantem, ale jego brak jest ostrzeżeniem, jeśli nowszy kompletny toolchain przechodzi weryfikację;
   - workload `Desktop development with C++`;
   - kompilator MSVC Hostx64/x64;
   - Windows 11 SDK `10.0.26100.0`;
   - C++ CMake tools for Windows;
   - MSBuild.
2. Script Hook RDR2:
   - runtime `1.0.1491.17`;
   - SDK `1.0.1207.73`.

Nie instaluj `NativeTrainer.asi`. Nie dodawaj RedM, LML, ScriptHookDotNet ani innych loaderów na potrzeby tego projektu. ScriptHook pozostaje osobną zależnością użytkownika i nie trafia do `dist`.

Obsługiwany pierwszy build gry:

- wersja pliku: `1.0.1491.50`;
- SHA-256 `RDR2.exe`: `B56C9548F670654A9B73BF25DEF3CD73AF12E269F6E47DBA28A34079ADAF465E`.

Inny hash jest twardą blokadą uruchomienia wersji deweloperskiej. Nie wyłączaj tej kontroli.

## 1. Weryfikacja bez zmian na dysku

Otwórz Windows PowerShell 5.1 w katalogu projektu:

```powershell
Set-Location 'C:\sciezka\do\RDR2-Coop-Story'
.\scripts\Verify-Prerequisites.ps1
```

Skrypt:

- szuka instalacji Steam przez `libraryfolders.vdf` i `appmanifest_1174180.acf`;
- przy innym launcherze przyjmuje jawne `-GamePath`;
- sprawdza dowolny kompletny toolchain Visual Studio C++, osobno raportuje planowany kanał 17.14, oraz weryfikuje MSVC, CMake, Windows SDK i .NET 10;
- ogląda zawartość katalogu lub ZIP-a SDK/runtime bez rozpakowywania;
- sprawdza wersję i hash `RDR2.exe`;
- nie wypisuje lokalnych ścieżek, nazwy użytkownika ani zawartości manifestów Steam.

Przykład dla instalacji spoza Steam:

```powershell
.\scripts\Verify-Prerequisites.ps1 -GamePath 'D:\Games\Red Dead Redemption 2'
```

Kod wyjścia `0` oznacza brak błędów. `WARN` nie zatrzymuje przygotowania, ale trzeba go świadomie rozwiązać przed testem gry. `FAIL` zatrzymuje build lub wdrożenie.

## 2. Baseline czystej gry

Najpierw obejrzyj plan:

```powershell
$game = 'D:\Games\Red Dead Redemption 2'
.\scripts\Capture-GameBaseline.ps1 -GamePath $game -WhatIf
```

Następnie zapisz baseline:

```powershell
.\scripts\Capture-GameBaseline.ps1 -GamePath $game
```

Domyślny wynik trafia do `artifacts\baselines`. Skrypt odmawia nadpisania pliku. Zapisuje metadane elementów top-level, hashuje wyłącznie jawną listę krytycznych plików i wykrywa typowe loadery/moduły (`dinput8.dll`, ScriptHook, pliki `.asi`, LML, Rampage, RedM). Nie hashuje wszystkich archiwów gry ani całej instalacji.

Najlepiej wykonać pierwszy baseline przed ręcznym zainstalowaniem runtime ScriptHooka. Kolejny baseline po instalacji ułatwi porównanie.

## 3. Copy-only backup save’ów

Wskaż jawny katalog docelowy poza folderem profili. Najpierw dry-run:

```powershell
.\scripts\Backup-Saves.ps1 `
  -DestinationRoot 'D:\RDR2-Coop-Backups' `
  -WhatIf
```

Domyślne źródło to folder `Rockstar Games\Red Dead Redemption 2\Profiles` pod bieżącym katalogiem Dokumenty, również gdy Dokumenty są przekierowane do OneDrive. Jeśli save’y są gdzie indziej:

```powershell
.\scripts\Backup-Saves.ps1 `
  -SourcePath 'C:\jawna\sciezka\do\Profiles' `
  -DestinationRoot 'D:\RDR2-Coop-Backups'
```

Skrypt:

- tylko odczytuje źródło i kopiuje pliki;
- nigdy nie przenosi ani nie usuwa save’ów;
- odrzuca reparse pointy i nakłada limit liczby oraz sumarycznego rozmiaru plików;
- tworzy nowy katalog z timestampem i odmawia nadpisania;
- liczy SHA-256 każdego pliku, weryfikuje kopię i zapisuje `backup-manifest.json`.

Nie kontynuuj testów w grze, dopóki backup nie zakończy się komunikatem z liczbą plików i hashem zestawu.

## 4. Kontrakt `dist`

Najpierw zbuduj i uruchom self-test sidecara:

```powershell
dotnet build .\CoopStory.slnx -c Release
dotnet run --project .\tests\CoopStory.SelfTest\CoopStory.SelfTest.csproj -c Release
```

Rdzeń/symulator C++ niezależny od SDK buduje się przez:

```powershell
cmake --preset bridge-vs2026
cmake --build --preset bridge-vs2026-release
ctest --preset bridge-vs2026-release
```

Brak kompletnego Visual Studio C++/MSVC/CMake jest twardym blockerem części
C++. Sam zainstalowany .NET 10 nie wystarcza. Build właściwego `.asi` wymaga
dodatkowo ustawienia `SCRIPT_HOOK_RDR2_SDK_DIR` na lokalny, rozpakowany SDK;
nie kopiujemy SDK do repo ani do `dist`:

```powershell
$env:SCRIPT_HOOK_RDR2_SDK_DIR = Join-Path (Get-Location) `
  'ScriptHookRDR2_SDK_1.0.1207.73'
cmake --preset bridge-asi-vs2026
cmake --build --preset bridge-asi-vs2026-release
ctest --preset bridge-asi-vs2026-release
```

Uruchom te polecenia w Developer PowerShell for VS 2026, aby `cmake` i
`ctest` były dostępne w `PATH`.

Po warning-clean rebuildzie `.asi` utwórz paczkę najpierw w dry-runie:

```powershell
$bridge = Join-Path (Get-Location) `
  'build\bridge-asi-vs2026\src\CoopStory.Bridge\Release\CoopStoryBridge.asi'
.\scripts\Stage-DevPackage.ps1 -BridgePath $bridge
```

Jeśli fresh-source, PE x64 DLL, config i lokalne assets NuGet przechodzą walidację:

```powershell
.\scripts\Stage-DevPackage.ps1 -BridgePath $bridge -Apply
```

Stage wykonuje framework-dependent `dotnet publish --no-restore`, więc nigdy nie pobiera ani nie przywraca paczek. Tworzy nowy, unikalny staging, odrzuca warningi i zabronione pliki ScriptHook/loader/trainer, a następnie atomowo przenosi kompletną paczkę jako nowy `dist`. Jeśli `dist` już istnieje, skrypt odmawia nadpisania lub łączenia. Nie wykonuje delete; nieudany częściowy staging pozostaje w `artifacts\staging` do jawnej inspekcji.

Po stagingu katalog musi mieć co najmniej:

```text
dist/
  CoopStoryBridge.asi
  config/
    coopstory.example.json
  sidecar/
    CoopStory.Sidecar.exe
    ...pozostałe pliki publish...
```

Paczka `dist` nie może zawierać `ScriptHookRDR2.dll`, `dinput8.dll` ani `NativeTrainer.asi`. Sidecar zawsze działa z `dist\sidecar`; nie kopiujemy go do katalogu gry.

## 5. Runtime ScriptHook — dopiero przed testem gry

Po poprawnym baseline i backupie użytkownik może ręcznie skopiować z oficjalnego runtime wyłącznie:

- `bin\ScriptHookRDR2.dll`;
- `bin\dinput8.dll`.

Nie kopiuj `NativeTrainer.asi`. Skrypty projektu celowo nie wykonują tego kroku i nie przechowują runtime w `dist`.

Po ręcznej instalacji ponownie uruchom `Verify-Prerequisites.ps1 -GamePath $game`.

## 6. Wdrożenie wersji deweloperskiej

Installer domyślnie jest dry-runem:

```powershell
.\scripts\Install-DevBuild.ps1 -GamePath $game
```

Jeśli plan pokazuje wyłącznie dwa pliki projektu, wykonaj:

```powershell
.\scripts\Install-DevBuild.ps1 -GamePath $game -Apply
```

Do katalogu gry trafiają tylko:

- `CoopStoryBridge.asi`;
- `CoopStory.config.json` — kopia `dist\config\coopstory.example.json`.

Manifest powstaje poza grą jako `artifacts\deploy\deployment-manifest.json`. Installer odmawia nadpisania istniejącego bridge’a, configu albo manifestu, waliduje hash gry, JSON configu i zawartość sidecara. Nie kopiuje ScriptHooka.

Edytuj lokalny `CoopStory.config.json` dopiero po instalacji. Ustaw `role` na `Host` albo `Guest`, adres hosta, porty i unikalny token sesji. Pozostaw `safety.storyModeOnly` i `safety.refuseOnlineMode` ustawione na `true`; installer odrzuca config bez obu blokad. Config jest oznaczony w manifeście jako plik modyfikowalny; uninstaller zachowa jego aktualną wersję w backupie.

## 7. Uruchomienie

Zawsze uruchamiaj Story Mode. Najpierw sidecar, potem grę:

```powershell
.\dist\sidecar\CoopStory.Sidecar.exe run --config "$game\CoopStory.config.json"
```

Do testu jednego PC służy syntetyczny peer:

```powershell
.\dist\sidecar\CoopStory.Sidecar.exe simulate `
  --config "$game\CoopStory.config.json" `
  --duration 10
```

Na drugim PC wykonaj ten sam baseline, backup, weryfikację hasha gry i instalację. Jeden config ma `role: "Host"`, drugi `role: "Guest"`. Oba komputery muszą używać identycznego protokołu i buildów. TCP `43120` oraz UDP `43121` należy dopuścić wyłącznie w profilu sieci prywatnej; reguły zapory tworzy użytkownik świadomie.

F9 otwiera menu awaryjne. Podczas problematycznego fragmentu misji host włącza `Solo override`: guest przechodzi do spectator mode, a po fragmencie wraca obok hosta. Nie dodajemy adapterów per misja.

## 8. Bezpieczne odinstalowanie

Najpierw dry-run:

```powershell
.\scripts\Uninstall-DevBuild.ps1 -GamePath $game
```

Następnie:

```powershell
.\scripts\Uninstall-DevBuild.ps1 -GamePath $game -Apply
```

Uninstaller ufa wyłącznie manifestowi z workspace. Przed usunięciem kopiuje i hashuje każdy obecny plik do `artifacts\uninstall-backups`. Niezmieniony bridge musi mieć hash z manifestu; w przeciwnym razie operacja jest zatrzymywana. Modyfikowalny config jest zawsze zabezpieczany w aktualnej wersji. ScriptHook, inne mody i sidecar pozostają nietknięte.
