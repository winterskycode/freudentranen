#!/usr/bin/env python3
"""Device monitor: GPU clock, GPU busy, thermal status, hottest CPU and GPU sensors, fan and battery, sampled on the
device every second (one adb shell, no per-sample round trips), written as TSV. Judges what a benchmark run cannot: the
GPU clock the thermal state leaves, and the power the run draws (the Odin discharges while it plays on USB power).

  port/android/tools/devmon.py <out.tsv> [--seconds N] [--interval S]     stop with Ctrl-C or when the seconds are up
  port/android/tools/devmon.py --summary <out.tsv>                         averages, and per-minute GPU clock and temps

Columns: t (s since start), gpu_mhz, gpu_busy (%), thermal (status 0-6), cpu_c (hottest cpu zone), gpu_c (hottest gpuss
zone), skin_c (the thermal service's skin sensor), fan_pct, fan_rpm, batt_pct, batt_ma (current_now; negative =
discharging), batt_mv.
"""
import argparse, os, signal, subprocess, sys

# Empty means adb's only device.
SERIAL = os.environ.get('ANDROID_SERIAL', '')
# one shell loop on the device: every sample is one line
SCRIPT = r'''
interval=%s; n=%s
zones=$(for z in /sys/class/thermal/thermal_zone*; do t=$(cat $z/type); case $t in cpu-*|gpuss-*|skin*) echo "$z:$t";; esac; done)
i=0; t0=$(date +%%s)
while [ $n -le 0 ] || [ $i -lt $n ]; do
  now=$(date +%%s); cpu=0; gpu=0; skin=0
  for zt in $zones; do z=${zt%%%%:*}; t=${zt#*:}; v=$(cat $z/temp 2>/dev/null || echo 0); v=$((v/1000))
    case $t in cpu-*) [ $v -gt $cpu ] && cpu=$v;; gpuss-*) [ $v -gt $gpu ] && gpu=$v;; skin*) skin=$v;; esac; done
  ts=$(dumpsys thermalservice 2>/dev/null); th=$(echo "$ts" | sed -n 's/.*Thermal Status: \([0-9]*\).*/\1/p' | head -1)
  s=$(echo "$ts" | sed -n 's/.*mValue=\([0-9]*\)\.[0-9]*, mType=[0-9]*, mName=skin.*/\1/p' | head -1); [ -n "$s" ] && skin=$s
  f=/sys/class/gpio5_pwm2
  echo "$((now-t0))	$(( $(cat /sys/class/kgsl/kgsl-3d0/gpuclk) / 1000000 ))	$(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage | tr -d ' %%')	${th:--1}	$cpu	$gpu	$skin	$(( $(cat $f/duty) * 100 / $(cat $f/period) ))	$(cat $f/speed)	$(cat /sys/class/power_supply/battery/capacity)	$(( $(cat /sys/class/power_supply/battery/current_now) / 1000 ))	$(( $(cat /sys/class/power_supply/battery/voltage_now) / 1000 ))"
  i=$((i+1)); sleep $interval
done
'''
HEADER = 't\tgpu_mhz\tgpu_busy\tthermal\tcpu_c\tgpu_c\tskin_c\tfan_pct\tfan_rpm\tbatt_pct\tbatt_ma\tbatt_mv\n'


def monitor(out, seconds, interval):
    with open(out, 'w') as f:
        f.write(HEADER); f.flush()
        p = subprocess.Popen(['adb', *(['-s', SERIAL] if SERIAL else []), 'shell', SCRIPT % (interval, seconds)], stdout=f)
        try:
            p.wait()
        except KeyboardInterrupt:
            p.send_signal(signal.SIGINT); p.wait()


def summary(path):
    rows = [l.rstrip('\n').split('\t') for l in open(path)][1:]
    rows = [r for r in rows if len(r) == 12 and r[0].isdigit()]
    if not rows:
        print('no samples'); return
    cols = list(zip(*[[float(x) for x in r] for r in rows]))
    names = HEADER.strip().split('\t')
    avg = {n: sum(c) / len(c) for n, c in zip(names, cols)}
    print(f"{len(rows)} samples over {cols[0][-1]:.0f} s")
    print(f"GPU {avg['gpu_mhz']:.0f} MHz avg, busy {avg['gpu_busy']:.0f}%; thermal {min(cols[3]):.0f}..{max(cols[3]):.0f}; "
          f"CPU {avg['cpu_c']:.0f} C avg / {max(cols[4]):.0f} max, GPU {avg['gpu_c']:.0f} C avg / {max(cols[5]):.0f} max; "
          f"fan {avg['fan_pct']:.0f}% avg; battery {cols[9][0]:.0f} -> {cols[9][-1]:.0f}%, {avg['batt_ma']:.0f} mA avg "
          f"({avg['batt_ma'] * avg['batt_mv'] / 1e6:.2f} W)")
    print('minute  gpu_mhz  busy%  thermal  cpu_c  gpu_c  fan%  batt_ma')
    for m in range(int(cols[0][-1] // 60) + 1):
        sel = [r for r in rows if m * 60 <= int(r[0]) < (m + 1) * 60]
        if not sel: continue
        c = list(zip(*[[float(x) for x in r] for r in sel]))
        mean = lambda i: sum(c[i]) / len(c[i])
        print(f"{m:6d}  {mean(1):7.0f}  {mean(2):5.0f}  {max(c[3]):7.0f}  {mean(4):5.0f}  {mean(5):5.0f}  {mean(7):4.0f}  {mean(10):7.0f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('out')
    ap.add_argument('--seconds', type=int, default=0, help='stop after N seconds (default: until Ctrl-C)')
    ap.add_argument('--interval', type=float, default=1.0)
    ap.add_argument('--summary', action='store_true', help='summarise an existing TSV instead of recording')
    a = ap.parse_args()
    if a.summary: summary(a.out)
    else: monitor(a.out, a.seconds, a.interval)


if __name__ == '__main__':
    main()
