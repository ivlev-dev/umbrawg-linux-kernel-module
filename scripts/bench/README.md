# Стенд производительности

Все замеры идут в двух приватных network namespace, соединённых veth.
Хост-сеть (`eno1`, `auto`, docker, veth) не затрагивается.

## Сборка и утилита

```sh
make -C src                 # umbrawg.ko
UWG_SOURCE=/path/to/tool UWG_LEGACY_FAMILY=<legacy-family> \
    ./scripts/dev/make-uwg.sh
```

`uwg` — утилита для семейства `umbrawg`. Для локальных тестов скрипт
`make-uwg.sh` собирает её из совместимого бинарника, подменяя строку
семейства genetlink. Репозиторий не содержит legacy-имён: источник и старое
имя семейства задаются через окружение.

## Запуск на хосте (root)

```sh
sudo ./scripts/dev/smoke.sh
sudo ./scripts/bench/bench.sh umbrawg 10 tcp1420 10 3
sudo FLOWS=12 ./scripts/bench/bench.sh umbrawg 0-11 tcp1420 10 3
```

## Запуск в QEMU/KVM (рекомендуется)

```sh
sudo vng --run --force-9p --cpus 12 --memory 3G -- \
    ./scripts/bench/bench.sh umbrawg 0 tcp1420 10 3
sudo vng --run --force-9p --cpus 12 --memory 3G -- \
    env FLOWS=12 ./scripts/bench/bench.sh umbrawg 0-11 tcp1420 10 3
```

Кейсы: `tcp1420`, `udp1320`, `udp64`.

Переменные:

- `FLOWS=N` — `iperf3 -P N`;
- `UWG_DEV_OPTS` — доп. опции устройства, например
  `"jc 4 jmin 8 jmax 80 s1 50 s2 60 s3 70 s4 80 h1 100 h2 200 h3 300 h4 400"`;
- `UWG_HP_KEY` — base64-ключ header protection; harness сам пишет его в файл;
- `UWG_MODULE` — путь к `.ko` (для A/B разных сборок);
- `UWG_MODULE_OPTS` — доп. параметры модуля, например `napi_weight=128`;
- `UWG_LABEL` — метка в выводе.

Формат вывода (TSV): `mode case cpu gbps pps busy%`.

## Контроль

`scripts/bench/plain.sh` — та же топология без туннеля (потолок veth).
