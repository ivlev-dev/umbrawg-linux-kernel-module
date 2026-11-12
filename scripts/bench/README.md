# Bench harness

Все замеры идут в двух приватных network namespace, соединённых veth.
Хост-сеть (`eno1`, `auto`, docker, veth) не затрагивается. Для тестов рядом
с живым `amneziawg` dev-модуль собирается с именем `umbra-wg`.

## Сборка и dev-инструмент

```sh
make -C src                 # umbra-wg.ko
./scripts/dev/make-awg-umbra.sh
```

`awg-umbra` — копия `awg` с тем же по длине патчем семейства genetlink
(`amneziawg` -> `umbra-wg\\0`). Нужна только для dev-модуля.

## Запуск на хосте (требует root)

```sh
sudo ./scripts/dev/smoke.sh
sudo ./scripts/bench/bench.sh new 10 tcp1420 10 3
sudo FLOWS=12 ./scripts/bench/bench.sh new 0-11 tcp1420 10 3
```

## Запуск в QEMU/KVM (рекомендуется)

```sh
sudo vng --run --force-9p --cpus 12 --memory 3G -- \
    ./scripts/bench/bench.sh new 0 tcp1420 10 3
```

Режимы:

| Режим | Сторона A | Сторона B |
|---|---|---|
| `old` | amneziawg | amneziawg |
| `new` | umbra-wg | umbra-wg |
| `cross` | amneziawg | umbra-wg |
| `cross2` | umbra-wg | amneziawg |

Кейсы: `tcp1420`, `udp1320`, `udp64`.

Переменные:

- `FLOWS=N` — `iperf3 -P N`;
- `AWG_DEV_OPTS` — доп. опции устройства, например
  `"jc 4 jmin 8 jmax 80 s1 50 s2 60 s3 70 s4 80 h1 100 h2 200 h3 300 h4 400"`;
- `AWG_HP_KEY` — base64-ключ header protection; harness сам пишет его в файл.

Формат вывода (TSV): `mode case cpu gbps pps busy%`.

## Контроль

`scripts/bench/plain.sh` — та же топология без туннеля (потолок veth).
