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
sudo vng --run --force-9p --cpus 12 --memory 3G -- \
    ./scripts/bench/bench.sh umbrawg 0 tcp1420 10 3
sudo vng --run --force-9p --cpus 12 --memory 3G -- \
    env FLOWS=12 ./scripts/bench/bench.sh umbrawg 0-11 tcp1420 10 3
```

Подробности: [`scripts/bench/README.md`](scripts/bench/README.md).

## Архитектура

Описание слоёв, модели конкурентности и использованных возможностей ядра
7.3 — в [`docs/architecture.md`](docs/architecture.md).

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
