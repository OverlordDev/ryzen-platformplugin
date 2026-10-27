# RyzenPlatformPlugin

Kext + демон для управления частотой AMD Ryzen под macOS. Целевая машина —
Ryzen 7 5800H (Cezanne), Hackintosh на OpenCore.

Идея: не зажимать частоту намертво и не держать все ядра на максимуме, а работать по принципу
**race-to-sleep**: короткий выстрел на высокой частоте под конкретный дедлайн (кадр, клик, запуск
приложения) — и быстрое падение в глубокий сон.

## Зачем это вообще нужно

На этой машине macOS **не управляет процессором**. Вообще.

| Что | Значение |
|---|---|
| `machdep.xcpm.mode` | `0` — XCPM выключен |
| `CPUPStates` у `X86PlatformPlugin` | `()` — пусто |
| `BoostLimit` в профиле | `0` |
| `hw.cpufrequency` | `3200000000` — статик, не меняется |
| `machdep.cpu.thermal.sensor` | `0` — датчика температуры CPU нет |
| SMC | отдаёт **0 вентиляторов** и ни одного ключа с температурой |

То есть: ни одного рычага частоты, ни одного термодатчика. Частотой распоряжается SMU по своему
усмотрению, а ОС об этом ничего не знает. Единственный способ что-то изменить — CPPC MSR
(`MSR_AMD_CPPC_REQ`, `0xC00102B3`), и писать их можно только из kext'а.

## Что уже работает

| Путь | Что это |
|---|---|
| `kext/` | **v0.1 «глаза»**: read-only kext. Читает CPPC, P-state и Tctl, публикует в IORegistry |
| `tools/ryzenstat.c` | Консольная читалка: таблица по всем ядрам, без root |
| `sim/policy_sim.py` | Модель политики: тепловой RC-контур, задержка SMU, сравнение режимов |
| `docs/TZ.md` | Полное ТЗ: физика, архитектура, выходной порт, этапы, критерии приёмки |
| `docs/SAFETY.md` | Watchdog, kill switch, восстановление после паники, красные линии |
| `shared/RyzenPlatformAPI.h` | ABI-контракт между kext и демоном |
| `scripts/oc-add-kext.py` | Добавляет kext в `Kernel -> Add` конфига OpenCore (идемпотентно) |
| `scripts/dev-load.sh` | Загрузка kext'а в живую систему для разработки |

### Kext v0.1

Ничего не пишет. Раз в секунду на каждом CPU читает:

- `CPPC_CAP1 / ENABLE / CAP2 / REQ / STATUS` (`0xC00102B0..B4`)
- текущий P-state (`0xC0010063`) и его определение (`0xC0010064 + n`)
- Tctl через SMN (`0x00059800`, host bridge `0:0.0`, index/data `0x60/0x64`)

Безопасность: каждый MSR читается через `rdmsr_carefully()` — несуществующий MSR возвращает
ошибку, а не панику. CPPC трогается только если CPUID Fn8000_0008 EBX[27] это разрешает.
Boot-arg `ryzenplatform=0` не даёт kext'у стартовать.

## Сборка

```bash
cd kext && make            # -> build/run/RyzenPlatform.kext
cd ../tools && make        # -> tools/ryzenstat
```

Нужны Command Line Tools (или Xcode). Xcode-проект не нужен, всё собирается Makefile'ом.

## Установка

**Через OpenCore** (единственный путь на macOS 15 — сторонний kext живёт в aux kernel
collection и собирается только на загрузке):

```bash
python3 scripts/oc-add-kext.py /Volumes/EFI/EFI/OC/config.plist RyzenPlatform.kext
cp -R build/run/RyzenPlatform.kext /Volumes/EFI/EFI/OC/Kexts/
```

Скрипт идемпотентен и проверяет, что конфиг остался валидным. Бэкап конфига делайте сами —
он не делает этого за вас.

**Для разработки** — `sudo scripts/dev-load.sh load`. На macOS 15 это упирается в
`requires a reboot`: живая загрузка сторонних kext'ов больше не поддерживается.

## Проверка

```bash
tools/ryzenstat          # один семпл
tools/ryzenstat -w 5     # пять подряд
```

Печатает по каждому потоку: `cap1` (low/nl/nom/high), `req` (min/max/desired/epp), статус CPPC,
P-state, FID/DID → МГц, и Tctl.

## Три узла

1. **Входной порт** — не «подменить X86PlatformPlugin и всё», а снять сигналы:
   загрузка ядер, активность `WindowServer`, HID-события, активация приложений,
   каденция дисплея, температура. В v0.1 — ко-пилот, а не замена.
2. **Мозг** — дедлайн-ориентированная политика с двумя уровнями простоя и гистерезисом.
   Живёт в userspace-демоне: его легко логировать, отлаживать и обновлять.
3. **Выходной порт** — CPPC (`MSR_AMD_CPPC_REQ`), а не сырой SMU mailbox.
   SMU сам держит замкнутый контур частоты/напряжения; ОС даёт ему per-core hints.

## Три вещи, которые нельзя делать

- Писать в SMU mailbox по угаданным message ID. Cezanne этого не прощает.
- Трогать напряжение / Curve Optimizer.
- Оставлять демона без watchdog: если он умрёт, все ядра не должны остаться на 800 МГц.

## Дальше

- [x] Замерить baseline на живой машине.
- [x] Kext «глаза»: CPPC read, P-state, Tctl, boot-arg kill switch.
- [ ] Посмотреть, что реально отдаёт SMU: `cppc.enabled`, границы `cap1`.
- [ ] Kext-актуатор: запись `CPPC_REQ`, verify read-back, watchdog.
- [ ] Демон в shadow-режиме: только логировать решения, ничего не писать.
- [ ] Замкнутый контур, тепловой guard, sleep/wake.

Подробности — в `docs/TZ.md`.
