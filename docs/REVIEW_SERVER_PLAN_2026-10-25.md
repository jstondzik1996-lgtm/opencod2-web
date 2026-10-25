# CoD2 Browser MP — współpraca i plan serwerów (recenzent techniczny, 2026-10-25)

> **Wspólne repo:** `jstondzik1996-lgtm/opencod2-web`. Ten dokument jest propozycją w oddzielnej gałęzi review. **Nie zmienia kodu Claude'a, nie zastępuje `HANDOFF.md` ani `docs/WORK_LOG.md` i nie potwierdza zaliczenia testów gry.** Work/Claude powinni przenosić zaakceptowane instrukcje do swoich kroków i aktualizować HANDOFF/WORK_LOG sami.

## Zrozumienie celu użytkownika
- Oryginalny gameplay/multi CoD2 1.3, ładowany w przeglądarce, bez singla.
- Każdy odwiedzający stronę widzi lobby: **Szybka gra / Serwery / Prywatny pokój**.
- Domyślnie dołączamy do **naszych własnych instancji** z wolnymi miejscami. Jeśli #1 jest pełny, wybieramy #2; jeśli wszystkie pełne: kolejka lub uruchomienie następnej instancji dopiero po sprawdzeniu dostępnych zasobów.
- Docelowy UX nie wymaga ręcznego wpisania CD-key do formularza. **To NIE jest zgoda na keygen, crack, zastępowanie cudzych kluczy, emulowanie oficjalnej autoryzacji lub obchodzenie kontroli cudzych serwerów.** Tożsamość/autoryzacja na naszych serwerach może być niezależną usługą projektową po sprawdzeniu możliwości technicznych i prawnych.
- Granie bez klucza nie oznacza, że wolno publicznie hostować oryginalne archiwa IWD. Publiczna dystrybucja plików wymaga odpowiednich uprawnień. Do prototypu pliki legalnie posiadane przez testującego.

## Ustalony stan źródła
- Gałąź implementacyjna: `codex/browser-mp-checkpoint`; odczytany commit bazowy `4ba720d635493be9eca2c9cf62c631510a6d0e09`. **Przed pracą należy sprawdzić nowy HEAD**.
- W `HANDOFF.md` Claude raportuje naprawę `cm_trace.c:CM_TempBoxModel` (`leaf.brushContents`) i 1-based indices / zero sentinel w `cm_world.c`. Test jednostkowy `tests/cm_world_link/run.sh` wg zapisu przeszedł, ale **NIE MA POTWIERDZENIA REALNEGO TESTU OBRAŻEŃ DWÓCH GRACZY**. Nie mylić testu list encji z combat PASS.
- Istnieją `tools/master_proxy/server.py`, `tools/game_proxy/relay.py`, `src/web/web_master.c`, `src/web/remote-config.js`. Master proxy używa aktualnie publicznego `api.cod.pm`, natomiast docelowy wybór serwerów ma domyślnie używać **naszego katalogu**.
- `sv_client_mp.c` ma `SV_GetChallenge`, `SV_AuthorizeRequest`, `SV_AuthorizeIpPacket`, `SV_DirectConnect`, a `sv_init_mp.c` rejestruje `sv_allowAnonymous`. Sam przełącznik nie jest potwierdzonym sposobem na dopuszczenie anonimowych klientów. Nie zakładać, że `net_lanauthorize 0` rozwiązuje publiczne logowanie.

## Priorytety dla Claude + Work

### P0 — walidacja walki (blokujące)
1. Work: aktualizacja lokalnego checkoutu do commitów Claude'a, kompilacja x86 dedicated + WASM; oddzielne porty **8081 / 20721 / 20722 / 28961**. Preview 8080/28960 pozostawić.
2. Test `tools/browser_multiplayer_check.py`: 2 realne klienty, różne ID, strzelanie, **HP celu maleje**, zabicie, respawn, rozłączenie i ponowne wejście, snapshoty stale rosną. Wynik JSON i wyczyszczone logi do `docs/evidence/`; błędny assert nadal musi pozostać FAIL.
3. Jeśli FAIL: śledzić `CM_LinkEntity` (contents), przejścia indeksów 1-based i sentinel 0, `CM_AreaEntities`, `SV_PointTraceToEntity`, `trace.entityNum/fraction` oraz `DObjTraceline`. Zróżnicować ścianę (1022) od trafienia gracza. Nie robić „sztucznego HP”.

### P1 — własne serwery i lobby
1. Zdefiniować kontrolowaną listę serwerów (id, nazwa, region, host:port, mode, map, maxPlayers, players, protocol, status, lastSeen). Odpytywanie zdrowia / `getstatus`, niestare liczniki i błędy offline.
2. Endpoint np. `GET /play/api/servers`: wyłącznie aktywne **nasze** serwery. Używać go w launcherze jako podstawy; publiczny master (jeśli zostanie) oznaczyć osobno jako „zewnętrzny”, z rzeczywistymi wymaganiami autoryzacji. Nie sugerować kompatybilności wszystkich zewnętrznych serwerów.
3. `POST /play/api/matchmake` z trybem/regionem: selekcja po kompatybilności, wolnym slocie, ping i optymalnym wypełnieniu. Używać krótkiej rezerwacji/admission TTL albo autorytatywnej odpowiedzi serwera, żeby 2 kandydaci nie zajęli tego samego slotu 32/32. W razie odmowy retry następną instancję, nie przełączać trwającego meczu.
4. Przeglądarkowy UI: „Szybka gra”, lista serwerów, „Dołącz”, pełny serwer, utrata sesji, reconnect. Rozmieszczać nowe osoby po serwerach, nie przenosić graczy już grających.
5. Lokalny test dwóch dedykowanych instancji, początkowo maxclients 2–4; pełna instancja A → wejście na B. Obsłużyć równoczesne join, timeout i offline bez fałszywego zapewnienia wejścia.
6. „32 graczy” to produktowy cel, **niepomierzony wynik**. Przejść próby obciążeniowe 4→8→16→32 i mierzyć serwerowe CPU, przepustowość, opóźnienia, utracone snapshoty oraz render klienta.

### P2 — własna tożsamość gracza bez ręcznego CD-key (tylko własne serwery)
1. Projektowy nick + trwały pseudolosowy identyfikator albo zalogowane konto i krótko żyjący podpisany token sesji. Nasz backend waliduje admisssion/ban/kick/rate limit i przypina token do konkretnego serwera.
2. Zweryfikować kod autoryzacji serwera i zaproponować **osobny, legalny tryb uwierzytelniania** dla własnego rekonstruowanego serwera, bez podszywania się pod autoryzację wydawcy. Klient ma odróżniać wejście na własny serwer od cudzych.
3. Nie przekazywać CD-key przez JS, logi, websocket, masterlist; nie używać jednego współdzielonego klucza wielu graczy. Nie dostarczać czyichś oryginalnych plików osobom bez praw do nich.
4. Test: 2 nowe profile przeglądarki, 2 odrębne sesje, brak ręcznego wpisywania klucza, odłączenie/ponowne wejście i zgodne bany; obie osoby mają legalny dostęp do plików gry (albo do odpowiednio licencjonowanego pakietu).

### P3 — bezpieczeństwo + publiczny hosting
- Produkcyjny WS/UDP relay musi ograniczać cele do zatwierdzonych host:port; `COD2_RELAY_ALLOW_ANY=1` tylko lokalnie, **nigdy publicznie**. Rate limits, limity bandwidth, izolacja backendu, TLS/WSS, blokada SSRF/open proxy, obsługa nadużyć.
- Hosting i routing publiczny po realnym PASS P0 i weryfikacji legalności dystrybucji assetów. 1 VPS może uruchamiać kilka instancji dedykowanych na różnych portach — nie oznacza to „automatycznej” skalowalności; autoscaling wymaga osobnego managera i limitów.

## Warunki odbioru
- P0: prawdziwe combat PASS z dowodem i bez wymuszania HP.
- P1: dwa własne serwery w lokalnym lobby, quick-join do wolnego, pełny A→B, reconnect.
- P2: dwie niezależne sesje bez wpisywania klucza do UI, w ramach dopuszczalnych prawnie własnych serwerów i legalnych assetów.
- P3: przetestowane bezpieczeństwo/rate limits i poprawne zachowanie przy serwerze offline.
- Każde potwierdzenie zapisane w `docs/WORK_LOG.md` i `docs/evidence` z aktualnym SHA; nie zakładać wykonania bez dowodu.

## Rozdział pracy i komunikacja
**Claude:** pisze zmiany do gałęzi implementacyjnej. **Work:** ma dostęp do lokalnych plików/kompilatorów i robi testy oraz raportuje wyniki. **Recenzent (ChatGPT):** przegląda, publikuje zalecenia, nie zmienia jednocześnie kodu na gałęzi Claude'a. Żadnego osobnego klona produktu.
