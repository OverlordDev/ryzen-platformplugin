# kext: карта классов и что нужно проверить на железе

> Это дизайн-карта, а не готовый Xcode-проект. Код актуатора пишется после этапа 0
> (замеры) и после проверки API на конкретной сборке macOS. Всё, что ниже, — контракт
> ответственности, а не «скомпилируется как есть».

## Зачем вообще kext

Из userspace нельзя сделать `wrmsr`/`rdmsr` и нельзя надёжно читать SMN. Демон может
принимать решения, но рукой, которая пишет в CPPC, должен быть kext.

## Классы

### `RyzenPlatformPlugin : IOService`

- `start(IOService *provider)`:
  - проверить boot arg `-ryzenpmoff` (через `PE_parse_boot_argn`; проверить доступность в
    целевом SDK);
  - проверить CPU: Family 19h, Model 50h (Cezanne). Если не совпало — лог и `return false`;
  - найти host bridge / root complex для SMN (позже — для сенсоров);
  - прочитать CPPC caps, проверить support/enable;
  - создать `RyzenCPPC`, `RyzenWatchdog`, зарегистрировать `RyzenUserClient`;
  - подписаться на sleep/wake через `IOPMrootDomain`/`registerPowerDriver`.
- `stop(IOService *provider)`:
  - `resetAuto()`, остановить watchdog, снять регистрации.
- `newUserClient(...)`:
  - создать `RyzenUserClient`, проверить entitlement/root (для локального проекта — root).

### `RyzenCPPC`

- `readTopology(ryzen_topology_t *)`:
  - core count, SMT siblings, preferred core;
  - `CAP1/CAP2`: lowest/nominal/highest perf;
  - частоты: из `_CPC`/ACPI, если доступны, иначе из known table Cezanne (проверить).
- `readState(core, ryzen_core_state_t *)`:
  - текущий `CPPC_REQ` (read-back), `CPPC_STATUS`;
  - effective frequency через `APERF/MPERF`;
  - temp/power — из `RyzenSensors` или заглушка.
- `apply(const ryzen_apply_t *)`:
  - для каждого запроса: clamp, записать `CPPC_REQ` **на целевом CPU**;
  - read-back; при расхождении — `RYZEN_E_READBACK`, отключить override;
  - TTL: если `ttl_ms > 0`, по истечении вернуть auto для этого ядра.
- `resetAuto()`:
  - `max = highest`, `min = lowest`, `desired = nominal`, `EPP = 128`;
  - снять per-core override, остановить TTL-таймеры.
- `setCppcEnabled(bool)`:
  - `MSR_AMD_CPPC_ENABLE` bit 0; проверить `CPPC_STATUS`.

**Критично:** MSR — per-core. Нельзя писать «в среднем по больнице». Нужен rendezvous на
каждый CPU (`mp_rendezvous_no_intrs` или аналог; проверить, что экспортируется и не запрещён
на целевой macOS). Если API недоступен — искать другой путь, а не писать с чужого ядра.

### `RyzenSensors` (v0.1 — опционально, fallback)

- SMN read через root complex (config space 0x60/0x64 — как в Linux `amd_nb.c`; проверить
  device/offset на конкретной машине).
- Температура Cezanne: смещение брать из `k10temp`/`SMCAMDProcessor`, не угадывать.
- Package power: если есть MSR/DFLL-путь — использовать; иначе `IOReport`/`pmset` в демоне.
- Все чтения — с таймаутом и проверкой валидности; «мусор» не должен попадать в thermal guard.

### `RyzenWatchdog`

- `IOTimerEventSource` на workloop, период 50 мс.
- `heartbeat(sequence)`: обновить `last_seen`, `missed = 0`.
- `tick()`: если `now - last_seen > 150 мс` → `resetAuto()`, флаг `WATCHDOG`, лог.
- Не останавливается из userspace. Только `stop()` kext / boot arg.
- На sleep — остановить; на wake — запустить заново после первого heartbeat.

### `RyzenUserClient : IOUserClient`

- `externalMethod(selector, args, result)`:
  - проверить `abi_version` и `struct_size`;
  - `getTopology` / `readState` / `applyPerf` / `resetAuto` / `heartbeat` / `getLastError`;
  - вернуть `kern_return_t` + структуру; ошибки — в `ryzen_error_t`.
- Ограничить размеры входа/выхода; не доверять `count` из userspace.
- Не давать доступ к произвольному MSR/SMN. Только те операции, что описаны ABI.

## Info.plist (что проверить)

- `CFBundleIdentifier`: `com.example.RyzenPlatformPlugin` (заменить на свой).
- `OSBundleLibraries`: `com.apple.kpi.iokit`, `com.apple.kpi.libkern`, `com.apple.kpi.mach`,
  `com.apple.kpi.bsd`; добавить `com.apple.kpi.unsupported`, если нужен `mp_rendezvous`.
- `IOKitPersonalities`:
  - v0.1: сервисный kext на `IOResources` (`IOProviderClass = IOResources`,
    `IOClass = RyzenPlatformPlugin`, `IOProbeScore = 1000`);
  - host bridge ищем сами через `IOPCIDevice` (vendor `0x1022`, class `0x060000`).
  - Альтернатива — матчиться на `pci1022,14b5`/конкретный host bridge, но это привязывает
    kext к одной модели. Начать с `IOResources` + runtime-поиск.
- `OSBundleRequired`: `Root`? Для локального проекта — да, но проверить, не блокирует ли
  загрузку при отсутствии kext.

## Сборка

- Xcode + `Kernel.framework` из SDK целевой macOS.
- `ARCHS = x86_64`, `MACOSX_DEPLOYMENT_TARGET` = версия macOS.
- Локально: SIP off, AMFI off, kext signing off (OpenCore `csr-active-config`, `amfi=0x80`).
- Подпись/notarization — вне v0.1.
- Всегда держать запасной EFI на USB и boot arg `-ryzenpmoff`.

## TODO / reverse-engineer

- [ ] Точный `newUserClient` signature в целевом SDK.
- [ ] Доступность `mp_rendezvous_no_intrs`/`ml_*` для per-CPU MSR.
- [ ] CPPC caps на Cezanne: реальные `lowest/nominal/highest perf` и частоты.
- [ ] `CPPC_REQ` layout: проверить биты на живой машине (Linux `amd_pstate` — референс).
- [ ] `CPPC_ENABLE`: кто его включает — firmware, kext, macOS? Не отбирать управление у SMU
      вслепую.
- [ ] SMN offsets для temp/power именно Cezanne (не Renoir, не Vermeer).
- [ ] Sleep/wake notifications: `IOPMrootDomain` vs `registerPowerDriver`.
- [ ] Boot arg API: `PE_parse_boot_argn` доступен? Если нет — `OSBoolean` из device tree?
- [ ] Что делает AMD Vanilla с XCPM на этой сборке: какие MSR уже патчатся/игнорируются.
- [ ] Не конфликтует ли с `SMCAMDProcessor`/`AMDRyzenCPUPowerManagement` за SMN.

## Красные линии

- Не писать в SMU mailbox.
- Не трогать voltage/CO.
- Не отключать thermal throttling.
- Не оставлять watchdog выключенным.
- Не грузить kext без возможности выключить его boot arg'ом.
