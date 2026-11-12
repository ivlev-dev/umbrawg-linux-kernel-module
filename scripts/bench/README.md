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

`vm.sh` создаёт cgroup v2 cpuset-раздел с `cpuset.cpus.partition=isolated`:
ядра получают только процессы прогона, фоновая нагрузка хоста на них не
планируется. Раздел создаётся перед запуском и удаляется после.

```sh
sudo ./scripts/bench/vm.sh vng --run --force-9p --cpus 4 --pin 8-11 --memory 3G -- \
    ./scripts/bench/bench.sh umbrawg 0 tcp1420 8 3
sudo ./scripts/bench/vm.sh vng --run --force-9p --cpus 4 --pin 8-11 --memory 3G -- \
    env FLOWS=4 ./scripts/bench/bench.sh umbrawg 0-3 udp1340 8 3
```

Ядра по умолчанию — `8-11`, переопределяются `BENCH_CPUS`.

## Сравнение сборок и профилирование

```sh
# перемежающееся A/B двух .ko
./scripts/bench/ab.sh old ./src/old.ko new ./src/new.ko tcp1420 0 1 8 2 2
# сравнение с внешней сборкой (amneziawg)
UWG_REF_TOOL=/usr/bin/awg UWG_REF_KIND=amneziawg UWG_REF_MODULE=amneziawg \
    ./scripts/bench/compare.sh tcp1420 0 1 8 2 3
# матрица 4 сценария x профили (default/obfs)
UWG_REF_TOOL=/usr/bin/awg UWG_REF_KIND=amneziawg UWG_REF_MODULE=amneziawg \
    ./scripts/bench/matrix4.sh 2
# perf по одному сценарию (внутри VM, символы из /proc/kallsyms)
PROFILE_CPU=0 ./scripts/bench/profile.sh ./src/umbrawg.ko 0 udp1340 1 8
```

UDP меряется по предельной скорости без потерь: `UWG_UDP_BITRATE` задаёт
суммарную целевую скорость в Мбит/с (делится на число потоков), в выводе
печатается столбец потерь. При `-b 0` однопоточный iperf3-сервер сам
ограничивает приём, поэтому многопоточный UDP гоняется через
`udp-split.sh` — с отдельным сервером на каждый поток:

```sh
UWG_UDP_BITRATE=4500 ./scripts/bench/udp-split.sh umbrawg 0-3 4 8 2
```

Кейсы: `tcp1420`, `udp1320`, `udp1340`, `udp64`.

Переменные:

- `FLOWS=N` — `iperf3 -P N`;
- `UWG_DEV_OPTS` — доп. опции устройства, например
  `"jc 4 jmin 8 jmax 80 s1 50 s2 60 s3 70 s4 80 h1 100 h2 200 h3 300 h4 400"`;
- `UWG_HP_KEY` — base64-ключ header protection; harness сам пишет его в файл;
- `UWG_OPTS_FILE` — файл с аргументами `set` (по одному на строку),
  например `profiles/obfs-full.opts`;
- `UWG_REF_TOOL`, `UWG_REF_KIND`, `UWG_REF_MODULE` — внешняя сборка для
  сравнения (режим `ref`);
- `UWG_MODULE` — путь к `.ko` (для A/B разных сборок);
- `UWG_MODULE_OPTS` — доп. параметры модуля, например `napi_weight=128`;
- `UWG_LABEL` — метка в выводе;
- `UWG_UDP_BITRATE` — суммарная целевая скорость UDP в Мбит/с (0 — без
  ограничения);
- `UWG_CC` — команда пейсеру, например `"all brutal 5000"`; записывается в
  файл устройства в debugfs до начала замера;
- `poll_iters` (параметр модуля) — бюджет адаптивного опроса крипто-воркера,
  0 полностью выключает опрос;
- `poll_all` (параметр модуля) — включить опрос и при многопоточной нагрузке
  (по умолчанию опрос выключается, когда трафик идёт с нескольких CPU:
  в этом режиме он проигрывает, проверено на 4 vCPU).

Замеры UDP чувствительны к тому, где именно теряются пакеты, поэтому
сравнивать сборки надо по предельной скорости без потерь (перебором
`UWG_UDP_BITRATE`), а не по скорости при `-b 0`.

Формат вывода (TSV): `mode case cpu gbps pps busy%`.

## Финальная матрица

```sh
sudo vng --run --force-9p --cpus 12 --pin 0-11 --memory 3G -- \
  env UWG_REF_TOOL=/usr/bin/awg UWG_REF_KIND=amneziawg \
      UWG_REF_MODULE=amneziawg UWG_HP_KEY=<base64> \
      ./scripts/bench/final-matrix.sh
```

Матрица: `default` и `obfs`, TCP 1420 и UDP 1340, 1 и 12 потоков,
сравнение с внешней сборкой (если заданы `UWG_REF_*`).

## Контроль

`scripts/bench/plain.sh` — та же топология без туннеля (потолок veth).
