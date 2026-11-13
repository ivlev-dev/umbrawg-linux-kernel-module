# UmbraWG — модуль ядра Linux

Модуль ядра для защищённых туннелей с обфускацией трафика. Wire-формат
совместим с предыдущей реализацией, но интерфейсы ядра переименованы:
genetlink-семейство, rtnl-kind и тип устройства теперь `umbrawg`.

Проект поддерживает только Linux **7.3+**. Поддержка старых ядер, а также
`compat`-слой и встроенная копия криптографии удалены.

## Требования

- Linux 7.3+ и заголовки ядра (`linux-headers` / `kernel-devel`);
- `make`, компилятор, которым собрано ядро (clang или gcc);
- модули `libcurve25519`, `udp_tunnel`, `ip6_udp_tunnel`;
- утилита `uwg` для конфигурации (см. ниже).

Исходники ядра не нужны — достаточно установленных заголовков.

## Сборка и установка

```sh
git clone git@github.com:ivlev-dev/umbrawg-linux-kernel-module.git
cd umbrawg-linux-kernel-module

make -C src
sudo make -C src install
sudo modprobe umbrawg
```

Если ядро собрано clang-ом, сборочная система сама включает `LLVM=1`.
Модуль будет называться `umbrawg.ko`.

Проверка:

```sh
sudo ip link add uwg0 type umbrawg
ip -d link show uwg0
sudo ip link del uwg0
```

## Утилита `uwg`

`uwg` — userspace-утилита для настройки интерфейсов `umbrawg`. Она
использует genetlink-семейство `umbrawg` и повторяет интерфейс команд
WireGuard-совместимых утилит:

```sh
uwg genkey > private.key
uwg pubkey < private.key > public.key
uwg set uwg0 private-key private.key listen-port 51820
uwg set uwg0 peer "$(cat peer-public.key)" \
    allowed-ips 10.0.0.2/32 endpoint 203.0.113.5:51820
uwg show uwg0
```

Утилита поставляется отдельно. Для локальных тестов можно собрать шим из
совместимого бинарника:

```sh
UWG_SOURCE=/path/to/tool UWG_LEGACY_FAMILY=<legacy-family> \
    ./scripts/dev/make-uwg.sh
```

## Быстрый старт

```sh
sudo ip link add uwg0 type umbrawg
uwg genkey > private.key
uwg set uwg0 private-key private.key listen-port 51820
uwg set uwg0 peer "$(cat peer-public.key)" \
    allowed-ips 10.0.0.2/32 endpoint 203.0.113.5:51820
sudo ip addr add 10.0.0.1/24 dev uwg0
sudo ip link set uwg0 up
```

## Производительность

Замеры на 4 vCPU в QEMU/KVM с зарезервированными ядрами хоста, медиана:

| Сценарий | amneziawg | umbrawg |
|---|---:|---:|
| TCP 1420, 1 поток | 3.9 Гбит/с | 5.4 Гбит/с |
| TCP 1420, 4 потока | 3.0 Гбит/с | 7.0 Гбит/с |
| UDP 1340, 1 поток | 2.2–2.4 Гбит/с | 2.8–3.0 Гбит/с |
| UDP 1340, 4 потока | 3.5 Гбит/с | 5.0 Гбит/с |

С профилем обфускации: TCP +88% на одном потоке и +153% на четырёх,
UDP +47% и +60%. UDP сравнивается по предельной скорости без потерь.
Методика, стенд и профилирование — в
[`scripts/bench/README.md`](scripts/bench/README.md).

## Параметры модуля

- `napi_weight` (по умолчанию 128) — бюджет NAPI на пира;
- `poll_iters` (по умолчанию 512) — бюджет адаптивного опроса
  крипто-воркера, `0` полностью выключает опрос;
- `poll_all` (по умолчанию 0) — включить опрос и при многопоточной
  нагрузке (в этом режиме он обычно проигрывает).

## Управление перегрузкой

Транспорт туннеля — UDP, поэтому очередь и скорость регулируются локально на
отправке. Пейсер работает на каждого пира, wire-формат не меняется. Режимы:

- `none` — выключен (по умолчанию), датапас не меняется;
- `brutal` — фиксированная скорость: token bucket и fq-очередь по потокам
  (8 бакетов по хешу внутреннего 5-tuple, round-robin), без реакции на потери;
- `bbr` — адаптивная скорость: измеряет доставку и RTT по собственному
  каналу обратной связи, разгоняется от `cc_rate` (или 64 кбит/с) и не
  ограничивает канал, пока не упрётся в реальную полосу или потери.

Настройка через debugfs:

```sh
ls /sys/kernel/debug/umbrawg/                 # dev1, dev2, ...
cat /sys/kernel/debug/umbrawg/dev1            # netdev/netns и список пиров
echo "all brutal 5000" > /sys/kernel/debug/umbrawg/dev1   # все пиры, 5 Гбит/с
echo "7 brutal 1000"   > /sys/kernel/debug/umbrawg/dev1   # только пир с id 7
echo "all none"        > /sys/kernel/debug/umbrawg/dev1   # выключить
```

Скорость задаётся для внешнего (UDP) трафика: полезная нагрузка получается
примерно на 6% меньше при MTU 1420. Всплеск токенов — параметр модуля
`cc_burst_ms` (по умолчанию 10 мс), частота проб обратной связи —
`cc_fb_ms` (по умолчанию 20 мс). Пейсер включается только при явной
настройке: при `none` на горячем пути остаётся одна проверка static key.

Для `bbr` число после режима (`all bbr 1000`) — стартовая скорость в
Мбит/с; без числа разгон начинается с минимальной скорости. После разгона
контроллер сам находит доступную полосу и адаптируется к потерям. Текущее
состояние видно в debugfs:

```sh
cat /sys/kernel/debug/umbrawg/dev1
#  bbr: phase=probe_bw pace_bps=... bw_bps=... full_bw_bps=... loss=...
```

## Параметры конфигурации

Все параметры, кроме `Jc`, `Jmin` и `Jmax`, должны совпадать на обеих
сторонах.

- `Jc` — количество junk-пакетов, `1..128` (рекомендуется `4..12`);
- `Jmin`/`Jmax` — размер junk-пакетов, `Jmin < Jmax`, рекомендуется `8..80`;
- `S1` — padding инициирующего пакета, `S1 ≤ 1132`;
- `S2` — padding ответа, `S2 ≤ 1188`; `S1 + 56 ≠ S2`;
- `S3` — padding cookie-пакета;
- `S4` — padding транспортных пакетов;
- `H1..H4` — типы сообщений, должны быть уникальны;
- `I1..I5` — шаблоны junk-пакетов перед handshake;
- `header-protection-key` — 32-байтный ключ защиты заголовков;
- `content-padding-addition` — дополнительный padding полезной нагрузки;
- `random-trailers` — случайные хвосты пакетов;
- `disable-cookies` — отключить cookie-механизм.

При включённой защите заголовков `S1..S4` должны быть не меньше `12`.
Максимальные значения `S1/S2` рассчитаны для MTU 1280.

## Тестирование

Тесты запускаются только в изолированных network namespace; хост-сеть не
затрагивается. Рекомендуемый способ — QEMU/KVM через `virtme-ng`:

```sh
sudo ./scripts/dev/smoke.sh
sudo ./scripts/bench/vm.sh vng --run --force-9p --cpus 4 --pin 8-11 --memory 3G -- \
    ./scripts/bench/bench.sh umbrawg 0 tcp1420 8 3
sudo ./scripts/bench/vm.sh vng --run --force-9p --cpus 4 --pin 8-11 --memory 3G -- \
    env FLOWS=4 ./scripts/bench/bench.sh umbrawg 0-3 udp1340 8 3
```

`scripts/bench/vm.sh` резервирует ядра хоста через cgroup v2
(`cpuset.cpus.partition=isolated`), поэтому планировщик не размещает на них
посторонние задачи, и замеры не зависят от фоновой нагрузки. Раздел
создаётся на время прогона и удаляется после.

Подробности: [`scripts/bench/README.md`](scripts/bench/README.md).

## Архитектура

Описание слоёв, модели конкурентности и использованных возможностей ядра
7.3 — в [`docs/architecture.md`](docs/architecture.md).

Протокол дополнительного слоя (FEC, обратная связь, контроллеры скорости) —
в [`docs/protocol.md`](docs/protocol.md); карта изменений при переходе с
AmneziaWG — в [`docs/migration.md`](docs/migration.md).

## Диагностика

Динамическая отладка:

```sh
echo "module umbrawg +p" | sudo tee /sys/kernel/debug/dynamic_debug/control
dmesg -wT
```

Если модуль не загружается, проверьте, что загружены зависимости:

```sh
modinfo -F depends umbrawg.ko
```

## Миграция

Netlink-семейство и rtnl-kind переименованы в `umbrawg`. Скрипты и
конфигурации, которые использовали прежнее имя, нужно обновить. Wire-формат
не менялся, поэтому совместимость на уровне протокола сохраняется.

## Лицензия и авторы

Проект распространяется по лицензии [GPLv2](COPYING).

Авторы оригинала: Jason A. Donenfeld, AmneziaVPN. Форк и переработка:
ivlev-dev.
