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
`virtme-ng`. Ядра хоста 8–11 резервируются cgroup v2 isolated-разделом
(`scripts/bench/vm.sh`), в однопоточных сценариях клиент и сервер iperf3
живут на одном ядре.

Гейты (4 vCPU, медиана, `scripts/bench/matrix4.sh` и `compare.sh`):

| Сценарий | amneziawg | umbrawg | Прирост |
|---|---|---|---|
| TCP 1420, 1 поток | 3.9 Гбит/с | 5.4 Гбит/с | +37% |
| TCP 1420, 4 потока | 3.2 Гбит/с | 6.9 Гбит/с | +114% |
| UDP 1340, 1 поток, доставлено | 1.9 Гбит/с | 2.7 Гбит/с | +46% |
| UDP 1340, 4 потока, предел без потерь | 3.5 Гбит/с | 5.0 Гбит/с | +43% |

С профилем обфускации (`scripts/bench/profiles/obfs-full.opts`):

| Сценарий | amneziawg | umbrawg | Прирост |
|---|---|---|---|
| TCP 1420, 1 поток | 1.9 Гбит/с | 3.6 Гбит/с | +88% |
| TCP 1420, 4 потока | 1.6 Гбит/с | 4.1 Гбит/с | +153% |
| UDP 1340, 1 поток, доставлено | 1.5 Гбит/с | 2.3 Гбит/с | +47% |
| UDP 1340, 4 потока, при 4 Гбит/с в канал | 2.5 Гбит/с | 4.0 Гбит/с | +60% |

UDP измеряется по предельной скорости без потерь: задаётся целевая скорость
(`UWG_UDP_BITRATE`), и сравниваются потери. При `-b 0` однопоточный
iperf3-сервер сам становится узким местом, поэтому многопоточный UDP
меряется `scripts/bench/udp-split.sh` с отдельным сервером на каждый поток.
