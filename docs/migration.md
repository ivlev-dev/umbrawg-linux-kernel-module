# Переход с AmneziaWG на umbrawg

Краткая карта изменений: что именно правится в кодовой базе AmneziaWG,
чтобы получить umbrawg. Порядок соответствует истории коммитов репозитория.

## 0. Требования

- ядро Linux 7.3+ и только оно: `compat/`, копии криптобиблиотек и
  `#if LINUX_VERSION_CODE` вырезаются;
- сборка тем же компилятором, что и ядро (clang → `LLVM=1`), LTO/ThinLTO
  наследуются от ядра;
- `-march`, `-mtune` и AVX-512 в модуле не задаются: модуль собирается
  флагами ядра, а ускоренные реализации ChaCha20/Poly1305 выбираются
  рантайм-диспетчеризацией крипто-библиотеки.

## 1. Переименование (механически)

| Было | Стало |
|---|---|
| модуль `amneziawg.ko` | `umbrawg.ko` |
| genetlink-семейство `amneziawg` | `umbrawg` |
| rtnl-kind `amneziawg` | `umbrawg` |
| утилита `awg` | `uwg` |

Правится: `src/Kbuild` (`UWG_NAME`), `src/Makefile`, `src/netlink.c`
(имена семейства), `src/device.c` (`rtnl_link_ops.id`), `dkms.conf`,
`uwg_identity.h` (строки для `ip link`), скрипты.

Проверка: `ip link add uwg0 type umbrawg`, `uwg show uwg0`.

## 2. Датапас: что заменяется

### 2.1. Очереди крипто-воркеров

Было: один `ptr_ring` на очередь устройства, `wg_crypt_queue_consume()`
забирает по одному пакету, `queue_work_on(wg_cpumask_next_online(...))`.

Стало:

- `struct crypt_queue { struct ptr_ring __percpu *ring; ... }` — кольцо на
  каждый CPU, индекс кольца выбирается `wg_cpumask_next_online`;
- воркер обслуживает своё кольцо батчами
  (`ptr_ring_consume_batched_bh`, до `WG_CRYPT_BATCH = 32`);
- при опустошении — короткий адаптивный опрос (`poll_iters`, 512 итераций
  `cpu_relax()`), воркер остаётся запланированным;
- пробуждение элиминируется флагом: `queue_work_on()` только при переходе
  флага `kick` 0→1 (см. `wg_multicore_worker_kick`);
- при многопоточной нагрузке (`tx_parallel`) опрос выключается
  (`poll_all` — диагностический возврат к прежнему поведению).

### 2.2. Порядок отправки

`tx_parallel` теперь затухает через секунду после последней смены
CPU-отправителя (раньше метка времени обновлялась каждым пакетом и флаг
залипал). Упорядоченный путь (`prev_queue` + tx-worker) не менялся.

### 2.3. Статистика

`peer->rx_bytes/tx_bytes` — per-CPU (`struct uwg_peer_stats __percpu *`),
netlink суммирует по CPU (`wg_peer_bytes`). Счётчик глубины `prev_queue`
больше не атомарный (мягкий предохранитель).

### 2.4. Обфускация

- на приёме один блок ChaCha20 (16 байт keystream) вместо двух: keystream
  используется и для маски типа, и для XOR заголовка; для handshake
  (заголовок > 16 байт) остаётся старый путь с откатом счётчика;
- `MESSAGE_DATA` проверяется первым в `uwg_determine_type_and_padding`.

Формат на проводе не меняется.

## 3. Пейсер (новый слой)

Добавляется `src/cc.c` + `src/cc.h`:

- `struct uwg_cc` в `struct wg_peer`: режим, скорость, token bucket,
  hrtimer, 8 fq-бакетов по хешу внутреннего 5-tuple;
- точка включения — `wg_packet_create_data_done()`:
  `static_branch_unlikely(&uwg_cc_active)` → при выключенном пейсере
  добавляется одна проверка static key;
- точка выхода — `wg_socket_send_skb_to_peer()` (не меняется);
- конфигурация — debugfs `/sys/kernel/debug/umbrawg/devN`:
  `all brutal 5000`, `7 none`, `all fec 10 1`, `all off`.

Режимы: `none`, `brutal` (фиксированная скорость). `bbr` зарезервирован
под слой из `docs/protocol.md`.

## 4. Слой umbrawg-tunnel (FEC и обратная связь)

Спецификация: [`protocol.md`](protocol.md). Точки интеграции:

1. **Отправка данных** — `wg_packet_create_data_done()`, после шифрования:
   собрать шифротекст пакета (`skb->data + padding + 16`,
   `skb->len - padding - 16`) в накопитель FEC.
2. **Приём данных** — `wg_packet_consume_data()`, до постановки в очередь
   расшифровки: отметить counter и сохранить копию шифротекста для
   восстановления.
3. **Приём служебного сообщения** — `wg_packet_consume_data_done()`:
   если первый байт расшифрованного payload — `0x00`, а второй — известный
   вид (`0x01..0x03`), вызвать обработчик слоя и не отдавать пакет
   в сетевой стек. Проверка стоит перед обработкой keepalive.
4. **Восстановленный пакет** — собрать `[message_data header][ciphertext]`
   и отдать в `wg_packet_consume_data()`; дальше он проходит обычную
   расшифровку, Poly1305 и replay-окно.
5. **Периодика** — hrtimer на пира для CC_REQUEST (20 мс); обратная связь
   идёт как обычное data-сообщение.

Служебные сообщения кодируются как data-пакеты (тип H4, header protection,
padding, трейлеры), поэтому обфускация не нарушается: новых типов на
проводе не появляется.

## 5. Чеклист проверки после порта

```sh
make -C src module-debug                       # self-tests в dmesg
UWG_MODULE=./src/umbrawg.ko ./scripts/dev/smoke.sh
./scripts/bench/vm.sh vng --run --force-9p --cpus 4 --pin 8-11 --memory 3G -- \
    env UWG_REF_TOOL=/usr/bin/awg UWG_REF_KIND=amneziawg \
        UWG_REF_MODULE=amneziawg ./scripts/bench/matrix4.sh 2
```

Ожидаемые self-tests: `allowedips`, `nonce counter`, `ratelimiter`.
Проверить также, что при `cc=none` и выключенном FEC разница с исходной
сборкой в пределах шума.
