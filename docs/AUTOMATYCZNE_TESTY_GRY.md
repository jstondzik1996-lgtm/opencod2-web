# Automatyczne testy prawdziwej gry CoD2

System korzysta z istniejącego klienta C/Emscripten, oryginalnych lokalnych zasobów i serwera Win32. Nie zmienia renderera ani systemu obrażeń.

## Uruchomienie po zbudowaniu klienta i serwera

W środowisku Python z pakietami Playwright i websockets:

```powershell
python tools/run_browser_game_test.py --web-root "ŚCIEŻKA/DO/WEB" --server-exe "ŚCIEŻKA/DO/cod2_win32_ded.exe" --game-root "ŚCIEŻKA/DO/CZYSTEGO-SERWERA" --source-revision "SHA-KOMITU" --output "qa-reports"
```

Klient musi być zbudowany z `COD2_WEB_TESTING=ON`. Katalog web zawiera `cod2.html`, `cod2.js`, `cod2.wasm`, `assets.json` i archiwa wskazane w manifeście. Dozwolony jest istniejący lokalny junction `main/`. Czysty katalog serwera ma własne `main/*.iwd`. Nie używaj katalogu z dodatkowymi modami.

Jeśli Playwright nie ma pobranego Chromium, wskaż zainstalowany Chrome przez `--browser "C:/Program Files/Google/Chrome/Application/chrome.exe"`. Alternatywnie zainstaluj przeglądarkę poleceniem `python -m playwright install chromium`. Narzędzie nie instaluje nic samodzielnie.

`--preflight-only` kontroluje pliki i indeksy ZIP, bez uruchamiania gry. Zwraca kod 1, ponieważ test rozgrywki pozostaje niewykonany. Indeksy i rozmiary nie stanowią pełnego testu CRC każdego pliku w archiwum.

## Dotychczasowy BAT

`tools/RUN_UPDATED_LOCAL_TEST.bat` przyjmuje teraz obie lokalizacje: własny katalog `tools/` w repozytorium i wcześniejszy katalog `work/`. Nadal korzysta z istniejących SDK i zasobów obok repozytorium. Buduje oddzielną rewizję, potem uruchamia automatyczny test zamiast pozostawiać ręcznie otwarte usługi.

Do sprawdzenia tego PR przed scaleniem ustaw w PowerShell:

```powershell
$env:COD2_QA_BRANCH = "codex/automated-game-tests"
$env:COD2_QA_NONINTERACTIVE = "1"
.\tools\RUN_UPDATED_LOCAL_TEST.bat
```

Po scaleniu domyślna gałąź pozostaje `codex/browser-mp-checkpoint`. Wybór `COD2_QA_MEMCHECK=1` pozostaje dostępny; pomiar FPS z SAFE_HEAP nie jest porównywalny z kompilacją do grania. BAT wymaga dostępu do GitHub podczas pobierania wybranej rewizji. Ręczne polecenie Pythona może pracować z już zbudowanymi lokalnymi plikami.

## Etapy i kryteria

| Obszar | Kontrola |
|---|---|
| Start | Pliki i nagłówek WASM/MZ, indeksy modeli/materiałów, odpowiedź UDP, poprawna mapa DM, katalog serwerów, MIME WASM, relay i stan obu klientów |
| Zasoby | Błędy ładowania modeli/materiałów, brak DObj, błędy HTTP i nieudane żądania przeglądarki |
| Płynność | 12 sekund po rozgrzaniu; średnio co najmniej 20 klatek/s, p95 do 150 ms, pojedyncza przerwa do 1000 ms |
| Synchronizacja | Obaj aktywni klienci, zgodny client/psClient, brak cofania czasu, zastój do 3000 ms, skok migawki i odchylenie zegara do 1500 ms |
| Trafienia | Ubytek amunicji i spadek zdrowia celu po strzelaniu; zdrowie nie jest ustawiane przez test |
| Cykl życia | Śmierć, ponowne 100 HP, potwierdzone rozłączenie, żywe migawki drugiego klienta, reconnect i dalszy postęp migawek |
| Zawieszenie | Osobny proces nadzoruje zakończenie usług, odpowiedzi UDP i limit czasu harnessu; nie zależy od odpowiedzi page.evaluate |

Progi: `--min-fps`, `--max-p95-ms`, `--observe-seconds`, `--max-stall-ms`, `--max-gap-ms`, `--timeout` (domyślnie 600 s). Progi muszą być dodatnie i skończone; obserwacja trwa co najmniej 6 s.

Próbnik JavaScript liczy takty requestAnimationFrame, pomiędzy którymi wystąpiły wywołania draw WebGL. Samo działanie rAF bez rysowania nie daje klatek. To przybliżenie płynności prezentacji, ograniczone odświeżaniem przeglądarki i narzutem próbnika; nie jest pomiarem czasu GPU ani dokładnej liczby pętli silnika. Mierzony jest pierwszy klient na pierwszym planie przy równoczesnym działaniu drugiego. Zapisywany jest renderer ujawniony przez WebGL. Headless/SwiftShader należy porównywać z tym samym środowiskiem, a nie z wynikiem użytkownika na GPU.

Brak komunikatów o brakujących modelach nie dowodzi prawidłowego wyglądu wszystkich obiektów. Zrzuty wymagają oceny, zwłaszcza w sprawie kolorów i niewidocznych postaci. Zdrowie pochodzi z migawek; test nie stanowi pełnej walidacji wszystkich rodzajów broni, kolizji i trybów gry.

## Raport i izolacja

Każdy przebieg dostaje nowy katalog. Wynik końcowy PASS wymaga wszystkich kontroli PASS. FAIL oznacza niespełnione kryterium albo przerwany przebieg; NIE_WYKONANO oznacza brak dowodu wykonania etapu. Brak zasobów, wyjątek albo timeout nigdy nie dziedziczy PASS ze starego JSON.

Pliki: `raport.md`, `raport.json`, `multiplayer-result.json` (po ukończonym scenariuszu), `worker-error.json` (przy wyjątku), `server-health.json`, `log-findings.json`, logi serwera/katalogu/HTTP/relay/przeglądarki oraz zrzuty PNG poszczególnych etapów. Przy zawieszeniu klienta screenshot może być niemożliwy; raport mówi o jego braku zamiast tworzyć zastępczy obraz.

Usługi wiążą się wyłącznie z 127.0.0.1 i otrzymują wolne porty przydzielone przez system. Małe okno między rezerwacją a startem pozostaje; konflikt powoduje FAIL zamiast przejmowania istniejącej usługi. HTTP działa z `--no-spawn`, a nadzorca sam uruchamia wszystkie procesy, więc zna ich PID. Sprząta wyłącznie własne procesy, w tym potomny Chromium. Nie zatrzymuje istniejących podglądów i nie zapisuje do źródłowego repozytorium.

Test używa nowych profili bez CD-Key i własnych sesji gościa z ticketami relay. Klucze, tokeny w URL/JSON i nagłówki Bearer są anonimizowane w tekstowych dowodach. Nie publikuj katalogów profili, IWD, binariów ani prywatnych zasobów. Oryginalne komunikaty silnika pozostają w logach w języku silnika; raport oceniający jest po polsku.

## Sprawdzenie infrastruktury bez gry

```text
python -m unittest discover -s tools -p "test_browser*.py" -v
python -m py_compile tools/browser_multiplayer_check.py tools/run_browser_game_test.py
```

Nowy workflow uruchamia te kontrole na Windows i Linux oraz zachowuje log i raport kontrolowanego błędu brakujących plików. Dane syntetyczne i pozorowany kontekst WebGL służą wyłącznie sprawdzeniu narzędzi. Zielone CI nie potwierdza działania prawdziwej rozgrywki.
