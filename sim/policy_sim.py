#!/usr/bin/env python3
"""
policy_sim.py — иллюстративная модель «race-to-sleep» для Ryzen 7 5800H.

Это НЕ замена измерениям на железе. Это песочница, чтобы до первого WRMSR
проверить, не противоречит ли политика физике:

  * двухузловая тепловая модель (кристалл -> радиатор -> воздух);
  * первичный порядок перехода SMU по частоте (tau ~ 1.5 мс);
  * кубическая (точнее, V^2 * f) модель динамической мощности;
  * дедлайн-ориентированная политика вместо «всегда максимум» / «всегда минимум».

Запуск:
    python3 sim/policy_sim.py
    python3 sim/policy_sim.py --duration 20 --seed 7
"""

from __future__ import annotations

import argparse
import math
import random
from dataclasses import dataclass, field
from typing import Dict, List, Tuple


# ---------------------------------------------------------------------------
# Параметры железа (приближены к 5800H, калибруются по реальным замерам)
# ---------------------------------------------------------------------------

@dataclass
class Params:
    dt_ms: float = 1.0
    duration_ms: int = 60_000

    n_cores: int = 8
    f_min_ghz: float = 0.8
    f_nom_ghz: float = 3.2
    f_max_ghz: float = 4.4

    v_min: float = 0.65
    v_max: float = 1.35
    c_dyn: float = 0.70          # Вт на (ГГц * В^2) на ядро
    p_static: float = 2.0        # Вт, утечки + SoC

    # Тепло: кристалл -> радиатор -> воздух
    c_die: float = 0.5           # Дж/К
    r_die_hs: float = 0.20       # К/Вт
    c_hs: float = 25.0           # Дж/К
    r_hs_amb: float = 1.20       # К/Вт
    t_amb: float = 25.0

    # 4.4 ГГц на всех ядрах не бывает: пакет упирается в PPT, и SMU равномерно
    # снижает частоты. Реальный all-core получается из этого лимита, а не задан числом.
    ppt_w: float = 45.0

    smu_tau_ms: float = 1.5      # постоянная времени перехода P-state
    throttle_temp: float = 95.0  # АППАРАТНЫЙ троттлинг SMU (политика его не контролирует)
    shutdown_temp: float = 105.0

    # Тепловой guard нашей политики (docs/SAFETY.md §6): срабатывает раньше SMU.
    guard_cap_c: float = 90.0      # >= : потолок nominal
    guard_hard_c: float = 98.0     # >= : все ядра lowest
    guard_release_c: float = 85.0  # гистерезис: снимаем cap, только когда остыло ниже

    # Сколько HID-событие «опережает» первый кадр (docs/TZ.md §3.2: 1-3 мс).
    hid_to_frame_ms: float = 3.0
    # Через сколько мс после появления кадра демон БЕЗ HID-предиктора его заметит:
    # опрос proc_pid_rusage(WindowServer) на 120-240 Гц + пробуждение (docs/TZ.md §3.1).
    # Это допущение, не замер. При 0 предиктор в модели бесполезен (см. --observe-ms).
    frame_observe_ms: float = 4.0
    # Цель режима SUSTAINED; None -> f_max_ghz (просим highest, PPT режет сам).
    # Задай f_nom_ghz, чтобы увидеть цену «экономии» (см. --sustained nominal).
    sustained_ghz: float | None = None

    frame_hz: float = 120.0
    frame_work_ms: float = 2.5   # работа кадра при F_MAX
    click_mean_s: float = 1.4
    compile_start_ms: int = 5_000
    compile_work_ms: float = 8 * 1_800.0
    compile_deadline_ms: int = 4_000


# ---------------------------------------------------------------------------
# Политика
# ---------------------------------------------------------------------------

@dataclass
class Decision:
    name: str
    # Целевая частота каждого ядра, ГГц
    target_ghz: List[float]
    # EPP: 0 = performance, 255 = power
    epp: int


class Policy:
    """Дедлайн-ориентированная политика. Одна и та же логика потом уедет в C++."""

    def __init__(self, p: Params, predict: bool = True):
        self.p = p
        self.predict = predict
        self.last_event_ms = -10_000
        self.ui_until_ms = -10_000
        self.burst_until_ms = -10_000
        self.state = "DEEP_IDLE"
        self.guard_active = False

    def notify_input(self, now_ms: float) -> None:
        # Без предиктора политика не видит HID: о активности она узнаёт только
        # по уже появившемуся кадру (см. decide()).
        if not self.predict:
            return
        self.last_event_ms = now_ms
        self.ui_until_ms = now_ms + 1_500
        self.burst_until_ms = now_ms + 4

    def decide(
        self,
        now_ms: float,
        die_temp: float,
        frame_pending: bool,
        compile_active: bool,
    ) -> Decision:
        """Тепловой guard стоит над всем: он режет решение, а не заменяет его,
        чтобы после остывания политика вернулась ровно туда, где была."""
        p = self.p
        dec = self._decide(now_ms, frame_pending, compile_active)

        if die_temp >= p.guard_hard_c:
            self.guard_active = True
            return Decision("THERMAL_GUARD", [p.f_min_ghz] * p.n_cores, 255)

        if die_temp >= p.guard_cap_c or (self.guard_active and die_temp > p.guard_release_c):
            self.guard_active = True
            capped = [min(t, p.f_nom_ghz) for t in dec.target_ghz]
            return Decision("THERMAL_GUARD", capped, 255)

        self.guard_active = False
        return dec

    def _decide(
        self,
        now_ms: float,
        frame_pending: bool,
        compile_active: bool,
    ) -> Decision:
        p = self.p
        n = p.n_cores
        low = [p.f_min_ghz] * n
        nominal = [p.f_nom_ghz] * n

        # Фоновая компиляция: просим highest на всех ядрах, EPP посередине; потолок
        # ставит PPT, а не мы. nominal здесь стоил бы ~35% времени сборки за ~4%
        # энергии (--sustained nominal): выбор, а не умолчание.
        if compile_active:
            f = p.sustained_ghz if p.sustained_ghz is not None else p.f_max_ghz
            return Decision("SUSTAINED", [f] * n, 128)

        # Кадр уже рисуется — активность видна и без предиктора. Продлеваем окно
        # UI, чтобы режим не мигал между кадрами.
        if frame_pending:
            self.last_event_ms = now_ms
            self.ui_until_ms = max(self.ui_until_ms, now_ms + 1_500)

        # Пользователь только что нажал: короткий выстрел по 1-2 ядрам.
        if now_ms < self.burst_until_ms:
            t = low.copy()
            t[0] = p.f_max_ghz
            t[1] = p.f_max_ghz
            return Decision("BURST", t, 0)

        # Идёт отрисовка/взаимодействие: держим «горячую» пару ядер.
        if now_ms < self.ui_until_ms or frame_pending:
            t = low.copy()
            t[0] = p.f_max_ghz
            t[1] = p.f_nom_ghz
            t[2] = p.f_nom_ghz
            return Decision("INTERACTIVE", t, 32)

        # Только что закончилось взаимодействие: одно ядро на nominal,
        # чтобы следующий клик не ждал полного разгона SMU.
        if now_ms - self.last_event_ms < 500:
            t = low.copy()
            t[0] = p.f_nom_ghz
            return Decision("IDLE_READY", t, 200)

        return Decision("DEEP_IDLE", low, 255)


# ---------------------------------------------------------------------------
# Нагрузка
# ---------------------------------------------------------------------------

@dataclass
class Frame:
    spawn_ms: float
    deadline_ms: float
    work_ms: float
    done_ms: float | None = None
    cold: bool = False  # первый кадр после простоя: то, ради чего нужен предиктор


@dataclass
class Workload:
    p: Params
    rng: random.Random
    clicks: List[float] = field(default_factory=list)
    frames: List[Frame] = field(default_factory=list)
    active_until_ms: float = -1.0
    frames_spawned: int = 0
    frames_missed: int = 0
    compile_started: bool = False
    compile_remaining_ms: float = 0.0
    compile_done_ms: float | None = None
    clicks_enabled: bool = True
    compile_enabled: bool = True
    next_frame_ms: float = -1.0
    next_is_cold: bool = False
    cold_spawned: int = 0
    cold_missed: int = 0

    def __post_init__(self) -> None:
        if not self.clicks_enabled:
            return
        t = 300.0
        while t < self.p.duration_ms:
            t += self.rng.expovariate(1.0 / (self.p.click_mean_s * 1000.0))
            if t < self.p.duration_ms:
                self.clicks.append(t)

    def click_at(self, now_ms: float) -> bool:
        return any(abs(now_ms - c) < 0.5 for c in self.clicks)

    def on_click(self, now_ms: float) -> None:
        # Пользователь «трогает» систему: окно активности ~1.5 с. Первый кадр
        # появляется не сразу, а через hid_to_frame_ms: это и есть фора предиктора.
        was_idle = now_ms >= self.active_until_ms
        self.active_until_ms = now_ms + 1_500.0
        if was_idle:
            self.next_frame_ms = now_ms + self.p.hid_to_frame_ms
            self.next_is_cold = True

    def tick(self, now_ms: float) -> None:
        # Новый кадр каждые 1000/frame_hz мс, только пока пользователь активен.
        period = 1000.0 / self.p.frame_hz
        if now_ms < self.active_until_ms and now_ms >= self.next_frame_ms:
            self.next_frame_ms += period
            self.frames.append(
                Frame(
                    spawn_ms=now_ms,
                    deadline_ms=now_ms + period,
                    work_ms=self.p.frame_work_ms,
                    cold=self.next_is_cold,
                )
            )
            self.frames_spawned += 1
            if self.next_is_cold:
                self.cold_spawned += 1
                self.next_is_cold = False

        # Компиляция стартует один раз.
        if (self.compile_enabled and not self.compile_started
                and now_ms >= self.p.compile_start_ms):
            self.compile_started = True
            self.compile_remaining_ms = self.p.compile_work_ms

    @property
    def compile_active(self) -> bool:
        return self.compile_started and self.compile_remaining_ms > 0

    def pending_frame(self, now_ms: float, min_age_ms: float = 0.0) -> bool:
        """Есть ли кадр в работе, который наблюдатель уже мог заметить (возраст >= min_age_ms)."""
        return any(
            f.done_ms is None and f.deadline_ms >= now_ms and now_ms - f.spawn_ms >= min_age_ms
            for f in self.frames
        )

    def process(
        self,
        now_ms: float,
        dt_ms: float,
        f_eff: List[float],
    ) -> Tuple[int, float]:
        """Возвращает (число проваленных кадров, работа компиляции за тик)."""
        p = self.p
        missed = 0

        # UI-кадры выполняются на ядре 0 (упрощение: один рендер-поток).
        rate0 = max(f_eff[0], 0.05) / p.f_max_ghz
        for f in self.frames:
            if f.done_ms is not None:
                continue
            if f.work_ms > 0:
                done = min(f.work_ms, rate0 * dt_ms)
                f.work_ms -= done
            late = False
            if f.work_ms <= 0:
                f.done_ms = now_ms
                # Тик покрывает [now, now+dt): кадр, доделанный в нём, готов не раньше
                # конца тика. Раньше опоздавший кадр получал done_ms до проверки
                # дедлайна и промахом не считался.
                late = now_ms + dt_ms > f.deadline_ms
            elif now_ms >= f.deadline_ms:
                f.done_ms = now_ms  # кадр опоздал, дальше не тащим
                late = True
            if late:
                missed += 1
                if f.cold:
                    self.cold_missed += 1

        self.frames_missed += missed

        # Компиляция использует все ядра.
        compile_done = 0.0
        if self.compile_active:
            total_rate = sum(max(f, 0.05) / p.f_max_ghz for f in f_eff)
            compile_done = min(self.compile_remaining_ms, total_rate * dt_ms)
            self.compile_remaining_ms -= compile_done
            if self.compile_remaining_ms <= 0:
                self.compile_done_ms = now_ms

        return missed, compile_done


# ---------------------------------------------------------------------------
# Модель CPU + тепла
# ---------------------------------------------------------------------------

class CPUModel:
    def __init__(self, p: Params):
        self.p = p
        self.f_eff = [p.f_min_ghz] * p.n_cores      # что запросили, с учётом перехода SMU
        self.f_applied = list(self.f_eff)           # что реально получили (после PPT/троттлинга)
        self.t_die = p.t_amb
        self.t_hs = p.t_amb
        self.power_w = 0.0

    def voltage(self, f_ghz: float) -> float:
        p = self.p
        x = max(0.0, min(1.0, (f_ghz - p.f_min_ghz) / (p.f_max_ghz - p.f_min_ghz)))
        return p.v_min + (p.v_max - p.v_min) * (x ** 1.6)

    def core_power(self, f_ghz: float) -> float:
        v = self.voltage(f_ghz)
        return self.p.c_dyn * (v ** 2) * f_ghz

    def _package_power(self, freqs: List[float]) -> float:
        return sum(self.core_power(f) for f in freqs) + self.p.p_static

    def step(self, dt_ms: float, targets: List[float]) -> float:
        p = self.p
        dt_s = dt_ms / 1000.0
        alpha = 1.0 - math.exp(-dt_ms / p.smu_tau_ms)

        for i in range(p.n_cores):
            self.f_eff[i] += (targets[i] - self.f_eff[i]) * alpha

        # Аппаратные ограничения SMU считаем от запрошенных частот и не пишем
        # обратно в f_eff, иначе множитель копился бы на каждом тике.
        f = list(self.f_eff)
        if self.t_die > p.throttle_temp:
            k = max(0.35, 1.0 - (self.t_die - p.throttle_temp) / 15.0)
            f = [x * k for x in f]

        # PPT: если пакет не влезает в лимит, SMU равномерно снижает частоты.
        if self._package_power(f) > p.ppt_w:
            lo, hi = 0.0, 1.0
            for _ in range(14):
                mid = (lo + hi) / 2
                trial = [max(p.f_min_ghz, x * mid) for x in f]
                if self._package_power(trial) > p.ppt_w:
                    hi = mid
                else:
                    lo = mid
            f = [max(p.f_min_ghz, x * lo) for x in f]

        self.f_applied = f
        self.power_w = self._package_power(f)

        # Два узла: C_die dT_die/dt = P - (T_die - T_hs)/R_die_hs
        #           C_hs  dT_hs/dt  = (T_die - T_hs)/R_die_hs - (T_hs - T_amb)/R_hs_amb
        q_die_hs = (self.t_die - self.t_hs) / p.r_die_hs
        self.t_die += (self.power_w - q_die_hs) / p.c_die * dt_s
        self.t_hs += (q_die_hs - (self.t_hs - p.t_amb) / p.r_hs_amb) / p.c_hs * dt_s
        return self.power_w


# ---------------------------------------------------------------------------
# Прогон
# ---------------------------------------------------------------------------

def run(
    policy_name: str,
    p: Params,
    seed: int,
    clicks: bool = True,
    compile_on: bool = True,
) -> Dict[str, float]:
    rng = random.Random(seed)
    wl = Workload(p, rng, clicks_enabled=clicks, compile_enabled=compile_on)
    cpu = CPUModel(p)
    predict = policy_name != "race_no_predict"
    pol = Policy(p, predict=predict)

    total_power = 0.0
    max_temp = p.t_amb
    latencies: List[float] = []
    cold_lat: List[float] = []
    energy_j = 0.0
    compile_done_ms: float | None = None

    now = 0.0
    while now < p.duration_ms:
        if wl.click_at(now):
            wl.on_click(now)
            pol.notify_input(now)

        wl.tick(now)

        if policy_name == "max":
            dec = Decision("MAX", [p.f_max_ghz] * p.n_cores, 0)
        elif policy_name == "low":
            dec = Decision("LOW", [p.f_min_ghz] * p.n_cores, 255)
        else:
            # race и race_no_predict идут одним путём; различие целиком внутри
            # Policy (predict=False игнорирует HID и реагирует на готовый кадр).
            # С предиктором кадры предсказаны по HID/CVDisplayLink; без него о кадре
            # узнаём только после опроса WindowServer.
            min_age = 0.0 if predict else p.frame_observe_ms
            dec = pol.decide(
                now,
                cpu.t_die,
                wl.pending_frame(now, min_age),
                wl.compile_active,
            )

        power = cpu.step(p.dt_ms, dec.target_ghz)
        total_power += power
        energy_j += power * p.dt_ms / 1000.0
        max_temp = max(max_temp, cpu.t_die)

        wl.process(now, p.dt_ms, cpu.f_applied)
        for f in wl.frames[:]:
            if f.done_ms is not None and f.spawn_ms <= now:
                latencies.append(f.done_ms - f.spawn_ms)
                if f.cold:
                    cold_lat.append(f.done_ms - f.spawn_ms)
                wl.frames.remove(f)

        if wl.compile_done_ms is not None and compile_done_ms is None:
            compile_done_ms = wl.compile_done_ms

        now += p.dt_ms

    latencies.sort()
    mean_lat = sum(latencies) / len(latencies) if latencies else 0.0
    p95 = latencies[int(0.95 * (len(latencies) - 1))] if latencies else 0.0

    return {
        "policy": policy_name,
        "avg_w": total_power / (p.duration_ms / p.dt_ms),
        "max_c": max_temp,
        "miss_pct": 100.0 * wl.frames_missed / max(1, wl.frames_spawned),
        "mean_ms": mean_lat,
        "cold_ms": sum(cold_lat) / len(cold_lat) if cold_lat else 0.0,
        "cold_miss_pct": 100.0 * wl.cold_missed / max(1, wl.cold_spawned),
        "p95_ms": p95,
        "energy_j": energy_j,
        "compile_s": (compile_done_ms - p.compile_start_ms) / 1000.0
        if compile_done_ms
        else float("nan"),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=60_000, help="мс")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--no-clicks", action="store_true", help="сценарий «читаю текст»")
    ap.add_argument("--no-compile", action="store_true", help="без фоновой компиляции")
    ap.add_argument("--click-mean", type=float, default=None,
                    help="средний интервал между кликами, с (по умолчанию 1.4; больше = больше холодных стартов)")
    ap.add_argument("--observe-ms", type=float, default=None,
                    help="задержка, с которой демон без предиктора замечает кадр (по умолчанию 4)")
    ap.add_argument("--sustained", choices=("max", "nominal"), default="max",
                    help="цель режима SUSTAINED: highest (PPT режет сам) или nominal")
    args = ap.parse_args()

    p = Params(duration_ms=args.duration)
    if args.click_mean is not None:
        p.click_mean_s = args.click_mean
    if args.observe_ms is not None:
        p.frame_observe_ms = args.observe_ms
    if args.sustained == "nominal":
        p.sustained_ghz = p.f_nom_ghz
    rows = [run(name, p, args.seed, not args.no_clicks, not args.no_compile)
            for name in ("max", "low", "race_no_predict", "race")]

    print()
    print(f"Модель: {p.n_cores} ядер, {p.f_min_ghz}-{p.f_max_ghz} ГГц, "
          f"{p.frame_hz:.0f} Hz, {args.duration / 1000:.0f} c, seed={args.seed}")
    print()
    header = (f"{'policy':<16}{'avg W':>8}{'max °C':>8}{'miss %':>9}{'p95 ms':>9}"
              f"{'1st ms':>9}{'1st miss%':>11}{'J':>9}{'compile s':>11}")
    print(header)
    print("-" * len(header))
    for r in rows:
        comp = "—" if math.isnan(r["compile_s"]) else f"{r['compile_s']:.2f}"
        print(f"{r['policy']:<16}{r['avg_w']:>8.1f}{r['max_c']:>8.1f}"
              f"{r['miss_pct']:>9.1f}{r['p95_ms']:>9.1f}{r['cold_ms']:>9.2f}"
              f"{r['cold_miss_pct']:>11.1f}{r['energy_j']:>9.0f}{comp:>11}")
    print()
    print("Напоминание: цифры — модель, а не замер. Калибровать по package power,")
    print("температуре и реальным frame times на конкретном ноутбуке.")
    print()


if __name__ == "__main__":
    main()
