# daemon: мозг политики

> Дизайн-карта userspace-демона `ryzend`. Код пишется после того, как kext-актуатор
> научится безопасно применять per-core CPPC и возвращать auto.

## Почему userspace

- Легко логировать, отлаживать, обновлять без перезагрузки kext.
- Ошибка политики не паникует ядро.
- Можно гонять в shadow-режиме: решения считаются, но ничего не пишется.
- Kext остаётся тонким: clamp, write, read-back, watchdog, thermal guard.

## Поток

```
HID event / timer 1 ms / NSWorkspace notification
        │
        ▼
  SignalSnapshot  ──►  Policy::tick()  ──►  Decision
        │                                      │
        │                                      ├─ shadow: log only
        │                                      └─ apply: IOConnectCallStructMethod
        ▼
  CSV / os_log / ring buffer
```

## Источники сигналов

| Сигнал | Как получаем | Зачем |
|---|---|---|
| Загрузка ядер | `host_processor_info(PROCESSOR_CPU_LOAD_INFO)` 100–200 Гц | фон/нагрузка |
| Активность WindowServer | `proc_pid_rusage` 120–240 Гц, дельта CPU time | рендер кадра |
| HID-события | `CGEventTap` (Accessibility) | предиктор, буст до кадра |
| Активация приложения | `NSWorkspace` notification | мгновенный буст на запуск |
| Каденция дисплея | `CVDisplayLink` | фаза кадра, 60/120 Гц |
| Power/perf states | `IOReport` (private), если доступен | package power, residency |
| Thermal pressure | `pmset -g therm` / `IOReport` | guard |
| Температура/мощность | kext `readState` (SMN) — fallback | guard |

Если `IOReport` на Hackintosh не отдаёт нужные каналы — не героически его ковырять, а
использовать kext-сенсоры. Если и там пусто — считать, что thermal guard активен, и не
разгоняться.

## Состояния политики

См. `docs/TZ.md`, раздел 3.2. Кратко:

- `DEEP_IDLE` — все `lowest`, EPP 255, после 500 мс тишины.
- `IDLE_READY` — preferred `nominal`, остальные `lowest`, 0–500 мс после ввода.
- `BURST` — preferred + sibling `highest`, 2–4 мс, EPP 0.
- `INTERACTIVE` — preferred `highest`, ещё 2 ядра `nominal`, EPP 32.
- `SUSTAINED` — все `nominal`, EPP 128.
- `THERMAL_GUARD` — cap/низкие частоты, приоритет выше всего.

Псевдокод:

```text
on_hid_event(now):
    last_input = now
    burst_until = now + burst_ms
    wake_policy_thread()

on_tick(now, signals):
    if signals.thermal >= hard_limit:
        return THERMAL_GUARD
    if signals.compile_active:
        return SUSTAINED
    if now < burst_until:
        return BURST
    if signals.frame_pending or now < ui_until:
        return INTERACTIVE
    if now - last_input < ready_ms:
        return IDLE_READY
    return DEEP_IDLE
```

Гистерезис: вверх — сразу, вниз — экспонента τ≈100 мс, не чаще 50 мс. Тепловой guard —
без гистерезиса, мгновенно.

## Бюджет задержки

Цель: **≤2 мс от HID-события до записи CPPC**.

| Участок | Бюджет |
|---|---|
| HID event tap → wake daemon | 0.2–0.5 мс |
| daemon tick + policy | 0.1–0.3 мс |
| `IOConnectCallStructMethod` | 0.01–0.05 мс |
| kext clamp + WRMSR (rendezvous) | 0.1–1 мс |
| SMU ramp | 1–3 мс |

Итого честно: 1.5–5 мс. Поэтому `IDLE_READY` и предиктор обязательны: если стартовать
ровно в момент клика, можно опоздать на первый кадр. HID tap даёт фору, `IDLE_READY` —
ещё немного.

Демон должен жить в `THREAD_TIME_CONSTRAINT_POLICY` (period 1 мс, computation 0.2 мс,
constraint 0.5 мс) или с высоким QoS. Проверить, что это не конфликтует с энергосбережением
и не требует entitlement, которого нет.

## Shadow-режим

Первый запуск — только логирование:

- считать `Decision` на каждом тике;
- писать в CSV: `timestamp, state, urgency, cores, perf, epp, temp, power, frame_ms, actual_freq`;
- ничего не отправлять в kext (кроме `heartbeat`, чтобы watchdog не сработал? Нет: в shadow
  kext вообще не нужен, или нужен только для чтения состояния).
- Сравнить решения с фактическим поведением: где политика хотела буст, а кадр всё равно
  опоздал; где хотела сон, а система дёргалась.

Только после того, как shadow-лог стабильно совпадает с ожиданиями, включать `--apply`.

## CLI (план)

```text
ryzend --shadow --csv /tmp/ryzen.csv
ryzend --apply --config /etc/ryzenpm.json
ryzend --apply --dry-run
ryzend --status
ryzend --reset
```

Конфиг:

```json
{
  "burst_ms": 3,
  "ready_ms": 500,
  "deep_idle_ms": 500,
  "interactive_ms": 500,
  "epp": { "burst": 0, "interactive": 32, "ready": 200, "idle": 255 },
  "thermal": { "cap_c": 90, "hard_c": 98 },
  "battery": { "burst_ms": 2, "max_all_core_perf": "nominal" }
}
```

## TODO

- [ ] `CGEventTap`: какие события слушать (mouse down/up, scroll, key down), как не мешать
      системе, что делать без Accessibility-разрешения.
- [ ] `proc_pid_rusage` для WindowServer: хватает ли прав root, какая реальная стоимость
      вызова на 240 Гц.
- [ ] `IOReport`: какие каналы есть на этой сборке; fallback на kext.
- [ ] `CVDisplayLink`: не создаёт ли лишнюю нагрузку; как получать фактический frame time.
- [ ] `THREAD_TIME_CONSTRAINT_POLICY`: проверить на macOS, не требует ли entitlement.
- [ ] Формат CSV и ротация логов.
- [ ] Конфиг: JSON или plist; hot reload или рестарт.
- [ ] Battery/AC: `IOPSCopyPowerSourcesInfo` / `IOPSGetTimeRemainingEstimate`.
- [ ] Sleep/wake: демон должен перестать спамить kext до его re-init.

## Красные линии

- Не писать в kext, пока не проверен shadow.
- Не отключать watchdog.
- Не ставить `desired_perf` ниже `lowest`/выше `highest`.
- Не трогать thermal guard из демона.
- Не обещать «0 мс» — честный бюджет 1.5–5 мс.
