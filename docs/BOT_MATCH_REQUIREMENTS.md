# WWII Browser Multiplayer — tryb domyślny: 1 gracz + 15 botów

**Decyzja właściciela projektu: 2026-10-26.**

**Docelowa konfiguracja pierwszej grywalnej wersji: jeden prawdziwy gracz i piętnaście botów sterowanych przez autorytatywny serwer, trudność `normal`.** To wymaganie produktu, **nie informacja, że obecny silnik już je obsługuje**. Przed publiczną premierą musimy też dopuścić wielu prawdziwych graczy, wtedy boty ustępują miejsca ludziom.

## 1. Źródło prawdy: wymagane wartości

Kontrakt docelowego silnika/serwera — wartości do implementacji w **jednej** konfiguracji, odczytywanej przez serwer, lobby, testy i administrację. Nie zapisuj oddzielnych sprzecznych stałych w kliencie i serwerze.

```json
{
  "mode": "deathmatch",
  "defaultHumanCount": 1,
  "defaultBotCount": 15,
  "difficulty": "normal",
  "maxMatchParticipants": 16,
  "botsEnabled": true,
  "botFillEnabled": true,
  "replaceBotsOnHumanJoin": true,
  "authoritativeCombat": true
}
```

Powyższy JSON jest **proponowanym kontraktem**, nie istniejącym plikiem konfiguracyjnym ładowanym przez obecną grę. Work/Antigravity mają ustalić właściwe miejsce implementacji po audycie. Normal to domyślna trudność; przyszłe ustawienia: easy, normal, hard, veteran. Liczbę botów należy pozwolić ustawić (0–15), zachowując domyślne **15**, ale start produkcyjnego serwera z 15 botami jest zależny od realnych testów wydajności i rozmiaru mapy.

**Zasada slotów:** `occupiedHumanSlots + occupiedBotSlots <= maxMatchParticipants`. Pojawienie się nowego człowieka, kiedy 16 miejsc jest zajętych, powoduje bezpieczne usunięcie bota przed zatwierdzeniem wejścia człowieka (bez kickowania ludzi); odłączony człowiek może zostać zastąpiony botem po odpowiednim okresie ochronnym. W wariancie treningowym zawsze rezerwuj miejsce dla właściciela sesji / jednego prawdziwego gracza.

## 2. Wymagania grywalności (bez atrap)

- Jeden człowiek w normalnej przeglądarce widzi wszystkie boty oraz ich prawdziwe modele, wyposażenie, pozy, animacje i aktualne HP; brak niewidzialnych encji.
- Każdy bot ma tożsamość (np. `BOT_01`), team/mode, pozycję, kolizję, hitboxy, health, broń/amunicję, stan żywy/martwy i cooldown respawnu. Scoreboard odróżnia botów od ludzi.
- Bot generuje wejścia ruchu analogiczne do `usercmd` człowieka i używa **tej samej** implementacji przyspieszania, grawitacji, skoku, schodów, kucania, wychylania, drabin, ADS, recoil, firing, reload, trafień, G_Damage/HP i respawnu. Nie wykonuje sztucznych teleportów ani obrażeń bez trafienia.
- Serwer jest autorytatywny: oblicza ruch, animacje stanu, line-of-sight, strzały, hit detection, damage, deaths i respawns. Klient wyświetla wynik, przewiduje własny ruch i interpoluje encje bez bot-teleportów i desynchronizacji.
- Bot AI: patrol/waypointy lub navmesh, omijanie przeszkód, śledzenie widocznego przeciwnika, detekcja line-of-sight, celowanie i ostrzał tylko po spełnieniu warunków, przeładowanie, wybór osłon, unikanie utknięcia; drabiny i obiekty specjalne obsługiwane przez nawigację. Stuck detection + recovery bez naruszania mechaniki.
- Trudność `normal`: parametry czytelne i mierzalne (czas reakcji, błąd celowania, tracking, zasięg percepcji, burst/cooldown, taktyka), bez wallhacka, magicznych strzałów czy aimbota. Nie wpisywać rzekomych dokładnych wartości CoD2 bez pomiaru; kalibracja w prywatnym środowisku referencyjnym i testy.
- Każdy bot walczy **w czasie rzeczywistym** z graczem i innymi botami, trafia/otrzymuje HP, umiera, odradza się i synchronizuje. Strzelanie ma efekty i dźwięki z legalnych źródeł; publiczny build bez proprietary assets.
- Zmiana liczby botów/trudności (menu serwera/lobby) nie powinna wymagać przebudowy silnika. Połączenie i UI zrozumiałe dla jednego gracza: `Graj z botami`.

## 3. Architektura i zgodność z aktualnym repo

- **Nie tworzyć nowego repo**. Pracować w `jstondzik1996-lgtm/opencod2-web`; odczytać `HANDOFF.md`, `docs/WORK_LOG.md`, `docs/ROADMAP.md`, najnowszy HEAD przed edycją; oddzielny worktree/branch podczas równoległej pracy Jules/Claude/Work. Stary CoD2 port tylko lokalna baza porównawcza, jeśli przebudowany publiczny silnik ma nowe komponenty.
- **Stan zastany:** skrypt `tools/RUN_UPDATED_LOCAL_TEST.bat` wystawia tylko `maxPlayers: 4` w katalogu QA; to sprzeczne z celem **16 slotów**. Agent musi odpowiednio dostosować nie tylko opis katalogu, ale prawdziwe `sv_maxclients`/konfigurację serwera, alokację tablic, entity limits, routing i testy. Nie podnosić ślepo limitu w JSON, gdy serwer nie obsługuje 16 encji.
- Źródła wiedzy o botach: [Bot Warfare](https://github.com/ineedbots/iw2_bot_warfare), [CoD2rev](https://github.com/voron00/CoD2rev_Server), [CoD2x CoD2rev](https://github.com/callofduty2x/CoD2rev_Server). Analizować schemat waypointów, sterowania, AI i dokumentację, **nie kopiować mechanicznie kodu reverse-engineered/bez zgodnej licencji**. Implementacja może działać nad docelowym silnikiem PlayCanvas/Rapier/Colyseus lub obecnym portem po audycie praw.
- Jeśli architektura to nowy browser-native runtime: logika botów i autorytatywna symulacja na serwerze (nie 15 pełnych procesów Chromium). Przeglądarka jednego gracza rysuje 15 zdalnych encji, ale serwer simuluje każdego bota. Klient i serwer muszą uzgadniać tickrate, serializację, identyfikatory i stany animacji.
- Wszystkie modele, mapy, dźwięki i animacje w wersji publicznej muszą mieć weryfikowalny status licencji; nie importować original IWD, BSP, IWI, XModels ani oryginalnego kodu CoD2 do publicznej dystrybucji bez praw.

## 4. Etapy i warunki odbioru

**P0 — funkcjonująca walka (gate, nie liczba NPC):**
1. Przeglądarka człowieka + **1 bot**; realny serwer, realne trafienia, spadek HP, śmierć i respawn u obu stron, animacje/model widoczne. Nagranie, log, test w przeglądarce i PASS/FAIL.
2. Bot porusza się i strzela tą samą implementacją mechaniki co gracz. Walidacja rozrzutu, kolizji świata i hitboxów, nie "bot hp -= damage" po upływie timera.
3. Automat bot-vs-bot bez przeglądarki odtwarza deterministyczny test przy ustalonym seedzie i raportuje zwycięzcę/HP/zdarzenia.

**P1 — skala domyślna:**
4. **1 człowiek + 4 boty**, a następnie **1 + 8**, potem **1 + 15** na tej samej mapie. Dopiero zaliczony scenariusz **1 + 15** może być reklamowany jako ukończony.
5. Przy 1+15 zweryfikować pathfinding, brak utknięć i nakładających się encji, wszystkie modele/animacje, brak desynchronizacji, poprawność obrażeń, reconnect oraz uzupełnianie miejsc po zgonie/wyjściu.
6. Human joins a full 1+15 server -> one bot yields the slot (human count grows to 2, bot count to 14), scoreboard and server directory show real counts. Test kiedy bot odradza się i kiedy człowiek rozłącza się równocześnie.

**P2 — jakość i optymalizacja:**
7. Monitorować median/p95/p99 client frame time, 1% low FPS, CPU/GPU draw calls i memory, tickrate/p95 server frame time, bandwidth, packet drops, snapshot gaps, errors. Celem jest **60 FPS przy 1080p na komputerze właściciela**, lecz to kryterium do pomiaru, nie potwierdzona cecha. Dopasować budżety AI/LOD/tick tylko na podstawie testów; nie skracać funkcji mechaniki.
8. Testy latency/reconnect, AI determinism, 15-bot stress, 2+ human join bot-eviction, resource/license manifest. Boty nie mogą otrzymać nieuczciwej przewagi wynikającej z liczby ticków lub dostępu do informacji serwera.

## 5. Dowody / praca autonomiczna Antigravity

- Zacząć od aktualnego kodu i lokalnego builda, bez nadpisywania prywatnych danych ani starego preview 8080/8081. Worktree izolowany; przed integrowaniem przejrzeć zmiany innych agentów.
- Po każdym etapie: **uruchom prawdziwą grę w przeglądarce**, test botów, zbierz FPS/obciążenie, zapisz `docs/evidence/` (bez prywatnych kluczy/assetów), commit i `docs/WORK_LOG.md` z PASS/FAIL, aktualnym stanem oraz blokadami.
- Nie deklarować sukcesu 1+15 na podstawie 1+1, stubów lub kompilacji CI. Jeśli nowy engine jeszcze nie istnieje, zacząć od testowego P0 i stopniowej implementacji.
- Brak ciągłych pytań o priorytety: kolejność P0 -> 1+4 -> 1+8 -> 1+15 -> multiplayer/publiczny release. Pytać wyłącznie o realne blokady wymagające decyzji użytkownika.

**Priorytet użytkownika: samodzielnie gra w przeglądarce przeciwko piętnastu widocznym, rzeczywiście walczącym botom na poziomie NORMAL, możliwie z feelingiem klasycznego CoD2.**
