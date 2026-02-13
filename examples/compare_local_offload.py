#!/usr/bin/env python3
"""Compare local vs RSU offload completion time using this repo's formulas.

Mirrors:
- `src/straight/GymOffloader.cc` local compute: t_cpu = cycles / cpuFreqVehicle
- `src/straight/TaskServer.cc` offload model:
  - Friis path loss, Shannon capacity
  - UL time: t_ul = 8*inputBytes / R_ul
  - RSU CPU time: t_cpu = cycles / cpuFreqRsu
  - DL time: t_dl = 8*outputBytes / R_dl

Notes:
- In `TaskServer.cc`, UL/DL effective bandwidth is divided by the number of active links.
- This script optionally adds a queue/busy delay (seconds) on the RSU side.

Edit the constants under "EXPERIMENT SETTINGS" below, then run:
    python3 examples/compare_local_offload.py
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import List, Sequence


def dbm_to_w(dbm: float) -> float:
    # Matches TaskServer::dbmToW
    return (10.0 ** (dbm / 10.0)) / 1000.0


def friis_path_loss_lin(freq_hz: float, d_m: float) -> float:
    # Matches TaskServer::friisPathLossLin
    c = 299_792_458.0
    if d_m <= 1e-3:
        return 1.0
    wavelength = c / freq_hz
    loss = (4.0 * math.pi * d_m / wavelength) ** 2
    return max(loss, 1.0)


def noise_power_w(bandwidth_hz: float, noise_figure_db: float) -> float:
    # Matches TaskServer::noisePowerW
    n0_mw_per_hz = 10.0 ** (-174.0 / 10.0)
    n_mw = n0_mw_per_hz * bandwidth_hz * (10.0 ** (noise_figure_db / 10.0))
    return n_mw / 1000.0


def shannon_rate(bandwidth_hz: float, snr_lin: float) -> float:
    # Matches TaskServer::shannonRate
    return bandwidth_hz * math.log2(1.0 + max(0.0, snr_lin))


@dataclass(frozen=True)
class ModelParams:
    # Task
    cycles_per_byte: float
    output_factor: float

    # CPU
    cpu_vehicle_hz: float
    cpu_rsu_hz: float

    # RF / link
    bandwidth_hz: float
    carrier_hz: float
    noise_figure_db: float
    tx_power_dbm_vehicle: float
    tx_power_dbm_rsu: float

    # Contention (as in TaskServer.cc)
    ul_active: int
    dl_active: int

    # Optional extra delay on RSU side (queue/busy), seconds
    queue_delay_s: float


@dataclass(frozen=True)
class Result:
    task_mb: float
    distance_m: float

    local_s: float
    offload_s: float

    ul_s: float
    rsu_cpu_s: float
    dl_s: float
    queue_s: float

    r_ul_mbps: float
    r_dl_mbps: float
    snr_ul_db: float
    snr_dl_db: float


def expected_queue_delay_external_busy(busy_on_mean_s: float, busy_off_mean_s: float) -> float:
    """Expected wait until RSU becomes available under an alternating exponential ON/OFF model.

    If ON and OFF durations are exponential with means Ton and Toff:
    - P(arrive during ON) = Ton/(Ton+Toff)
    - Expected residual ON time given ON (memoryless) = Ton
    => E[wait] = Ton^2/(Ton+Toff)

    This is a simplification: it ignores CPU queueing due to other tasks.
    """
    ton = max(0.0, float(busy_on_mean_s))
    toff = max(0.0, float(busy_off_mean_s))
    if ton == 0.0:
        return 0.0
    if ton + toff == 0.0:
        return 0.0
    return (ton * ton) / (ton + toff)


###########################################################
# EXPERIMENT SETTINGS (edit these values)
###########################################################

# Sweep points
TASK_SIZES_MB: List[float] = [1, 10, 100, 1000]
DISTANCES_M: List[float] = [100, 200, 300, 500, 1000]

# Task model (matches `src/straight/GymOffloader.ned` defaults)
CYCLES_PER_BYTE = 100.0
OUTPUT_FACTOR = 0.2

# CPU model
CPU_VEH_HZ = 0.7e9
CPU_RSU_HZ = 10e9

# Link model (matches `src/straight/TaskServer.ned` defaults)
BANDWIDTH_HZ = 10e6
CARRIER_HZ = 5.9e9
NOISE_FIGURE_DB = 9.0
PT_VEH_DBM = 24.0
PT_RSU_DBM = 24.0

# Contention (TaskServer divides bandwidth by active links)
UL_ACTIVE = 1
DL_ACTIVE = 1

# Optional RSU-side queue/busy delay
# - Set QUEUE_DELAY_S directly, OR
# - Set BUSY_ON/OFF means to estimate expected wait under the external busy model.
QUEUE_DELAY_S = 0.0
BUSY_ON_MEAN_S: float | None = None   # e.g. 2.0
BUSY_OFF_MEAN_S: float | None = None  # e.g. 2.0


def compute_times(task_mb: float, distance_m: float, p: ModelParams) -> Result:
    input_bytes = float(task_mb) * 1e6
    output_bytes = float(p.output_factor) * input_bytes
    cycles = float(p.cycles_per_byte) * input_bytes

    # Local
    local_s = cycles / float(p.cpu_vehicle_hz)

    # UL
    loss = friis_path_loss_lin(float(p.carrier_hz), float(distance_m))
    beff_ul = float(p.bandwidth_hz) / max(1, int(p.ul_active))
    n_ul = noise_power_w(beff_ul, float(p.noise_figure_db))
    pr_ul = dbm_to_w(float(p.tx_power_dbm_vehicle)) / loss
    snr_ul = pr_ul / n_ul
    r_ul = shannon_rate(beff_ul, snr_ul)
    ul_s = (8.0 * input_bytes) / max(r_ul, 1e-9)

    # RSU CPU
    rsu_cpu_s = cycles / float(p.cpu_rsu_hz)

    # DL
    beff_dl = float(p.bandwidth_hz) / max(1, int(p.dl_active))
    n_dl = noise_power_w(beff_dl, float(p.noise_figure_db))
    pr_dl = dbm_to_w(float(p.tx_power_dbm_rsu)) / loss
    snr_dl = pr_dl / n_dl
    r_dl = shannon_rate(beff_dl, snr_dl)
    dl_s = (8.0 * output_bytes) / max(r_dl, 1e-9)

    queue_s = max(0.0, float(p.queue_delay_s))
    offload_s = queue_s + ul_s + rsu_cpu_s + dl_s

    return Result(
        task_mb=float(task_mb),
        distance_m=float(distance_m),
        local_s=float(local_s),
        offload_s=float(offload_s),
        ul_s=float(ul_s),
        rsu_cpu_s=float(rsu_cpu_s),
        dl_s=float(dl_s),
        queue_s=float(queue_s),
        r_ul_mbps=float(r_ul) / 1e6,
        r_dl_mbps=float(r_dl) / 1e6,
        snr_ul_db=10.0 * math.log10(max(snr_ul, 1e-300)),
        snr_dl_db=10.0 * math.log10(max(snr_dl, 1e-300)),
    )


def fmt_s(x: float) -> str:
    if x >= 10:
        return f"{x:7.2f}"
    if x >= 1:
        return f"{x:7.3f}"
    return f"{x:7.4f}"


def print_table(results: Sequence[Result]) -> None:
    header = (
        "taskMB dist(m)  local(s) offload(s)  ul(s) rsuCPU(s)  dl(s) queue(s)  "
        "Rul(Mbps) Rdl(Mbps) SNRul(dB)"
    )
    print(header)
    for r in results:
        print(
            f"{r.task_mb:6.2f} {r.distance_m:7.1f}"
            f" {fmt_s(r.local_s)} {fmt_s(r.offload_s)}"
            f" {fmt_s(r.ul_s)} {fmt_s(r.rsu_cpu_s)} {fmt_s(r.dl_s)} {fmt_s(r.queue_s)}"
            f" {r.r_ul_mbps:8.2f} {r.r_dl_mbps:8.2f} {r.snr_ul_db:8.2f}"
        )


def main() -> int:
    queue_delay_s = float(QUEUE_DELAY_S)
    if BUSY_ON_MEAN_S is not None or BUSY_OFF_MEAN_S is not None:
        if BUSY_ON_MEAN_S is None or BUSY_OFF_MEAN_S is None:
            raise ValueError("BUSY_ON_MEAN_S and BUSY_OFF_MEAN_S must be set together")
        queue_delay_s = expected_queue_delay_external_busy(BUSY_ON_MEAN_S, BUSY_OFF_MEAN_S)

    p = ModelParams(
        cycles_per_byte=float(CYCLES_PER_BYTE),
        output_factor=float(OUTPUT_FACTOR),
        cpu_vehicle_hz=float(CPU_VEH_HZ),
        cpu_rsu_hz=float(CPU_RSU_HZ),
        bandwidth_hz=float(BANDWIDTH_HZ),
        carrier_hz=float(CARRIER_HZ),
        noise_figure_db=float(NOISE_FIGURE_DB),
        tx_power_dbm_vehicle=float(PT_VEH_DBM),
        tx_power_dbm_rsu=float(PT_RSU_DBM),
        ul_active=int(UL_ACTIVE),
        dl_active=int(DL_ACTIVE),
        queue_delay_s=float(queue_delay_s),
    )

    results: List[Result] = []
    for d in DISTANCES_M:
        for mb in TASK_SIZES_MB:
            results.append(compute_times(task_mb=mb, distance_m=d, p=p))

    # Sort for readability
    results.sort(key=lambda r: (r.distance_m, r.task_mb))

    print_table(results)

    # Quick summary: where local beats offload
    local_wins = [r for r in results if r.local_s < r.offload_s]
    if local_wins:
        print("\nLocal is faster for:")
        for r in local_wins:
            print(f"  task={r.task_mb:g}MB dist={r.distance_m:g}m (local={r.local_s:.3f}s < offload={r.offload_s:.3f}s)")
    else:
        print("\nOffload is faster for all shown cases.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
