# Технічне завдання: budyk — легковагий моніторинг для FreeBSD з адаптивним збором

> **Назва проєкту:** budyk (українська "будик" — будильник)
> **Ліцензія:** BSD-3-Clause
> **Цільова платформа:** FreeBSD 13.x / 14.x / 15.x (amd64, arm64); бажано — сумісність з NetBSD/OpenBSD/Linux
> **Мова:** C++ (C++17 мінімум, C++20 бажано) + C для платформних колекторів
> **Система збірки:** CMake 3.22+ (або Meson, рішення на M0)
> **Форм-фактор:** єдиний репозиторій з модульною збіркою + статично злінкований CLI-бінарник
> **Референсні системи:** kula, Netdata, btop, Glances, sysstat/sar, atop, RRDtool

---

## 0. Позиціонування

### 0.1. Що розробляємо
Оригінальна розробка (BSD-3-Clause) — self-contained моніторинг-демон для FreeBSD-серверів. Один бінарник, без зовнішніх залежностей, без БД. Ключова відмінність від існуючих — **адаптивна модель збору** (§3.4): система справді спить, коли не потрібна, і миттєво прокидається, коли потрібна.

### 0.2. Чим не є
Не є портом, форком або похідною роботою (derivative work) будь-якого конкретного проєкту. Ідеї та підходи запозичуються з кількох систем (див. Додаток A), реалізація — власна з нуля.

### 0.3. Референсні системи для вивчення
Кожна вирішує частину задачі, жодна — всю:

| Система | Що беремо | Чого не беремо |
|---------|----------|----------------|
| **kula** (Go, AGPL-3.0) | Архітектура ring-buffer tier'ів, вбудований SPA, single-binary design, структура `Sample` як контракт між collector і storage | Always-on 1 Hz (марнотратно), JSON codec (повільно), прив'язку до `/proc` |
| **Netdata** (C, GPL-3.0) | Tier-архітектура зберігання, ML anomaly detection (концепція), parent-child streaming | Важкий footprint (~200 MB RSS), cloud-залежність, always-on |
| **btop / htop** (C++/C) | On-demand модель (збір лише поки запущений), мінімальний footprint | Немає історії, немає web UI, немає демона |
| **Glances** (Python) | REST API + WebSocket потік, крос-платформність, psutil-абстракція | Python overhead, немає on-disk persistence |
| **sysstat/sar** (C) | Бінарний формат зберігання метрик, cron-based heartbeat, post-mortem replay | Фіксований інтервал, немає web UI, немає real-time |
| **atop** (C) | Post-mortem replay бінарних логів, повний snapshot системи | Важкі snapshots, фіксований інтервал |
| **RRDtool** (C) | Родоначальник ring-buffer tier'ів, передбачуване використання диску, consolidation functions | Застарілий API, окремий демон |

---

## 1. Фаза 1 — Вивчення референсних систем

### 1.1. Що витягти з кожної референсної системи
Для kula (як найближчої за scope) — глибокий розбір по модулях. Для решти — точкове вивчення конкретних підсистем.

**З kula** (`github.com/c0m4r/kula`, Go, AGPL-3.0):

- **`internal/collector/`**
  - Перелік усіх шляхів `/proc/*` та `/sys/*`, що читаються (grep по `os.Open`, `os.ReadFile`).
  - Формат кожного вхідного файлу (колонки, одиниці, дельти vs абсолютні значення).
  - Алгоритм агрегації: як обчислюються per-core delta, як рахується `%util` для дисків, як агрегуються TCP-каунтери.
  - Частота семплювання, jitter-компенсація, обробка пропусків.
  - Визначення структури `Sample` (`collector/types.go`) — контракт, через який collectors спілкуються зі storage.

- **`internal/storage/`**
  - Точний on-disk формат ring-buffer файлу: заголовок, запис, хвіст, endianness.
  - Кодек (`codec.go`) — JSON чи бінарний? Якщо JSON — це вузьке місце по CPU на 1 Hz, у нашому проєкті дивитися в бік `msgpack` або власного бінарного формату.
  - Логіка ротації: що пишеться в заголовок, як відстежується write-cursor, як обробляється crash-recovery.
  - Алгоритм агрегації Tier 1 → Tier 2 → Tier 3: які поля усереднюються, які беруться last-value, які max/min.
  - Розміри за замовчуванням: 250 / 150 / 50 MB → оцінити, скільки це в записах і на скільки годин/днів історії вистачає.

- **`internal/web/`**
  - Повний реєстр REST-ендпоінтів (метод, шлях, query/body, формат відповіді).
  - Протокол WebSocket: формат кадрів, heartbeat, backpressure, broadcast-hub.
  - Auth-flow: видача cookie, sliding expiration, persist-at-rest для сесій, Argon2id-параметри (m, t, p).
  - Вбудовування SPA: `//go:embed static/*` → в C/C++ еквівалент через `xxd -i` або `incbin`.

- **`internal/tui/`**
  - Які екрани, переключення, які метрики виведені.
  - Частота оновлення TUI і як підписується на collector.

- **`cmd/kula/main.go`**
  - Субкоманди: `serve`, `tui`, `hash-password`, ... — зібрати повний список і кожну описати як CLI-контракт.

- **`config.example.yaml`**
  - Повна схема конфігу з типами, дефолтами, env-override-правилами (`KULA_*`).

**З Netdata** (точково):
- Tier-архітектура `dbengine` — як організовано зберігання в 3 tier'и, як працює aggregation, які consolidation functions.
- ML anomaly detection — які алгоритми (k-means на edge), які features, overhead.

**З sysstat/sar** (точково):
- Бінарний формат `sa*` файлів — як кодуються метрики, заголовки, endianness.
- Які саме sysctl/procfs значення збирає `sadc` і як агрегує.

**З atop** (точково):
- Формат binary log, replay-механізм, як реалізований «time travel» по снапшотах.

### 1.2. Deliverable фази 1
`docs/reference-study/` — набір markdown-файлів, по одному на систему, плюс:
- `data-contract.md` — структура Sample та її еволюція через тири
- `storage-format.md` — байтовий формат ring-buffer
- `http-api.md` — OpenAPI-специфікація (YAML)
- `ws-protocol.md` — опис кадрів
- `config-schema.md` — JSON Schema для конфігу

---

## 2. Фаза 2 — Маппінг джерел даних Linux → FreeBSD

Це критична фаза. На FreeBSD **немає** `/proc` за замовчуванням і **ніколи не буде** такого ж `/sys`. Потрібна абстракція — абстрактний клас `MetricSource` (C++) або vtable-інтерфейс (C), під яким ховаються Linux- та FreeBSD-реалізації.

### 2.1. Таблиця відповідностей (мінімальна)

| Метрика | Linux (джерело) | FreeBSD (джерело) | C/C++ API |
|---------|-----------------|-------------------|-----------|
| CPU per-core (user/sys/idle/iowait/irq/steal) | `/proc/stat` | `sysctl kern.cp_times` (масив `long[CPUSTATES * ncpu]`) | `sysctlbyname(3)` |
| Load average | `/proc/loadavg` | `sysctl vm.loadavg` або `getloadavg(3)` | `getloadavg(3)` — POSIX |
| Memory | `/proc/meminfo` | `sysctl vm.stats.vm.*`, `hw.physmem`, `hw.pagesize` | `sysctlbyname(3)` |
| Swap | `/proc/swaps` + `/proc/meminfo` | `kvm_getswapinfo(3)` або `sysctl vm.swap_info` | `<kvm.h>` + `-lkvm` |
| Uptime | `/proc/uptime` | `sysctl kern.boottime` + `CLOCK_MONOTONIC` | `clock_gettime(2)` — POSIX |
| Hostname | `/proc/sys/kernel/hostname` | `gethostname(3)` | `gethostname(3)` — POSIX |
| Entropy | `/proc/sys/kernel/random/entropy_avail` | На FreeBSD пул не експонується так — або пропустити, або через `kern.random.*` (інша семантика) | `sysctlbyname(3)` |
| Clock sync | `/proc/sys/kernel/...`, NTP-check | `ntp_gettime(2)` / `ntp_adjtime(2)` (`struct ntptimeval`) | `<sys/timex.h>` |
| Network per-interface | `/proc/net/dev` | `getifaddrs(3)` + `struct if_data` (через `AF_LINK` адресу) | `getifaddrs(3)`, `<net/if.h>` |
| TCP/UDP/ICMP counters | `/proc/net/snmp`, `netstat -s` | `sysctl net.inet.{tcp,udp,ip,icmp}.stats` (повертають `struct *stat`) | `sysctlbyname(3)` + `<netinet/tcp_var.h>` тощо |
| Сокети (established/...) | `/proc/net/tcp[6]` | `sysctl net.inet.tcp.pcblist` + `xtcpcb` | `sysctlbyname(3)` + `<netinet/tcp_var.h>` |
| Disk I/O per-device | `/proc/diskstats` | `devstat(3)` API (`devstat_getdevs`, `devstat_compute_statistics`) | `<devstat.h>` + `-ldevstat` |
| Filesystem usage | `/proc/mounts` + `statfs` | `getmntinfo(3)` + `struct statfs` | `getmntinfo(3)` — BSD |
| Inode usage | `statfs.f_files/f_ffree` | `statfs.f_files/f_ffree` (збігається) | ditto |
| Processes by state | `/proc/*/stat` сканування | `sysctl kern.proc.all` → масив `struct kinfo_proc`, поле `ki_stat` | `sysctlbyname(3)` + `<sys/user.h>` |
| Threads total | `/proc/*/status` | `kinfo_proc.ki_numthreads` | ditto |
| Self metrics (CPU%, RSS) | `/proc/self/stat`, `/proc/self/status` | `kinfo_getproc(getpid())` з `libutil` | `<libutil.h>` + `-lutil` |
| Temperature/thermal | `/sys/class/thermal/*` | `sysctl dev.cpu.N.temperature`, `hw.acpi.thermal.*` | `sysctlbyname(3)` |
| GPU NVIDIA | nvidia-smi/NVML | NVML на FreeBSD через драйвер — FreeBSD реалізація — заглушка або через `nvidia-smi` exec | NVML SDK headers / `popen("nvidia-smi ...")` |

### 2.2. Deliverable фази 2
`docs/platform-mapping.md` + прототип модуля `collector_freebsd.c` із заглушками всіх функцій, що повертають `ENOSYS`. Це дозволить паралельно розвивати вищі шари.

---

## 3. Фаза 3 — Архітектура (C/C++)

### 3.1. Структура проєкту

```
budyk/
├── CMakeLists.txt                  # top-level, підключає всі модулі
├── cmake/
│   ├── platform.cmake              # detect OS, set platform-specific flags
│   └── embed_resources.cmake       # вбудовування SPA в бінарник (xxd / incbin)
├── src/
│   ├── core/                       # Sample, інтерфейси, кодек (C++17)
│   │   ├── sample.h                # struct Sample { CpuStats, MemStats, ... }
│   │   ├── metric_source.h         # abstract class MetricSource
│   │   ├── collection_level.h      # enum class Level { L1, L2, L3 }
│   │   └── codec.h / codec.cpp     # бінарний кодек (msgpack або custom)
│   ├── collector/
│   │   ├── collector.h             # спільний інтерфейс
│   │   ├── linux/                  # реалізація через /proc, /sys (C)
│   │   │   ├── cpu.c
│   │   │   ├── memory.c
│   │   │   ├── network.c
│   │   │   ├── disk.c
│   │   │   └── system.c
│   │   └── freebsd/                # реалізація через sysctl, devstat, kvm (C)
│   │       ├── cpu.c
│   │       ├── memory.c
│   │       ├── network.c
│   │       ├── disk.c
│   │       └── system.c
│   ├── scheduler/                  # tick scheduler: L1↔L2↔L3, anomaly-detector (C++)
│   ├── hot_buffer/                 # lock-free circular buffer для WS catch-up (C++)
│   ├── storage/                    # tiered ring-buffer, mmap, level-маркери (C++)
│   │   ├── ring_file.h / .cpp      # single ring-buffer file
│   │   ├── tier_manager.h / .cpp   # multi-tier coordinator + aggregation
│   │   └── codec.h / .cpp          # on-disk binary format
│   ├── web/                        # HTTP/WebSocket server (C++)
│   │   ├── server.h / .cpp         # routes, API handlers
│   │   ├── ws_hub.h / .cpp         # broadcast hub, client management
│   │   ├── auth.h / .cpp           # Argon2id + sessions
│   │   └── static/                 # вбудований SPA (html/js/css)
│   ├── tui/                        # terminal UI (C++)
│   ├── rules/                      # Lua-based rule engine (C++ + Lua C API)
│   │   ├── lua_engine.h / .cpp     # Lua VM lifecycle, sandbox setup, per-tick eval
│   │   ├── lua_bindings.h / .cpp   # expose Sample fields як read-only Lua таблиці
│   │   ├── lua_stdlib.h / .cpp     # watch(), alert(), exec(), escalate() builtins
│   │   └── yaml_compat.h / .cpp    # опціональний YAML-to-Lua транспілер для простих правил
│   ├── ai/                         # AI-асистована генерація правил (C++)
│   │   ├── baseline.h / .cpp       # статистичний baseline з історії
│   │   ├── suggest.h / .cpp        # engine пропозицій правил (локальні евристики)
│   │   └── llm_client.h / .cpp     # опціонально: LLM API клієнт для розширених пропозицій
│   ├── config/                     # YAML config loader (C++)
│   └── main.cpp                    # CLI entry point (serve, tui, hash-password, suggest-rules)
├── third_party/                    # vendored або submodule залежності
├── rules/                          # бібліотека правил за замовчуванням
│   ├── examples.lua                # приклади правил з коментарями
│   └── freebsd-defaults.lua        # FreeBSD-специфічні розумні дефолти
├── tests/                          # unit + integration tests
├── addons/
│   ├── freebsd/                    # rc.d script, pkg-plist, Makefile для ports
│   ├── linux/                      # systemd unit, deb rules
│   └── docker/
├── docs/
│   └── budyk.8                     # man page
└── config.example.yaml
```

### 3.2. Ключові залежності

| Підсистема | Бібліотека | Обґрунтування |
|------------|-----------|---------------|
| Event loop | **libuv** (або raw `kqueue`/`epoll`) | Крос-платформний async I/O, активно використовується на FreeBSD. Альтернатива: `libevent`. |
| HTTP + WebSocket | **mongoose** (embedded, MIT) або **libwebsockets** | mongoose — single-header, ідеально для embedded-сервера. libwebsockets — важче, але потужніше. |
| YAML config | **libyaml** (C) або **yaml-cpp** (C++) | libyaml — мінімалістичніше, yaml-cpp — зручніше з C++ |
| JSON (API responses) | **cJSON** (C, MIT) або **nlohmann/json** (C++, MIT) | cJSON — мінімалізм; nlohmann — зручність |
| Binary codec (storage) | **msgpack-c** або **custom** | msgpack — стандартизований, швидкий. Custom — якщо потрібен повний контроль розміру запису |
| Argon2 | **libargon2** (reference impl, CC0/Apache-2.0) | де-факто стандарт |
| Rule engine | **Lua 5.4** (MIT) | ~200 KB, вбудовуваний, FreeBSD використовує Lua в bootloader з версії 12.0. Замінює кастомний DSL-парсер |
| TUI | **ncurses** (BSD base) або **notcurses** (modern) | ncurses є в base FreeBSD. notcurses — гарніше, але залежність |
| CLI parsing | **getopt_long** (base) або **argtable3** (vendored, BSD) | getopt_long — zero-dependency, в base |
| Logging | **syslog(3)** + custom structured logger | В base FreeBSD, zero-dependency |
| Hashing (CRC32C) | **SSE4.2 intrinsics** або `<zlib.h>` crc32 | Для ring-buffer записів |
| SPA embedding | **xxd** або **incbin** (compile-time embed) | Еквівалент `go:embed` |
| Тестування | **cmocka** (C, Apache-2.0) або **Google Test** (C++, BSD) | cmocka — для C-модулів колекторів; gtest — для C++ |

**Принцип: мінімум залежностей.** FreeBSD base system вже містить: libc, libm, libkvm, libdevstat, libutil, ncurses, zlib, libcrypto. Ідеальний варіант — залежати лише від base + mongoose (vendored) + libyaml (ports) + libargon2 (ports).

### 3.3. Ключові архітектурні інваріанти

1. **Separation of concerns**: `core/` — чисті структури та інтерфейси, без I/O, без алокацій з heap де можливо. Колектори — plain C, компілюються як `.o` файли, лінкуються умовно через `#ifdef __FreeBSD__` / `#ifdef __linux__`.
2. **`class MetricSource`** (абстрактний) — єдина точка розширення під ОС. Compile-time вибір через `cmake -DPLATFORM=freebsd`.
3. **Storage не знає про колектор** — приймає `struct Sample*` і пише байти. Формат зберігання — власний, бінарний, little-endian, версійований.
4. **WS-hub — один fd-backed broadcast**: `pipe(2)` або custom lock-free SPMC ring. Slow-consumer disconnect.
5. **Конфіг незмінний після старту** (за винятком перечитування по `SIGHUP` — опціонально, v2).
6. **Весь I/O — неблокуючий** через event loop (libuv/kqueue). Важкі `sysctl`/`devstat` виклики — в окремому thread, результат передається через pipe в event loop.
7. **Без винятків C++**. Компіляція з `-fno-exceptions -fno-rtti` для мінімального footprint. Помилки — через return codes / `std::expected` (C++23) або власна `Result<T,E>`.
8. **Static linking за замовчуванням** (`-static` або `-static-libstdc++`) — один бінарник без runtime залежностей.

### 3.4. Адаптивна модель збору даних

Більшість моніторингів (kula, Netdata, Prometheus node_exporter, Telegraf, collectd) використовують модель **always-on**: збір щосекунди незалежно від того, чи хтось дивиться на дашборд. Це марнотратно — за даними Netdata issue #238 та аналогічних дискусій, моніторинг-демон спалює 2–5% CPU 99.99% часу даремно.

Наш проєкт реалізує **3-рівневу адаптивну модель + окремий hot buffer**.

#### 3.4.1. Три рівні колектора (вкладена матрьошка)

```
                  ┌─────────────────────────────────────────────┐
                  │  L3 — ACTIVE (1 Hz, повний набір метрик)    │
                  │  Тригер: WS-клієнт підключений АБО          │
                  │          ескалація з L2 (аномалія > N хв)    │
                  │  ┌─────────────────────────────────────────┐ │
                  │  │  L2 — WATCHFUL (15–60s, розширений)     │ │
                  │  │  Тригер: L1 виявив аномалію АБО          │ │
                  │  │          конфіг: l2_always_on: true       │ │
                  │  │  ┌─────────────────────────────────────┐ │ │
                  │  │  │  L1 — HEARTBEAT (5 хв, мінімум)     │ │ │
                  │  │  │  Завжди працює. Load, CPU%,          │ │ │
                  │  │  │  mem total, swap, uptime.            │ │ │
                  │  │  │  ~1 sysctl виклик, ~200 B на диск.   │ │ │
                  │  │  └─────────────────────────────────────┘ │ │
                  │  └─────────────────────────────────────────┘ │
                  └─────────────────────────────────────────────┘
```

**L1 — Heartbeat (завжди активний).** Один тік кожні 5 хвилин. Мінімальний набір: load average, сумарний CPU%, загальна пам'ять, swap, uptime. Один `sysctl`-виклик, ~200 байт запису. За добу — 288 записів × 200 B ≈ 56 KB (проти ~165 MB/добу у always-on моніторингу в Tier 1). Мета: знати, що машина жива; груба картина на горизонті тижнів.

**L2 — Watchful (за умовою).** Тік кожні 15–60 секунд (конфігурується). Розширений набір: per-core CPU, per-interface net, per-device disk I/O, TCP-каунтери, process counts. Активація двома шляхами: (a) L1 виявив аномалію (load/CPU/swap вище порогу), (b) конфігом `l2_always_on: true`. Деескалація — з гістерезисом 5 хвилин після нормалізації.

**L3 — Active (клієнт або алерт).** 1 Hz, повний набір включно з thermal, entropy, clock sync, self-metrics. Активація: підключення WS-клієнта або TUI, або ескалація з L2 (аномалія довше N хвилин). Деескалація — grace period 30–60 секунд після від'єднання останнього клієнта.

**Вкладеність:** коли активний L3, L2 і L1 не запускають окремі збори — L3-тік містить усі дані. Агрегатор періодично згортає L3-семпли в записи для L2-ring'у (кожні 15–60s) і L1-ring'у (кожні 5 хв).

**Scheduler:** центральна точка — tick scheduler, керований трьома входами:
1. `atomic_int` — лічильник активних WS/TUI-підписників
2. `enum anomaly_state { NORMAL, ELEVATED, CRITICAL }` — результат anomaly-check на останньому L1-тіку
3. Поточний рівень

Ескалація — миттєва (наступний тік). Деескалація — з гістерезисом (захист від осциляції при нестабільному WS-з'єднанні або F5 у браузері).

#### 3.4.2. Hot buffer (окрема сутність, лише RAM)

Hot buffer **не є частиною tier-піраміди зберігання**. Це in-memory circular buffer фіксованого розміру (300 записів = 5 хвилин при 1 Hz, ~600 KB RAM), який існує виключно для обслуговування WebSocket-потоку.

Життєвий цикл:
- При підключенні WS-клієнта: клієнт отримує dump поточного вмісту hot buffer (catch-up — графіки не порожні), потім підписується на broadcast.
- Hot buffer **ніколи не пише на диск**. Дані ефемерні.
- Hot buffer і Tier 1 ring-файл живляться від **одного тіку колектора** — один collect → два споживачі (storage + hot buffer).
- Grace period: після від'єднання останнього клієнта hot buffer залишається «теплим» 60 секунд (наступний клієнт миттєво побачить 5 хвилин графіків). Після grace — скидання.

#### 3.4.3. Наслідки для storage

Щільність даних у ring-файлах **неоднорідна в часі**: періоди з 1-секундною роздільністю чергуються з проміжками по 5 хвилин. Це принципова відмінність від always-on моніторингів (стала щільність).

Наслідки:
1. Кожен запис містить **абсолютну мітку часу** (не дельту).
2. Кожен запис містить **мітку рівня** (L1/L2/L3) — фронтенд знає, як інтерполювати і де малювати «low-resolution zone».
3. Агрегація Tier 1 → Tier 2 працює з нерегулярними вибірками: «всі записи за хвилинне вікно, скільки б їх не було».
4. API `/api/history` повертає дані з маркерами зон різної роздільності.

#### 3.4.4. Конфігурація

```yaml
collection:
  mode: adaptive          # always | adaptive | on-demand
  l1:
    interval: 300s        # heartbeat
    metrics: minimal      # load, cpu_total, mem_total, swap, uptime
  l2:
    interval: 30s
    always_on: false       # true = L2 працює завжди
    escalation_thresholds:
      load_1m: 4.0
      cpu_percent: 85
      swap_used_percent: 50
    hysteresis: 300s       # 5 хв до деескалації
  l3:
    interval: 1s
    metrics: full
    grace_period: 60s      # після від'єднання останнього клієнта
  hot_buffer:
    capacity: 300          # записів (= 5 хв при 1 Hz)
    warm_grace: 60s        # скільки тримати після від'єднання клієнтів
```

У режимі `always` — поведінка як у kula/Netdata (L3 завжди). У режимі `on-demand` — L1 вимкнений, L2 вимкнений, збір лише при підключенні клієнта (hot buffer only, ring-buffer не використовується).

#### 3.4.5. Ресурсне порівняння

| Сценарій | always-on (kula/Netdata) | Наш проєкт (adaptive) |
|----------|--------------------------|------------------------|
| Idle, 0 клієнтів, 24 год | 86 400 записів, ~165 MB disk, CPU ~2% | 288 записів (L1), ~56 KB disk, CPU ≈ 0% |
| 1 клієнт, 1 година | 3 600 записів, ~7 MB | 3 600 записів (L3) + 288 (L1), ~7 MB |
| Idle, 0 клієнтів, 30 днів | ~4.8 GB Tier 1 (кільце перезаписане) | ~1.6 MB (L1) |

### 3.5. Формат зберігання (пропозиція)

```
[ File Header 64 B ]
  magic:       "KULA\0RB\x02"        // 8 B (v2 — з підтримкою level-маркерів)
  version:     uint32_t              // 4 B
  tier:        uint8_t (1|2|3)       // 1 B
  _pad:        uint8_t[3]
  record_size: uint32_t              // 4 B — фікс. розмір запису
  capacity:    uint64_t              // 8 B — кількість слотів
  write_idx:   uint64_t              // 8 B — атомарно оновлюється
  _reserved:   uint8_t[28]

[ Record 0 ] [ Record 1 ] ... [ Record N-1 ]

Запис:
  timestamp_unix_nanos: uint64_t     // абсолютний (не дельта!)
  level:               uint8_t      // 1=L1, 2=L2, 3=L3
  _pad:                uint8_t[1]
  crc32c:              uint32_t
  payload:             uint8_t[record_size - 14]  // msgpack-serialized Sample
```

Запис — `pwrite(2)` за розрахованим offset; `write_idx` оновлюється через `__atomic_fetch_add` у `mmap`-регіоні. При краші втрачається не більше одного запису.

### 3.6. Rule Engine (на базі Lua)

Rule engine вбудовує Lua 5.4 для оцінки користувацьких правил моніторингу по кожному зібраному `Sample`. Замість винаходу кастомного DSL з власним парсером — використовуємо справжню мову програмування, яку FreeBSD вже поставляє у своєму bootloader'і з версії 12.0.

#### 3.6.1. Чому Lua, а не кастомний DSL

Кастомна YAML-based мова умов (`condition: "> 90"`, `all:`, `any:`, wildcard `*`) вимагає побудови парсера, AST-evaluator'а та логіки комбінаторів — приблизно 2 000 рядків коду, що дублюють те, що будь-яка скриптова мова вже надає. Lua додає ~200 KB до бінарника, вбудовується через `#include <lua.h>` з ~200 рядками glue-коду і дає користувачам повну виразність: `and`, `or`, цикли, функції, математика, рядкові операції.

Прості правила залишаються такими ж читабельними, як monit. Складні правила (обчислювані пороги, ітерація по динамічних наборах пристроїв, крос-метричні кореляції) — це просто Lua, а не обмежений DSL, що потребує розширень.

#### 3.6.2. Синтаксис правил

```lua
-- rules.lua — простий приклад, зрозумілий без знання Lua

watch("high_cpu", {
  when = function() return cpu.total_percent > 90 end,
  for_ticks = 5,
  severity = "warning",
  action = alert
})

watch("memory_critical", {
  when = function() return mem.available_percent < 5 end,
  for_ticks = 3,
  severity = "critical",
  action = { alert, exec("/usr/local/bin/drop-caches.sh") }
})

-- Обчислюваний поріг — неможливо в простому DSL
watch("swap_under_load", {
  when = function()
    return swap.used_percent > 80 and load.avg_1m > cpu.count * 2
  end,
  for_ticks = 3,
  action = { alert, escalate }  -- примусовий збір на L3
})

-- Ітерація по інтерфейсах — просто цикл, без wildcard DSL
for name, iface in pairs(net.interfaces) do
  watch("errors_" .. name, {
    when = function() return iface.rx_errors_per_sec > 100 end,
    for_ticks = 5,
    action = alert
  })
end
```

Опціонально, тонкий шар YAML-сумісності транспілює прості YAML-правила у Lua `watch()` виклики при завантаженні — для користувачів, які вважають за краще YAML для базових випадків. Внутрішньо engine завжди Lua.

#### 3.6.3. Sandboxing

Lua-правила запускаються в обмеженому sandbox'і:
- Немає `io`, `os`, `loadfile`, `dofile`, `require` — лише чиста логіка.
- `exec()` — це C-функція під нашим контролем (timeout, `setrlimit`, прапорець `--enable-exec`).
- Поля `Sample` expose'яться як read-only Lua-таблиці (`cpu`, `mem`, `net`, `disk`, `load`, `swap`).
- Час виконання обмежений через `lua_sethook` — максимум 10ms на тік, скрипт вбивається при перевищенні.
- Без доступу до файлової системи, без мережевого доступу, без FFI.

#### 3.6.4. Конвеєр оцінки (Evaluation Pipeline)

На кожному тіку колектора:

1. **Load** — файли правил парсяться один раз при старті (або при `SIGHUP`). Кожен `watch()` виклик реєструє правило в C-side масиві. Без парсингу на кожному тіку.
2. **Bind** — поля поточної `Sample` структури пушаться в Lua глобальні таблиці (`cpu.total_percent`, `mem.available_percent` тощо) як read-only значення.
3. **Eval** — для кожного зареєстрованого правила викликається його `when()` функція. Булевий результат.
4. **Sustain** — інкремент або скидання per-rule лічильника (C-side, не в Lua). Лічильник досягає `for_ticks` → спрацювання.
5. **Act** — `alert`: лог + WS-подія. `exec()`: fork+exec, неблокуючий, з таймаутом. `escalate`: сигнал до scheduler'а. `log`: структурований запис у лог.
6. **Cooldown** — після спрацювання правило входить у cooldown (конфігурується, за замовчуванням = `for_ticks × interval`).

Бюджет продуктивності: 100 `watch()` правил оцінюються за тік за ≤ 1ms. Lua function calls з числовими порівняннями — тривіально швидкі.

#### 3.6.5. Правила vs рівень збору

Не всі метрики доступні на всіх рівнях. L1 збирає лише `load`, `cpu_total`, `mem_total`, `swap`, `uptime`. Якщо `when()` правила звертається до `disk.sda.util_percent`, це поле буде `nil` на L1.

Engine обробляє це: якщо `when()` повертає `nil` (бо звернулася до nil-поля, і Lua-порівняння з nil дає nil/false), правило пропускається на цей тік, і sustain-лічильник не скидається. Лічильник зберігається при переходах між рівнями: L3 (4/5 хітів) → L1 (пропуск, залишається 4) → L3 (продовжує з 4).

### 3.7. AI-асистована генерація правил

Rule engine корисний лише якщо в ньому гарні правила. Написання правил вимагає розуміння нормальних baseline'ів і типових патернів збоїв — саме те, в чому AI сильний. Ця секція описує AI-шар, який **пропонує** правила; він ніколи не застосовує їх автономно.

#### 3.7.1. Два рівні AI-допомоги

**Tier A — Локальні евристики (без зовнішнього API, завжди доступний).**

Вбудований статистичний аналіз зібраної історії. Запускається як CLI-команда:

```
$ budyk suggest-rules --window 7d --output suggested-rules.lua
```

Як працює:
1. Зчитує Tier 2/Tier 3 ring-буфери за запитане вікно.
2. Для кожної метрики обчислює: mean, stddev, p95, p99, min, max за вікно.
3. Застосовує евристичні шаблони. Приклади: якщо `cpu.total_percent` p99 = 45% → пропонує поріг 85% (≈ 2× запас). Якщо `memory.available_percent` коли-небудь падала нижче 10% → пропонує правило на 8% з severity critical. Якщо `disk.*.util_percent` показує стійкі епізоди >90% → пропонує з тривалістю, що спостерігалася, як `for_ticks`.
4. Виводить `suggested-rules.lua` файл з `watch()` викликами та коментарями, що пояснюють обґрунтування.

Це **не** машинне навчання. Це арифметика + шаблони. Працює локально, не потребує мережевого доступу, завершується за секунди. Завжди доступний навіть на ізольованих (air-gapped) системах.

**Tier B — LLM-асистований (опціональний, потребує доступу до API).**

Для користувачів, які хочуть розумніших пропозицій: відправка анонімізованих метричних зведень (не сирих даних) до LLM API і отримання пропозицій правил з поясненнями природною мовою.

Як працює:
1. Користувач запускає: `$ budyk suggest-rules --ai --api-key $KEY`
2. Інструмент обчислює те ж статистичне зведення, що й Tier A.
3. Формує промпт: «Ось 7-денні зведення метрик FreeBSD-сервера. Запропонуйте правила моніторингу як Lua `watch()` виклики. Для кожного правила поясніть чому.»
4. Зведення анонімізоване: без хостнеймів, без IP, без імен користувачів. Лише числові розподіли.
5. LLM відповідає Lua-правилами + поясненнями. Lua — широко відомий формат, LLM генерують його значно надійніше, ніж кастомний DSL.
6. Інструмент валідує відповідь: чи парситься Lua? чи посилається на валідні поля? чи пороги в межах правдоподібного діапазону?
7. Вивід у `suggested-rules.lua` з маркерами `-- AI-suggested`.

**Користувач завжди перевіряє та затверджує.** Інструмент ніколи не пише напряму в `config.yaml`. Робочий процес завжди: `suggest → review → copy to config → restart/reload`.

#### 3.7.2. AI у дашборді (майбутнє, v1.2+)

Майбутня версія може інтегрувати AI-пропозиції безпосередньо у SPA-дашборд: кнопка «Suggest Rules», що запускає Tier A локально (через API-запит до демона) або Tier B (через браузер → LLM API, з ключем користувача). Результати з'являються як diff-style preview: «Додати це правило? [Прийняти] [Змінити] [Відхилити].» Це явно поза scope v1.0, але архітектура не повинна цьому перешкоджати.

#### 3.7.3. Архітектурні обмеження

1. **AI ніколи не в hot path.** Оцінка правил (§3.6) — чиста арифметика, без AI. AI запускається лише коли користувач явно просить пропозиції.
2. **Без постійного AI.** Демон не телефонує додому, не відправляє телеметрію, не запускає фонові ML-моделі. Tier A — локальна арифметика. Tier B — ініційований користувачем.
3. **Tier B опціональний при компіляції.** `cmake -DENABLE_LLM=OFF` видаляє LLM-клієнт повністю. Жодного мережевого коду для LLM у бінарнику. Ізольовані системи отримують лише Tier A.
4. **Жодних API-ключів у конфігу.** Ключі передаються через змінну оточення або CLI-прапорець, ніколи не зберігаються на диск інструментом.

---

## 4. Фаза 4 — План реалізації

Milestones (кожний = окрема PR-серія, окремий реліз `0.x`):

| M | Scope | Definition of Done |
|---|-------|--------------------|
| M0 | Скелет проєкту, CMake, CI (GitHub Actions + Cirrus CI для FreeBSD 13/14/15), clang-tidy + clang-format + cppcheck | `cmake --build .` зелений на Linux і FreeBSD |
| M1 | `core/` — struct Sample + msgpack-кодек + unit-тести roundtrip + `enum Level` | 100% покриття по Sample serialization |
| M2 | `collector/linux/` MVP (CPU+mem+net+disk+load) | збігається з `top`, `vmstat`, `/proc` даними на тому ж хості (golden tests) |
| M3 | `collector/freebsd/` MVP (CPU+mem+net+disk+load через sysctl/devstat/getifaddrs) | порівняння з `top`, `netstat -ib`, `iostat` в межах ±1% |
| M4 | `storage/` — ring-buffer з level-маркерами + агрегація тирів (нерегулярні вибірки) + crash-recovery | fsck-тул і recovery без втрати > 1 запису |
| M4.1 | **Tick scheduler** — L1/L2/L3 адаптивна логіка + anomaly-detector (пороговий) + hysteresis | Unit-тести: ескалація/деескалація за сценаріями; integration-тест: підключив WS → L3 за < 2s |
| M4.2 | **Hot buffer** — lock-free in-memory ring, catch-up dump при підключенні клієнта, grace period | WS-клієнт отримує 300 записів catch-up при connect; hot buffer скидається через 60s після disconnect |
| M5 | `web/` — REST-ендпоінти з маркерами зон роздільності | `/api/history` повертає записи з полем `level` та зонами `L1/L2/L3` |
| M6 | WS-hub + вбудований SPA (з візуалізацією зон різної роздільності) | dashboard показує L1-зони сірим фоном, L3-зони — повні графіки |
| M7 | `auth/` Argon2id + sessions | auth-тести, rate-limit на логін |
| M8 | `tui/` на ncurses/notcurses | CPU, mem, net, disk, load, processes — як btop, але легший |
| M8.1 | **Lua rule engine** — вбудована Lua 5.4 VM, sandbox, `watch()`/`alert()`/`exec()`/`escalate()` builtins, per-tick eval, sustain-лічильники, cooldown | 20 прикладних правил проходять; sandbox блокує `io`/`os`; `exec` запускається з таймаутом; ескалація scheduler'а при спрацюванні правила |
| M8.2 | **AI-пропозиції правил (Tier A)** — локальний евристичний engine, CLI-команда `budyk suggest-rules` | За 7 днів синтетичної історії генерує валідні Lua `watch()` виклики, що парсяться та оцінюються без помилок |
| M9 | Packaging: FreeBSD port (`Makefile` для `/usr/ports/sysutils/budyk`), rc.d-скрипт, pkg-message; Linux — deb/rpm | `pkg install budyk` працює на FreeBSD 13/14/15 |
| M10 | Документація, man page, changelog, 1.0.0 | Реліз |

---

## 5. Критерії приймання

1. **Повний набір метрик** (таблиця в §2.1) — всі поля `struct Sample` заповнені на FreeBSD 13/14/15.
2. **Продуктивність у idle (adaptive, 0 клієнтів)**: CPU ≈ 0% (1 sysctl кожні 5 хв), disk I/O < 1 KB/хв. RSS ≤ 10 MB.
3. **Продуктивність при активному клієнті**: RSS ≤ 25 MB, CPU ≤ 1% на 4-core хості при L3 (1 Hz) і одному WS-клієнті.
4. **Ескалація L1→L3**: ≤ 2 секунди від підключення WS-клієнта до першого sample на 1 Hz.
5. **Hot buffer catch-up**: WS-клієнт при підключенні отримує ≤ 300 записів історії за ≤ 100ms.
6. **Footprint бінарника**: ≤ 5 MB stripped static на amd64.
7. **Стабільність**: 72-годинний soak-тест під nemesis'ом (kill -9 кожні N хвилин) — recovery без пошкодження ring-buffer. Valgrind/ASan clean.
8. **Cross-compilation**: `cmake --toolchain=freebsd-amd64.cmake` та `freebsd-aarch64.cmake` з Linux-хоста (через cross-clang або FreeBSD sysroot).
9. **Безпека**: cppcheck clean, clang-tidy clean, `-fsanitize=address,undefined` в CI, Argon2id-параметри не нижче рекомендацій OWASP 2024.
10. **Тести**: покриття `core/` та `storage/` — ≥ 85% по рядках (gcov/lcov); golden-тести на парсери; scenario-тести на tick scheduler (ескалація/деескалація).
11. **Платформи**: CI зелений на FreeBSD 13.4, 14.2, 15-CURRENT + Linux (Ubuntu 24.04) + Cirrus CI.
12. **Документація**: Doxygen без warnings, man page, README з quickstart на FreeBSD.
13. **Rule engine**: Lua 5.4 sandbox; 100 `watch()` правил оцінюються за тік за ≤ 1ms; `exec`-дії запускаються з конфігурованим таймаутом (за замовчуванням 30s); sustain-лічильники зберігаються при переходах між рівнями (L3→L1→L3); sandbox блокує `io`, `os`, `loadfile`.
14. **AI-пропозиції (Tier A)**: CLI-команда `budyk suggest-rules` завершується за 5s на 30 днях Tier 2 даних; вивід — валідний Lua, що парситься без помилок; запропоновані пороги в межах правдоподібного діапазону.

---

## 6. Ризики та mitigation

| Ризик | Ймовірність | Impact | Mitigation |
|-------|-------------|--------|------------|
| Референсні системи мутують, їх API/протоколи застарівають | Середня | Low | Наш проєкт не залежить від них — це лише джерела ідей |
| AGPL-зараження при випадковому копіюванні коду з kula/Netdata | Низька | Критичний | Code review на кожен PR, SPDX-headers у всіх файлах, `scancode-toolkit` у CI |
| `devstat`/`kinfo_proc` API змінюється між FreeBSD 13→14→15 | Середня | Medium | `#ifdef`-розгалуження по `__FreeBSD_version`, CI на 13/14/15-CURRENT |
| Memory safety (buffer overflow, use-after-free) у C/C++ | Висока | Критичний | `-fsanitize=address,undefined` у CI, Valgrind на кожен PR, `-D_FORTIFY_SOURCE=2`, `-fstack-protector-strong`. Колектори (C) — максимально прості, без динамічних алокацій |
| NVML на FreeBSD — обмежена підтримка | Висока | Low | `#ifdef HAVE_NVML`, вимкнений за замовчуванням |
| Залежність від mongoose/libyaml зникає або стає непідтримуваною | Низька | Medium | Обидві бібліотеки vendored у `third_party/`, можна форкнути |
| Проблеми з capsicum-sandboxing на FreeBSD при mmap+sysctl | Низька | Low | Відкласти sandbox до v1.1 |
| C++ ABI incompatibility між clang/gcc на різних FreeBSD | Низька | Medium | Static linking за замовчуванням, `-static-libstdc++` |
| `exec`-дія правила запускає довільні команди від користувача демона | Середня | High | Задокументувати security implications; рекомендувати запуск демона від непривілейованого користувача; `exec` вимкнений за замовчуванням, вмикається прапорцем `--enable-exec`; команди запускаються з таймаутом + resource limits (setrlimit) |
| LLM API надсилає метричні дані зовні (Tier B) | Низька | Medium | Надсилаються лише анонімізовані статистичні зведення (без сирих семплів, без хостнеймів); Tier B opt-in, вимкнений за замовчуванням, потребує явного прапорця `--ai` + API key |

---

## 7. Що не входить у scope (v1.0)

- Кластерна федерація (кілька нод → один дашборд)
- Експорт у Prometheus/OpenMetrics
- Доставка нотифікацій (email, Slack, webhook, PagerDuty) — в v1.0 правила генерують `alert` лише в лог + WS-подію; зовнішні інтеграції нотифікацій відкладені на v1.1
- AI-пропозиції правил Tier B (LLM-асистований) — архітектура підготовлена, реалізація відкладена на v1.1; Tier A (локальні евристики) входить у scope
- AI у дашборді (інтерактивний конструктор правил) — v1.2+
- Підтримка Jails/VNET-aware метрик (відкладено на v1.1 — FreeBSD-специфіка, окрема тема)
- Підтримка ZFS-specific метрик (`kstat.zfs.*` — багата окрема тема, v1.2)
- Windows/macOS

---

## 8. Наступні кроки

Після затвердження цього ТЗ:

1. Створити репозиторій `budyk` зі скелетоном CMake (M0), BSD-3-Clause у кожному файлі.
2. Призначити одну людину на фазу 1 (вивчення референсів) і другу на прототип колектора FreeBSD.
3. Запустити фазу 1 — очікуваний термін 1–2 тижні на одного інженера.
4. Паралельно почати збір sysctl-значень на реальному FreeBSD-хості (13.x, 14.x, 15-CURRENT) для валідації маппінгу §2.1.
5. Визначитися з HTTP-бібліотекою (mongoose vs libwebsockets) — написати PoC echo-server на обох, виміряти footprint і складність.

---

## Додаток A. Prior art: ПЗ з подібними моделями збору

Наша 3-рівнева адаптивна модель не є повністю новою. Нижче — реєстр існуючих систем, які реалізують елементи цього підходу. Жодна з них не реалізує всі три елементи (багаторівневий адаптивний збір + anomaly-triggered escalation + on-demand hot buffer) разом.

### A.1. Чистий on-demand (модель 3: збір лише за наявності клієнта)

| ПЗ | Мова | Як працює | Що взяти |
|----|------|-----------|----------|
| **htop / btop / top** | C / C++ | Класичні TUI-монітори: збирають і показують метрики **лише поки запущені**. Немає фонового демона, немає історії. Закрив — дані втрачені. | Підтвердження того, що on-demand модель працює і затребувана (btop — один з найбільш зіркових TUI на GitHub). |
| **Glances** (server mode `-w`) | Python / psutil | Web-сервер стартує, але колектори працюють завжди, поки процес живий. Без експорту в БД — дані in-memory, втрачаються при перезапуску. | REST API, WebSocket-потік, крос-платформність через `psutil`. |
| **Netdata issue #238** (2016, earthgecko) | — | Пропозиція «power saving mode»: запускати Netdata-збір лише при HTTP-запиті на порт. Реалізовано хаком через `nc → start netdata`. Upstream **відхилив** — Netdata принципово будується на always-on 1 Hz. | Валідація самої ідеї: якщо 99.99% часу ніхто не дивиться, always-on — марнотратство. |

### A.2. Фіксований tiered збір (без адаптивності)

| ПЗ | Мова | Як працює | Що взяти |
|----|------|-----------|----------|
| **Kula** | Go | 3 рівні зберігання (1s, 1m, 5m ring-buffers), але **збір завжди на 1 Hz**. Tier'и — це агрегація при зберіганні, не при зборі. | Ring-buffer design, JSON-кодек (його слабкість — замінюємо на msgpack/binary). |
| **Netdata** | C | 3 tier'и зберігання (1s, 1m/5m агрегати), збір завжди 1 Hz. Підтримка RAM-mode для child-нод (без disk I/O). ML anomaly detection на рівні Parent. | Tier-архітектура, parent-child streaming. ML для anomaly — але Netdata не знижує частоту збору у відповідь на аномалію, а навпаки алертить. |
| **sysstat / sar** | C | Cron-based: фіксований інтервал (10 хв за замовчуванням), бінарні файли `/var/log/sa/saDD`, ротація по днях. Жодної адаптивності. | Формат sa-файлів — приклад компактного бінарного зберігання метрик. |
| **atop** | C | Фіксований інтервал (10 хв за замовчуванням), бінарні снапшоти в `/var/log/atop/`. Post-mortem replay (`atop -r`). | Концепція post-mortem replay по бінарних логах. |
| **RRDtool** (Round Robin Database) | C | Класика tiered ring-buffer: кілька RRA (Round Robin Archives) з різним consolidation (AVG, MAX, MIN) і кроком. Фіксований розмір файлу. Використовується в Cacti, Munin, collectd. | Родоначальник ring-buffer tier'ів. Формат добре вивчений. |
| **Prometheus** | Go | Pull-модель з фіксованим scrape_interval (15s за замовчуванням). Recording rules для агрегації в грубіші інтервали. Downsampling через Thanos/Cortex. | Recording rules — аналог нашої агрегації L3→L2→L1. |

### A.3. Adaptive sampling (інтервал змінюється за умовою)

| ПЗ | Мова | Як працює | Що взяти |
|----|------|-----------|----------|
| **AdaM** (Університет Кіпру, IEEE TSC 2018) | Python/C | Фреймворк для IoT: динамічно змінює частоту збору та фільтрації на основі поточної мінливості метрики. Якщо метрика стабільна — інтервал зростає; якщо volatile — стискається. Результат: зниження обсягу даних на 74%, енергії на 71%, при точності >89%. | Найближчий академічний аналог нашої моделі. Алгоритм confidence-based estimation для вибору T. |
| **Azure Application Insights Adaptive Sampling** | .NET | Автоматичне регулювання відсотка семплювання запитів для підтримки бюджету по обсягу. Не про метрики хоста, а про APM-трейси. | Ідея «бюджет IOPS / бюджет записів» — переносна на наш storage. |
| **Datadog Adaptive Sampling** | — | Автопідстройка sampling rate для distributed traces на основі бюджету. До 800 service/env комбінацій. | Механізм per-service sampling rate — аналогія з per-metric level у нашому L1/L2/L3. |
| **FAST** (PID-контролер для adaptive sampling) | Python | Використовує PID-контролер для вибору оптимального інтервалу збору. Агресивний — великі інтервали в стабільні періоди. Точність нижча, ніж у AdaM, на волатильних сигналах. | PID-підхід як альтернатива пороговому anomaly-detector для v1.1+ |
| **Meng et al. (violation-likelihood)** | — | Для cloud-мереж: збільшує частоту моніторингу, коли значення метрики наближається до користувацького порогу. Інакше — фіксований rate. | Пряма аналогія нашої L1→L2 ескалації по порогах. |

### A.4. Anomaly-triggered escalation (збір прискорюється при виявленні проблеми)

| ПЗ | Мова | Як працює | Що взяти |
|----|------|-----------|----------|
| **Dynatrace Davis AI** | пропрієтарний | Auto-adaptive thresholds + seasonal baselines. При виявленні аномалії — автоматична прив'язка до topology, root-cause analysis. Не змінює частоту збору агента, але змінює інтенсивність обробки. | Концепція «investigation escalation» — при виявленні аномалії підключити більше ресурсів аналізу (у нас — більше метрик через L2→L3). |
| **Netdata ML** | C + Python | Anomaly detection на edge. ML-моделі навчаються на Parent-ноді, inference на Child. Аномалія → алерт, але **не** зміна частоти збору. | ML anomaly rate — можна використати як додатковий тригер L1→L2 у майбутніх версіях (v1.1+). |
| **Chowdhury et al. (policy-violation framework)** | — | Підлаштовує інтенсивність моніторингу при порушенні користувацьких політик. | Концепція policy-driven escalation — розширення нашої threshold-моделі. |

### A.5. Висновок: наша позиція

Жоден зі знайдених інструментів не поєднує всі три елементи:

1. **Multi-level adaptive collection** (L1/L2/L3 з різною частотою та набором метрик)
2. **Anomaly-triggered escalation** (автоматичний перехід на частіший збір при виявленні проблеми)
3. **On-demand hot buffer** (RAM-only для live-стрімінгу з нульовим disk I/O у idle)

AdaM (§A.3) найближчий за духом, але він орієнтований на IoT з battery constraint, а не на серверний моніторинг. Netdata issue #238 показує, що проблема «always-on overhead» усвідомлена спільнотою, але major-проєкти не вирішують її — натомість оптимізують footprint always-on підходу.

Наш проєкт займає порожню нішу: **мінімальний self-contained моніторинг, який справді спить, коли не потрібний, і миттєво прокидається, коли потрібний**.

---

*Документ — версія 0.7, draft. v0.7: проєкт названо "budyk"; rule engine переписано з кастомного YAML DSL на вбудовану Lua 5.4; Lua sandbox, `watch()` API; AI Tier A генерує Lua `watch()` виклики.*
