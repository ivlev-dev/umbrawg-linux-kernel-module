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
- RX: per-CPU decrypt-worker + per-peer NAPI (порядок и replay window
  сохраняются).
- Конфиг (padding, junk, заголовки) читается датапасом без локов.

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

Гейты (TCP, 1420B, VM 12 vCPU):

| Сценарий | Базовая версия | umbrawg | Прирост |
|---|---|---|---|
| 1 поток, 1 CPU | 2.22 Гбит/с | 3.47 Гбит/с | +56% |
| 12 потоков, 12 CPU | 2.14 Гбит/с | 9.16 Гбит/с | +329% |

С обфускацией (Jc/Jmin/Jmax, S1–S4, H1–H4, header protection) и 12 потоками:
6.2 Гбит/с.
