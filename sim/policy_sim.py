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

    smu_tau_ms: float = 1.5      # постоянная времени перехода P-state
    throttle_temp: float = 95.0  # начало теплового троттлинга
    shutdown_temp: float = 105.0

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

    def notify_input(self, now_ms: float) -> None:
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
        p = self.p
        n = p.n_cores
        low = [p.f_min_ghz] * n
        nominal = [p.f_nom_ghz] * n
        maxed = [p.f_max_ghz] * n

        # Тепловой предохранитель: он важнее любой хотелки демона.
        if die_temp >= p.throttle_temp:
            return Decision("THERMAL_GUARD", low, 255)

        # Фоновая компиляция: все ядра на nominal, EPP посередине.
        if compile_active:
            return Decision("SUSTAINED", nominal, 128)

        # Пользователь только что нажал: короткий выстрел по 1-2 ядрам.
        if now_ms < self.burst_until_ms:
            t = low.copy()
            t[0] = p.f_max_ghz
            t[1] = p.f_max_ghz
            return Decision("BURST", t, 0)

        # Идёт отрисовка/взаимодействие: держим «горячую» пару ядер.
        if now_ms < self.ui_until_ms or (frame_pending and self.predict):
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
        # Пользователь «трогает» систему: окно активности ~1.5 с.
        self.active_until_ms = now_ms + 1_500.0

    def tick(self, now_ms: float) -> None:
        # Новый кадр каждые 1000/frame_hz мс, только пока пользователь активен.
        period = 1000.0 / self.p.frame_hz
        if now_ms < self.active_until_ms and now_ms % period < self.p.dt_ms:
            self.frames.append(
                Frame(
                    spawn_ms=now_ms,
                    deadline_ms=now_ms + period,
                    work_ms=self.p.frame_work_ms,
                )
            )
            self.frames_spawned += 1

        # Компиляция стартует один раз.
        if (self.compile_enabled and not self.compile_started
                and now_ms >= self.p.compile_start_ms):
            self.compile_started = True
            self.compile_remaining_ms = self.p.compile_work_ms

    @property
    def compile_active(self) -> bool:
        return self.compile_started and self.compile_remaining_ms > 0

    def pending_frame(self, now_ms: float) -> bool:
        return any(f.done_ms is None and f.deadline_ms >= now_ms for f in self.frames)

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
            if f.work_ms <= 0 and f.done_ms is None:
                f.done_ms = now_ms
            if f.done_ms is None and now_ms >= f.deadline_ms:
                missed += 1
                f.done_ms = now_ms  # кадр опоздал, дальше не тащим

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
        self.f_eff = [p.f_min_ghz] * p.n_cores
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

    def step(self, dt_ms: float, targets: List[float]) -> float:
        p = self.p
        dt_s = dt_ms / 1000.0
        alpha = 1.0 - math.exp(-dt_ms / p.smu_tau_ms)

        for i in range(p.n_cores):
            self.f_eff[i] += (targets[i] - self.f_eff[i]) * alpha

        # Тепловой троттлинг: если кристалл перегрелся, SMU режет частоты.
        if self.t_die > p.throttle_temp:
            k = max(0.35, 1.0 - (self.t_die - p.throttle_temp) / 15.0)
            self.f_eff = [f * k for f in self.f_eff]

        p_core = sum(self.core_power(f) for f in self.f_eff)
        self.power_w = p_core + p.p_static

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
        elif policy_name == "race_no_predict":
            # Без HID-предиктора: буст только когда кадр уже начал рисоваться.
            if wl.pending_frame(now):
                t = [p.f_min_ghz] * p.n_cores
                t[0] = p.f_max_ghz
                t[1] = p.f_nom_ghz
                dec = Decision("INTERACTIVE", t, 32)
            else:
                dec = pol.decide(now, cpu.t_die, False, wl.compile_active)
        else:
            dec = pol.decide(
                now,
                cpu.t_die,
                wl.pending_frame(now),
                wl.compile_active,
            )

        power = cpu.step(p.dt_ms, dec.target_ghz)
        total_power += power
        energy_j += power * p.dt_ms / 1000.0
        max_temp = max(max_temp, cpu.t_die)

        wl.process(now, p.dt_ms, cpu.f_eff)
        for f in wl.frames[:]:
            if f.done_ms is not None and f.spawn_ms <= now:
                latencies.append(f.done_ms - f.spawn_ms)
                wl.frames.remove(f)

        if wl.compile_done_ms is not None and compile_done_ms is None:
            compile_done_ms = wl.compile_done_ms

        now += p.dt_ms

    latencies.sort()
    p95 = latencies[int(0.95 * (len(latencies) - 1))] if latencies else 0.0

    return {
        "policy": policy_name,
        "avg_w": total_power / (p.duration_ms / p.dt_ms),
        "max_c": max_temp,
        "miss_pct": 100.0 * wl.frames_missed / max(1, wl.frames_spawned),
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
    args = ap.parse_args()

    p = Params(duration_ms=args.duration)
    rows = [run(name, p, args.seed, not args.no_clicks, not args.no_compile)
            for name in ("max", "low", "race_no_predict", "race")]

    print()
    print(f"Модель: {p.n_cores} ядер, {p.f_min_ghz}-{p.f_max_ghz} ГГц, "
          f"{p.frame_hz:.0f} Hz, {args.duration / 1000:.0f} c, seed={args.seed}")
    print()
    header = f"{'policy':<16}{'avg W':>8}{'max °C':>8}{'miss %':>9}{'p95 ms':>9}{'J':>9}{'compile s':>11}"
    print(header)
    print("-" * len(header))
    for r in rows:
        comp = "—" if math.isnan(r["compile_s"]) else f"{r['compile_s']:.2f}"
        print(f"{r['policy']:<16}{r['avg_w']:>8.1f}{r['max_c']:>8.1f}"
              f"{r['miss_pct']:>9.1f}{r['p95_ms']:>9.1f}{r['energy_j']:>9.0f}{comp:>11}")
    print()
    print("Напоминание: цифры — модель, а не замер. Калибровать по package power,")
    print("температуре и реальным frame times на конкретном ноутбуке.")
    print()


if __name__ == "__main__":
    main()
