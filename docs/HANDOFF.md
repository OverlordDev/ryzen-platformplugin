# HANDOFF: что написано, что проверено, что сломается первым

Этот файл для того, кто будет **собирать и запускать** проект на macOS (человек или ИИ).
Код v0.2 (актуатор, user client, демон) написан **по памяти, без компилятора macOS и без
железа**. Ниже честно: что реально проверено, что нет, где ломаться будет в первую очередь и
что делать.

## 1. Что проверено, а что нет

| Что | Статус |
|---|---|
| Логика актуатора (`kext/Source/Actuator.cpp`): clamp, read-back, watchdog, TTL, thermal guard, lockout, сон/пробуждение, ABI | **Проверено** на хосте: `make test` → 12 сценариев, все проходят (виртуальные MSR, моки времени/температуры) |
| Синтаксис `kext/Source/*.cpp`, `daemon/ryzend.c`, `tools/ryzenstat.c` | Проверен **только против заглушек** (`kext/test/stub`, ничего общего с настоящими заголовками XNU/IOKit) |
| Модель политики (`sim/policy_sim.py`) | Запускается, цифры в README, но это модель |
| Сборка kext под macOS | **НЕ проверена** |
| Загрузка kext, user client, `ryzend` на macOS | **НЕ проверены** |
| Любое поведение железа (меняет ли запись `CPPC_REQ` частоту) | **НЕ проверено, и это главный риск проекта** |

Заглушки в `kext/test/stub` нельзя использовать как довод, что «настоящая сборка заработает».
Они нужны только чтобы тест логики актуатора запускался без ядра.

## 2. Порядок работы (с условиями остановки)

```bash
make test                       # 0. логика актуатора на хосте; должно быть ALL PASSED
make                            # 1. kext + ryzenstat + ryzend. Дальше пункт 3 (что чинить)
```

Дальше на железе, **строго по порядку**; каждый шаг имеет условие «стоп, дальше не идём»:

1. **Загрузить kext без записи** (без `ryzenplatform_write=1`): `tools/ryzenstat -w 5` в простое.
   Стоп, если: `cppc.enabled` меньше числа ядер, `cap1` из нулей/мусор, `C0%` в простое высокий
   (ядра не спят: частотная политика проблему не лечит), `smn.retries` растёт (SMN делится с
   другим kext'ом), Tctl бессмысленный.
2. `sudo daemon/ryzend --status`: топология, caps, флаги. Стоп, если ABI не совпал или
   `preferred_core`/`perf` выглядят нереально.
3. `sudo daemon/ryzend --shadow --csv /tmp/ryzen.csv` минут 10 за обычной работой. Ничего не
   пишется. Смотри, что решения похожи на здравый смысл.
4. **Включить запись** boot-arg'ом `ryzenplatform_write=1` (через OpenCore `NVRAM → boot-args`;
   запасной EFI и `ryzenplatform=0` под рукой). Перезагрузка.
5. **Контрольный опыт**: в одном терминале `tools/ryzenstat -w 8`, в другом
   `sudo daemon/ryzend --poke 2 low 5`, затем `--poke 2 high 5`. Должно измениться
   **только** `effMHz`/`C0%` у CPU 2 (и, возможно, его SMT-брата). Стоп, если: частота не
   двигается (CPPC это ничего не делает, весь актуатор бессмыслен), двигаются чужие ядра,
   `readback_failures` не 0.
6. `sudo daemon/ryzend --apply --csv /tmp/ryzen.csv`, обычная работа. Затем `kill -9` демона:
   CPU вернутся к значениям прошивки почти сразу (<0.5 с): при смерти процесса user client
закрывается и kext откатывает сам, а если бы не закрылся, сработал бы watchdog на 150 мс.
7. Сон/пробуждение ×20, под нагрузкой и сразу после клика. 8 ч soak. Только потом доверять.

## 3. Где сломается первым (написано по памяти)

Для каждого пункта: что я предположил, как это проверить, запасной вариант.

### Kext

| # | Где | Предположение | Если неверно |
|---|---|---|---|
| K1 | `KernelDecls.hpp`: `wrmsr_carefully(msr, lo, hi)` | Экспортируется и симметричен `rdmsr_carefully` | Ошибка линковки `_wrmsr_carefully`. **Не** заменять на голый `wrmsr`/`wrmsr64` без защиты: #GP в ядре = паника. Искать экспортируемый аналог; крайний вариант: голый wrmsr, но только после успешного `rdmsr_carefully` того же MSR и с проверенным CPUID CPPC. Сказать об этом человеку явно |
| K2 | `KernelDecls.hpp`: `clock_get_uptime`, `absolutetime_to_nanoseconds` | Нужно переобъявить | Если SDK уже объявляет их, будет конфликт типов: удалить мои объявления и включить `<kern/clock.h>` |
| K3 | `MsrIo.hpp` + `RyzenPlatform.cpp`: `mp_rendezvous_no_intrs`, `cpu_number` | Резолвятся (их же использует RyzenHV) | Если нет, это блокер: найти, как делает RyzenHV |
| K4 | `RyzenUserClient.cpp`: `initWithTask(task_t, void*, UInt32, OSDictionary*)`, `externalMethod(...)`, `IOExternalMethodDispatch`, `IOExternalMethodArguments` поля | Классическая раскладка | Сверить с `IOKit/IOUserClient.h` целевого SDK; в новых SDK есть `IOExternalMethodDispatch2022`: не нужен, классика поддерживается |
| K5 | `RyzenPlatform.cpp`: `newUserClient(task_t, void*, UInt32, OSDictionary*, IOUserClient**)` | Пятиаргументная сигнатура | Сверить с `IOService.h`; есть и вариант без `properties` |
| K6 | `RyzenPlatform.cpp`: `IOUserClient::clientHasPrivilege`, `kIOClientPrivilegeAdministrator` | В `IOUserClient.h`/`IOKitKeys.h` | Подключить нужный заголовок. **Не убирать проверку привилегий**: это единственное, что отделяет любого пользователя от `wrmsr` |
| K7 | `libkern`: `strlcpy`, `bzero`, `snprintf` | Есть | Заменить на `strncpy`+ручной нуль-терминатор |
| K8 | `RYZEN_ENABLE_PM` (по умолчанию 0) | `PMinit/joinPMtree/registerPowerDriver` на провайдере `IOResources` может не работать | Не включать, пока всё остальное не работает. Основной механизм сна: демон (`IORegisterForSystemPower`) |
| K9 | `Info.plist` `OSBundleLibraries` | Версий KPI хватает | Подправить версии по ошибке `kmutil`/`kextutil` |
| K10 | Производительность записи | `mp_rendezvous_no_intrs` на каждый apply (одна на **все** ядра за раз) приемлем | Замерить. Если дрожь заметна: таргетный вызов на один CPU (`mp_cpus_call` и родня, проверить экспорт). Это оптимизация, не корректность |

### Демон (`daemon/ryzend.c`)

| # | Где | Предположение | Если неверно |
|---|---|---|---|
| D1 | `hid_install()`: `CGEventTapCreate(kCGSessionEventTap…)` | Работает из терминала под `sudo` при выданном Accessibility / Input Monitoring | Из LaunchDaemon вне GUI-сессии tap не создастся. Демон тогда работает без предиктора (`--no-hid`), это штатный режим. `TODO` в `hid_cb`: переподнимать tap при `kCGEventTapDisabledBy*` |
| D2 | `proc_pid_rusage(pid, RUSAGE_INFO_V2, (rusage_info_t*)&ri)` | Нужен root; поля `ri_user_time/ri_system_time` в mach-единицах | Проверить на реальном SDK; при ошибке `frame_pending` просто всегда false |
| D3 | `pthread_cond_timedwait_relative_np` | Есть в macOS | Заменить на `pthread_cond_timedwait` с абсолютным временем |
| D4 | SMT-соседство `c ^ 1` и «горячее» ядро = `preferred_core` из kext | Потоки одного ядра идут парами | **Проверить**: `--poke N high` и смотреть, какие CPU в `ryzenstat` двигаются. Иначе `--smt-stride 0` / `--hot-cpu` |
| D5 | Нумерация CPU в kext (`cpu_number()`) = логические потоки 0..N-1, плотно | Да (kext проверяет плотность) | Иначе kext откажется стартовать с логом |

## 4. Железные неизвестные (не про компиляцию)

Это то, что **никто не знает**, пока не проверено на машине:

- Включает ли прошивка CPPC (`CPPC_ENABLE` bit 0). **Kext сам его не включает и не будет**:
  если бит выключен, `applyPerf` вернёт `UNSUPPORTED` с понятным сообщением.
- Раскладка `CPPC_REQ` (7:0 max, 15:8 min, 23:16 desired, 31:24 EPP) взята из Linux
  `amd-pstate`. В v0.1 `ZenMsr.h` её путал (max/min наоборот), исправлено; всё равно сверить
  с живым значением `req` в `ryzenstat`: прошивка обычно ставит max=highest, min=lowest.
- Обращает ли SMU на запись внимание вообще. Именно это проверяет `--poke`.
- `desired_perf = 0` означает «SMU решает сам внутри [min, max]»; демон его не использует
  (всегда явный desired), но kext пропускает 0.
- Перевод perf → МГц: kext даёт только оценку (линейную, от P0). Реальную частоту смотреть по
  `effMHz` (APERF/MPERF) в `ryzenstat`; `hw.tscfrequency` на Hackintosh бывает неверным.
- Tctl: бит 19 и смещения сверять с `temp.raw`. Без работающего датчика kext капает потолок на
  nominal (так задумано: «нет данных ≠ всё хорошо»).

## 5. Правила для того, кто будет править

1. **Не ослабляй проверки безопасности, чтобы собралось или чтобы тест прошёл.** Нельзя:
   убирать read-back, clamp, проверку `CPPC_ENABLE`, привилегию user client'а, thermal guard,
   «нет датчика → cap», lockout, boot-arg `ryzenplatform_write`.
2. Нельзя: писать в SMU mailbox, трогать напряжение/Curve Optimizer, включать `CPPC_ENABLE`
   самому, отключать троттлинг.
3. После **любой** правки `Actuator.cpp`: `make test` должен оставаться зелёным. Если тест
   падает, потому что поведение намеренно изменено, объясни это явно, не подгоняй молча.
4. Если API не такое, как я написал, **сначала** сверься с заголовками целевого SDK и с тем,
   как это делают рабочие kext'ы (RyzenHV, SMCAMDProcessor), а не угадывай дальше.
5. Не расширяй права user client'а за пределы `shared/RyzenPlatformAPI.h` (никакого «записать
   произвольный MSR/SMN»).
6. Любая найденная проблема на железе → записать в этот файл, раздел 6.

## 6. Журнал сборки/запуска (заполнять по ходу)

| Дата | Шаг | Результат / что пришлось поправить |
|---|---|---|
| | | |

## 7. Что осознанно не сделано

- `effective_freq_mhz` и `package_power_mw` в `ryzen_core_state_t` не заполняются (нули):
  данные есть в IORegistry (`aperf/mperf/tsc`), считает `ryzenstat`.
- Нет launchd-плиста, нет установщика, нет подписи.
- Нет `NSWorkspace`-сигнала (нужен Objective-C), нет батареи/AC, нет `IOReport`.
- Нет hot-reload конфига; параметры только флагами.
- Sleep/wake в kext через PM-дерево выключен по умолчанию (K8).
