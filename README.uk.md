# budyk

[English](README.md) | **Українська**

**Легкий моніторинг серверів з адаптивним збором метрик.**

budyk («будик») — автономний демон моніторингу для серверів на FreeBSD і
Linux. Це один статичний бінарний файл без залежностей під час виконання
і без бази даних. Метрики зберігаються в кільцевих файлах фіксованого
розміру, вебпанель вбудована в бінарник, а правила сповіщень пишуться
звичайним Lua або YAML.

## Головна ідея: збирати рівно стільки, скільки потрібно

Більшість систем моніторингу знімають метрики щосекунди, навіть коли на
них ніхто не дивиться. budyk перемикається між трьома рівнями збору:

| Рівень | Частота | Коли |
|--------|---------|------|
| **L1 Heartbeat** | раз на 5 хв | ніхто не дивиться, система в нормі |
| **L2 Watchful** | 15–60 с | перевищено поріг (навантаження, CPU, swap) |
| **L3 Active** | 1 Гц | підключено вебпанель або TUI |

Рівень підвищується одразу, щойно з'являється аномалія або хтось
відкриває панель, і знижується назад після періоду гістерезису. На
спокійному сервері budyk здебільшого спить.

## Можливості

- **Метрики:** CPU, пам'ять, swap, середнє навантаження, дисковий I/O,
  мережа, процеси, ентропія, температура, аптайм, а також власні CPU та
  RSS budyk.
- **Багаторівневе сховище:** сирі вибірки з частотою 1 Гц, а також
  1-хвилинні й 5-хвилинні агрегати в кільцевих файлах з обмеженням
  розміру, без бази даних.
- **Вебпанель:** живі графіки через WebSocket і перегляд історії за
  збереженими даними. Необов'язковий вхід за паролем (Argon2id).
- **Термінальний інтерфейс:** `budyk tui`.
- **Правила:** правила `watch()` на Lua або в простому YAML-форматі;
  перезавантажуються за сигналом `SIGHUP` без перезапуску. Лічильники
  cooldown переживають перезапуск.
- **Канали сповіщень:** ntfy, Discord, Telegram, e-mail через SMTP,
  SMS через Twilio.
- **Реагування на інциденти:** стеження за змінами файлів
  (`/etc/sudoers`, `/etc/passwd`, …) і `freeze()` / `unfreeze()`, щоб
  призупинити процес, що вийшов з-під контролю. Обидві функції вимкнені
  за замовчуванням.
- **Підказки правил:** `budyk suggest-rules` пропонує пороги на основі
  зібраної історії.

## Встановлення

### Готові бінарні файли

До кожного [релізу](https://github.com/click0/budyk/releases) додаються
статичні бінарні файли для Linux amd64 та FreeBSD 14.2 / 15.0 amd64:

```
budyk-<версія>-linux-amd64.tar.gz
budyk-<версія>-freebsd14.2-amd64.tar.gz
budyk-<версія>-freebsd15.0-amd64.tar.gz
```

Кожен архів має також варіант `-debug` із налагоджувальними символами та
файл контрольної суми `.sha256`. Розпакуйте архів і покладіть `budyk` у
каталог із вашого `PATH`.

### Збирання з вихідного коду

Залежності:

```sh
# FreeBSD (ncurses входить до базової системи)
pkg install cmake pkgconf lua54 libyaml libargon2

# Debian / Ubuntu
apt install cmake g++ pkg-config liblua5.4-dev libyaml-dev libargon2-dev libncurses-dev
```

Збирання, тести й встановлення:

```sh
cmake -B build
cmake --build build -j
ctest --test-dir build
cmake --install build      # бінарник, приклад конфігурації, правила, man-сторінка
```

За замовчуванням збірка повністю статична. Щоб лінкувати зі спільними
бібліотеками, додайте `-DSTATIC_LINK=OFF`.

### Запуск як служби

- **FreeBSD:** заготовка порту та rc.d-скрипт лежать у
  [`addons/freebsd/`](addons/freebsd/). rc.d-скрипт читає змінні
  `budyk_enable`, `budyk_config`, `budyk_user` і `budyk_flags`.
- **Linux:** захищений systemd-юніт лежить у
  [`addons/linux/budyk.service`](addons/linux/budyk.service).
- **Docker:** `Dockerfile` і `docker-compose.yml` лежать у
  [`addons/docker/`](addons/docker/).

## Швидкий старт

```sh
cp config.example.yaml config.yaml     # задайте data_dir, listen, port
budyk serve --config config.yaml
```

Вебпанель відкривається за адресою <http://127.0.0.1:8080>. Термінальний
інтерфейс:

```sh
budyk tui [--host 127.0.0.1] [--port 8080]
```

Щоб захистити панель паролем, згенеруйте хеш і впишіть його в
конфігурацію:

```sh
budyk hash-password                    # друкує хеш $argon2id$...
```

```yaml
web:
  auth:
    enabled: true
    password_hash: "$argon2id$v=19$..."   # лапки обов'язкові
```

> TUI звертається до HTTP API без входу, тому працює лише тоді, коли
> `web.auth.enabled` має значення `false`.

Усі налаштування описано в
[`config.example.yaml`](config.example.yaml).

## Правила

Вкажіть у `rules.path` файл `.lua` або `.yaml`. Після змін надішліть
`SIGHUP` (`kill -HUP <pid>`), і правила перезавантажаться без перезапуску.
Лічильники cooldown кожного правила зберігаються.

### Lua

```lua
watch("high_cpu", {
    when      = function() return cpu.total_percent > 90 end,
    for_ticks = 5,       -- скільки тіків поспіль має виконуватись умова (за замовч. 1)
    cooldown  = 60,      -- скільки тіків мовчати після спрацювання (за замовч. = for_ticks)
    action    = function()
        alert("high_cpu", "warning",
              string.format("CPU at %.0f%%", cpu.total_percent))
    end,
})
```

`when` обов'язкове. `action` має бути **функцією**: правило без неї лише
рахує спрацювання і нічого не робить.

Функції, доступні в правилах:

| Функція | Призначення |
|---------|-------------|
| `alert(name, severity, message)` | Надіслати в усі налаштовані канали. `severity`: `"info"`, `"warning"` (за замовч.) або `"critical"`. |
| `print(...)` | Записати в журнал демона. |
| `exec(cmd [, timeout_s])` | Запустити програму: рядок або таблиця argv, абсолютний шлях, тайм-аут за замовчуванням 30 с. **Вимкнено за замовчуванням.** Вмикається через `rules.exec.enabled` або `--enable-exec`, обмежується списком `rules.exec.allow`. Повертає `{ ok, exit_status, signal, timed_out, elapsed_seconds }`. |
| `freeze(pid)` / `unfreeze(pid)` | Надіслати `SIGSTOP` / `SIGCONT`. **Вимкнено за замовчуванням.** Вмикається через `rules.freeze.enabled` або `--enable-freeze`, обмежується за іменем процесу списком `rules.freeze.allow`. |
| `escalate()` | Зарезервовано. Наразі нічого не робить. |

Метрики оновлюються щотіку в глобальних змінних:

| Змінна | Поля |
|--------|------|
| `cpu` | `total_percent`, `count` |
| `mem` | `total`, `available`, `available_percent` |
| `swap` | `total`, `used`, `used_percent` |
| `load` | `avg_1m`, `avg_5m`, `avg_15m` |
| `disk` | `read_bytes_per_sec`, `write_bytes_per_sec`, `device_count` |
| `net` | `rx_bytes_per_sec`, `tx_bytes_per_sec`, `interface_count` |
| `proc` | `total`, `running` |
| `entropy` | `available_bits`, `present` |
| `thermal` | `max_celsius`, `sensor_count`, `present` |
| `self_` | `rss_bytes`, `peak_rss_bytes`, `cpu_user_seconds`, `cpu_system_seconds` |
| `uptime_seconds` | число |
| `files` | `files["/шлях"].modifies`, `.deletes`, `.tampered` (коли увімкнено `security.file_watch`) |

Правила виконуються в пісочниці, де є лише базова бібліотека, `math`,
`string` і `table`. `io`, `os`, `require`, `load`, `loadfile` і `dofile`
недоступні.

### YAML

Для простих порогових правил:

```yaml
- name: disk_busy
  when: "disk.write_bytes_per_sec > 200000000"   # вираз Lua
  for_ticks: 3
  cooldown: 60
  severity: critical        # info | warning | critical
  action: alert             # alert | log
  message: "Disk writes above 200 MB/s"
```

Під час завантаження файлу кожен запис перетворюється на виклик
`watch()`.

### Підказки правил

```sh
budyk suggest-rules --config config.yaml --window 7d --output suggested.lua
```

Команда читає зібрану історію і генерує правила `watch()` з порогами на
її основі. Перегляньте їх, перш ніж використовувати.

## Сповіщення

Канали задаються в `alerts.channels`. `alert()` надсилає повідомлення в
усі канали; якщо один з них не спрацював, решта все одно його отримають.

| `type` | Куди |
|--------|------|
| `ntfy` | ntfy.sh або власний сервер ntfy |
| `discord` | вебхук Discord |
| `telegram` | Telegram Bot API (чат, група або канал) |
| `smtp` | e-mail через SMTP / SMTPS |
| `twilio` | SMS через Twilio |

Які поля потрібні кожному типу, див. у
[`config.example.yaml`](config.example.yaml).

## HTTP API

| Метод | Шлях | Вхід | Повертає |
|-------|------|------|----------|
| `GET` | `/api/health` | ні | статус, версія, каталог даних |
| `POST` | `/api/auth/login` | ні | тіло `{"password": "..."}`; встановлює cookie `budyk_session` |
| `POST` | `/api/auth/logout` | ні | завершує сесію |
| `GET` | `/api/samples` | так | останні вибірки з буфера в пам'яті |
| `GET` | `/api/range` | так | збережена історія: `since`, `until` (наносекунди від епохи), `tier` (`1` сирі, `2` 1-хв, `3` 5-хв), `limit` (до 5000) |
| `GET` | `/api/ws` | так | живий потік через WebSocket |

Стовпець «Вхід» діє лише тоді, коли `web.auth.enabled` має значення `true`.

## Сигнали

| Сигнал | Дія |
|--------|-----|
| `SIGHUP` | Перезавантажити файл правил. |
| `SIGTERM`, `SIGINT` | Зберегти стан правил і коректно завершитися. |

## Платформи

- **FreeBSD 14.2, 15.0:** основна платформа, збирається і тестується в CI.
- **Linux:** збирається і тестується в CI на Ubuntu.

Інші версії та системи не тестуються. Для NetBSD і OpenBSD колектора
метрик поки немає.

## Ліцензія

[BSD-3-Clause](LICENSE)
