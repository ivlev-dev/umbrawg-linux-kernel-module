# Архитектура UmbraWG

Модуль собран из одного кодового пути под Linux 7.3+. Наследие
(`src/compat`, `src/crypto/zinc`, старые arch-конфиги) удалено.

## Идентичность

```sh
make                    # umbrawg.ko, genetlink "umbrawg", rtnl "umbrawg"
make UWG_NAME=umbrawg   # явное указание имени
```

`src/uwg_identity.h` — единственная точка, где определяется `UWG_NAME`.
Wire-формат не зависит от имени: netlink/rtnl переименованы, протокол
остался прежним.

## Слои (целевая структура)

```
netdev/uwg_netdev.c   netdev lifecycle, ndo_start_xmit, NAPI
datapath/uwg_tx.c     TX: route -> keypair -> encrypt -> udp xmit
datapath/uwg_rx.c     RX: parse -> keypair -> decrypt -> GRO
crypto/uwg_aead.c     chacha20poly1305 / blake2s
noise/uwg_noise.c     Noise IK, keypairs, replay window
obfs/uwg_obfs.c       padding, junk, header protection
net/uwg_socket.c      udp_tunnel, endpoint, dst_cache
ctrl/uwg_netlink.c    genetlink ABI
ctrl/uwg_timers.c     timers, coalescing
peers/                peer lifecycle, allowedips, lookup
```

Сейчас код остаётся в плоском `src/`; структура выше — направление
дальнейшей декомпозиции.

## Конкурентность

- В датапасе нет sleepable-локов и глобальных cacheline-записей.
- Ключ header protection — RCU-снапшот (`struct uwg_hp_key __rcu *`);
  запись только из netlink под `update_lock`, чтение без локов.
- Endpoint пира — `seqlock`: читатель копирует структуру, писатель
  обновляет её редко.
- Счётчики `rx_bytes`/`tx_bytes` — `atomic64_t`; обновляются из нескольких
  CPU.
- TX:
  - по умолчанию упорядоченный путь (`prev_queue` + один tx-worker на пира);
  - если пакеты пира пришли более чем с одного CPU (`tx_parallel`),
    включается параллельный путь: per-CPU encrypt-worker сам отправляет
    список, без per-peer сериализации;
  - выбор фиксируется в `PACKET_CB(first)->parallel` на время обработки
    списка.
- RX: per-CPU кольца decrypt-worker'ов с work-stealing (порядок и replay
  window сохраняются per-peer NAPI); `napi_consume_skb` для отложенного free,
  NAPI weight 128 (настраивается параметром `napi_weight`).
- Конфиг (padding, junk, заголовки) читается датапасом без локов.
- Очереди крипто-воркеров распределены по CPU (`struct ptr_ring __percpu`),
  producer-lock больше не общий. Воркер обслуживает в первую очередь своё
  кольцо (батчами через `ptr_ring_consume_batched_bh`), при опустошении
  забирает работу из чужих колец.
- Пробуждение воркера элиминируется флагом `kick`: producer вызывает
  `queue_work_on()` только на переходе флага 0→1, то есть когда воркер уже
  не запланирован.
- После обработки очереди воркер выполняет короткий адаптивный опрос кольца
  (`poll_iters`, по умолчанию 512 итераций `cpu_relax()`), оставаясь
  «запланированным»; это убирает `__queue_work` с горячего пути. Опрос
  выключается, когда трафик пира приходит более чем с одного CPU
  (`tx_parallel`), чтобы не отнимать CPU у приложения при многопоточной
  нагрузке.

## Что используется из 7.3

- `netif_threaded_enable()` и per-NAPI конфигурация;
- `netif_napi_add_weight()`;
- `ptr_ring`, `WQ_PERCPU`, `dev_sw_netstats`, `dst_cache`;
- kernel crypto lib с runtime-диспетчеризацией (ChaCha20/Poly1305/BLAKE2s/
  Curve25519);
- `seqlock`, `RCU`, `atomic64`.

Не используется: AVX-512 в kernel mode, `-march=native`, io_uring
(userspace-only).

## Измерения

Стенд: `scripts/bench` (две приватные netns + veth), запуск в QEMU/KVM через
`virtme-ng`, чтобы не рисковать хостом.

Стенд: 4 vCPU в QEMU/KVM, ядра хоста 8–11 зарезервированы cgroup v2
isolated-разделом (`scripts/bench/vm.sh`), iperf3-сервер и клиент на одном
ядре для однопоточных сценариев.

Гейты (4 vCPU, медиана):

| Сценарий | amneziawg | umbrawg | Прирост |
|---|---|---|---|
| TCP 1420, 1 поток | 4.0 Гбит/с | 5.7 Гбит/с | +42% |
| TCP 1420, 4 потока | 3.4 Гбит/с | 7.4 Гбит/с | +118% |
| UDP 1340, 1 поток, без потерь | ~2.4 Гбит/с | ~2.9 Гбит/с | +21% |
| UDP 1340, 4 потока, без потерь | ~3.5 Гбит/с | ~5.0 Гбит/с | +43% |

UDP измеряется по предельной скорости без потерь: задаётся целевая скорость
(`UWG_UDP_BITRATE`), и сравниваются потери. При `-b 0` однопоточный
iperf3-сервер сам становится узким местом, поэтому многопоточный UDP
меряется `scripts/bench/udp-split.sh` с отдельным сервером на каждый поток.
